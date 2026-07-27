// ======================= DRAW CALL BATCHER =======================
// Batches consecutive GLES 1.x draw calls with the same state (texture, blend)
// into single large VBO-backed draws. Reduces ~100 draws/frame to ~20.
//
// Architecture:
//   1. Game calls glVertexPointer/glTexCoordPointer/glColorPointer → state recorded
//   2. Game calls glDrawArrays → vertices transformed by current MVP, appended to batch
//   3. When texture/blend changes or frame ends → batch flushed as single glDrawArrays
//
// Unified vertex format: [pos.x pos.y pos.z] [tex.s tex.t] [col.r col.g col.b col.a]
//                         9 floats = 36 bytes per vertex

#ifndef DRAW_BATCHER_H
#define DRAW_BATCHER_H

#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES 1
#endif
#include <SDL3/SDL_opengl.h>
#include <cstring>
#include <cstdint>
#include <iostream>

// Max vertices per frame in the batch buffer (4MB / 36 bytes = ~116k verts)
static constexpr int BATCH_MAX_VERTS = 116000;
static constexpr int BATCH_FLOATS_PER_VERT = 9; // xyz + st + rgba
static constexpr int BATCH_STRIDE = BATCH_FLOATS_PER_VERT * sizeof(float); // 36 bytes

struct BatchState {
    GLuint texture_id;
    GLenum blend_src, blend_dst;
    bool blend_enabled;
    bool alpha_test_enabled;
    bool color_array_enabled;
    
    bool operator==(const BatchState& o) const {
        return texture_id == o.texture_id &&
               blend_enabled == o.blend_enabled &&
               (!blend_enabled || (blend_src == o.blend_src && blend_dst == o.blend_dst)) &&
               alpha_test_enabled == o.alpha_test_enabled;
    }
    bool operator!=(const BatchState& o) const { return !(*this == o); }
};

struct DrawBatcher {
    // Vertex buffer: interleaved [pos3 tex2 col4] per vertex
    float* buffer = nullptr;  // CPU-side staging
    int vertex_count = 0;
    
    // GPU VBO
    GLuint vbo = 0;
    bool initialized = false;
    
    // Current batch state
    BatchState current_state = {};
    bool has_batch = false;
    
    // Stats
    int batched_draws = 0;
    int flushed_draws = 0;
    int total_input_draws = 0;
    
    // Current GL matrices (captured for CPU-side transform)
    float modelview[16];
    float projection[16];
    float mvp[16]; // pre-computed MVP
    bool mvp_dirty = true;
    
    void init() {
        if (initialized) return;
        buffer = new float[BATCH_MAX_VERTS * BATCH_FLOATS_PER_VERT];
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        // Allocate 4MB VBO with GL_STREAM_DRAW hint
        glBufferData(GL_ARRAY_BUFFER, BATCH_MAX_VERTS * BATCH_STRIDE, nullptr, GL_STREAM_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        
        // Identity matrices
        memset(modelview, 0, sizeof(modelview));
        memset(projection, 0, sizeof(projection));
        modelview[0] = modelview[5] = modelview[10] = modelview[15] = 1.0f;
        projection[0] = projection[5] = projection[10] = projection[15] = 1.0f;
        
        initialized = true;
    }
    
    void destroy() {
        if (vbo) { glDeleteBuffers(1, &vbo); vbo = 0; }
        delete[] buffer; buffer = nullptr;
        initialized = false;
    }
    
    // Matrix multiply: out = a * b (column-major 4x4)
    static void mat4_mul(float* out, const float* a, const float* b) {
        float tmp[16];
        for (int c = 0; c < 4; c++) {
            for (int r = 0; r < 4; r++) {
                tmp[c*4+r] = a[0*4+r]*b[c*4+0] + a[1*4+r]*b[c*4+1] + 
                             a[2*4+r]*b[c*4+2] + a[3*4+r]*b[c*4+3];
            }
        }
        memcpy(out, tmp, sizeof(tmp));
    }
    
    // Transform a 3D point by the current MVP matrix
    void transform_point(float x, float y, float z, float& ox, float& oy, float& oz) {
        if (mvp_dirty) {
            mat4_mul(mvp, projection, modelview);
            mvp_dirty = false;
        }
        float w = mvp[3]*x + mvp[7]*y + mvp[11]*z + mvp[15];
        if (w == 0.0f) w = 1.0f;
        ox = (mvp[0]*x + mvp[4]*y + mvp[8]*z  + mvp[12]) / w;
        oy = (mvp[1]*x + mvp[5]*y + mvp[9]*z  + mvp[13]) / w;
        oz = (mvp[2]*x + mvp[6]*y + mvp[10]*z + mvp[14]) / w;
    }
    
    void set_modelview(const float* m) {
        memcpy(modelview, m, 16 * sizeof(float));
        mvp_dirty = true;
    }
    
    void set_projection(const float* m) {
        memcpy(projection, m, 16 * sizeof(float));
        mvp_dirty = true;
    }
    
    // Append vertices to the batch. Transforms positions by current MVP.
    // vertex_data: raw interleaved data from guest memory
    // Returns false if batch is full
    bool append(const uint8_t* memory,
                uint32_t vptr, GLint vsize, GLsizei vstride,
                uint32_t tptr, GLint tsize, GLsizei tstride,
                uint32_t cptr, GLsizei cstride, bool has_colors,
                GLenum draw_mode, int first, int count) {
        
        // Convert TRIANGLE_STRIP to TRIANGLES
        int out_count;
        if (draw_mode == GL_TRIANGLE_STRIP && count >= 3) {
            out_count = (count - 2) * 3;
        } else if (draw_mode == GL_TRIANGLES) {
            out_count = count;
        } else if (draw_mode == GL_TRIANGLE_FAN && count >= 3) {
            out_count = (count - 2) * 3;
        } else {
            return false; // Can't batch this draw mode
        }
        
        if (vertex_count + out_count > BATCH_MAX_VERTS) return false;
        
        // Calculate actual strides
        if (vstride == 0) vstride = vsize * sizeof(float);
        if (tstride == 0) tstride = tsize * sizeof(float);
        if (cstride == 0) cstride = 4; // 4 bytes for RGBA
        
        float* dst = buffer + vertex_count * BATCH_FLOATS_PER_VERT;
        
        auto emit_vertex = [&](int idx) {
            // Position (transform by MVP)
            const float* vp = (const float*)(memory + vptr + (first + idx) * vstride);
            float px, py, pz;
            transform_point(vp[0], vp[1], vsize >= 3 ? vp[2] : 0.0f, px, py, pz);
            *dst++ = px;
            *dst++ = py;
            *dst++ = pz;
            
            // TexCoord
            if (tptr) {
                const float* tp = (const float*)(memory + tptr + (first + idx) * tstride);
                *dst++ = tp[0];
                *dst++ = tsize >= 2 ? tp[1] : 0.0f;
            } else {
                *dst++ = 0.0f;
                *dst++ = 0.0f;
            }
            
            // Color
            if (has_colors && cptr) {
                const uint8_t* cp = memory + cptr + (first + idx) * cstride;
                *dst++ = cp[0] / 255.0f;
                *dst++ = cp[1] / 255.0f;
                *dst++ = cp[2] / 255.0f;
                *dst++ = cp[3] / 255.0f;
            } else {
                *dst++ = 1.0f;
                *dst++ = 1.0f;
                *dst++ = 1.0f;
                *dst++ = 1.0f;
            }
        };
        
        if (draw_mode == GL_TRIANGLES) {
            for (int i = 0; i < count; i++) {
                emit_vertex(i);
            }
        } else if (draw_mode == GL_TRIANGLE_STRIP) {
            for (int i = 0; i < count - 2; i++) {
                if (i % 2 == 0) {
                    emit_vertex(i);
                    emit_vertex(i + 1);
                    emit_vertex(i + 2);
                } else {
                    emit_vertex(i + 1);
                    emit_vertex(i);
                    emit_vertex(i + 2);
                }
            }
        } else if (draw_mode == GL_TRIANGLE_FAN) {
            for (int i = 1; i < count - 1; i++) {
                emit_vertex(0);
                emit_vertex(i);
                emit_vertex(i + 1);
            }
        }
        
        vertex_count += out_count;
        batched_draws++;
        return true;
    }
    
    // Flush the current batch to GPU
    void flush() {
        if (vertex_count == 0) return;
        
        // Upload to VBO
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, vertex_count * BATCH_STRIDE, buffer);
        
        // Save GL state
        glPushAttrib(GL_ALL_ATTRIB_BITS);
        glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
        
        // Set identity matrices (vertices are pre-transformed)
        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadIdentity();
        
        // Disable lighting (colors are baked)
        glDisable(GL_LIGHTING);
        
        // Set up vertex arrays from VBO
        glEnableClientState(GL_VERTEX_ARRAY);
        glVertexPointer(3, GL_FLOAT, BATCH_STRIDE, (void*)0);
        
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glTexCoordPointer(2, GL_FLOAT, BATCH_STRIDE, (void*)(3 * sizeof(float)));
        
        glEnableClientState(GL_COLOR_ARRAY);
        glColorPointer(4, GL_FLOAT, BATCH_STRIDE, (void*)(5 * sizeof(float)));
        
        // Apply batch state
        glBindTexture(GL_TEXTURE_2D, current_state.texture_id);
        if (current_state.blend_enabled) {
            glEnable(GL_BLEND);
            glBlendFunc(current_state.blend_src, current_state.blend_dst);
        } else {
            glDisable(GL_BLEND);
        }
        if (current_state.alpha_test_enabled) {
            glEnable(GL_ALPHA_TEST);
        } else {
            glDisable(GL_ALPHA_TEST);
        }
        
        // Draw everything in one call!
        glDrawArrays(GL_TRIANGLES, 0, vertex_count);
        flushed_draws++;
        
        // Restore GL state
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
        glMatrixMode(GL_MODELVIEW);
        glPopMatrix();
        glPopClientAttrib();
        glPopAttrib();
        
        vertex_count = 0;
        has_batch = false;
    }
    
    // Try to batch a draw call. Returns true if batched, false if must draw immediately.
    bool try_batch(const BatchState& state, const uint8_t* memory,
                   uint32_t vptr, GLint vsize, GLsizei vstride,
                   uint32_t tptr, GLint tsize, GLsizei tstride,
                   uint32_t cptr, GLsizei cstride, bool has_colors,
                   GLenum draw_mode, int first, int count) {
        
        if (!initialized) return false;
        
        // Can't batch non-triangle modes
        if (draw_mode != GL_TRIANGLES && draw_mode != GL_TRIANGLE_STRIP && 
            draw_mode != GL_TRIANGLE_FAN) {
            flush(); // flush pending, draw this one directly
            return false;
        }
        
        // State changed → flush previous batch
        if (has_batch && state != current_state) {
            flush();
        }
        
        current_state = state;
        has_batch = true;
        total_input_draws++;
        
        return append(memory, vptr, vsize, vstride, tptr, tsize, tstride,
                      cptr, cstride, has_colors, draw_mode, first, count);
    }
    
    void reset_stats() {
        batched_draws = 0;
        flushed_draws = 0;
        total_input_draws = 0;
    }
};

#endif // DRAW_BATCHER_H
