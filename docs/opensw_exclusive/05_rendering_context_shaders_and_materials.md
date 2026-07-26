# Swordigo OpenSwordigo Research: Rendering Context, Shaders & Material Systems

## 1. Overview of the Rendering Engine

The graphics pipeline in OpenSwordigo abstracts OpenGL ES 2.0 / OpenGL 3.3 hardware state through `Caver::RenderingContext` and `Caver::ShaderProgram`. It manages shader uniforms, vertex attribute array bindings, MVP matrix stacks, texture unit slots, and GLES2 state caching (depth testing, blending, cull face).

---

## 2. GLES2 Shader Program Abstraction

```cpp
#pragma once
#include <string>
#include "caver/graphics/gl_inc.h"
#include "caver/math/caver_math.h"

namespace Caver {

class ShaderProgram {
public:
    ShaderProgram() : m_program_id(0) {}
    ~ShaderProgram() {
        if (m_program_id) glDeleteProgram(m_program_id);
    }

    bool CompileFromSource(const std::string& vert_src, const std::string& frag_src) {
        GLuint vert = CompileShader(GL_VERTEX_SHADER, vert_src);
        GLuint frag = CompileShader(GL_FRAGMENT_SHADER, frag_src);
        if (!vert || !frag) return false;

        m_program_id = glCreateProgram();
        glAttachShader(m_program_id, vert);
        glAttachShader(m_program_id, frag);
        glLinkProgram(m_program_id);

        glDeleteShader(vert);
        glDeleteShader(frag);
        return true;
    }

    void Bind() const { glUseProgram(m_program_id); }
    void Unbind() const { glUseProgram(0); }

    void SetUniformMatrix4(const char* name, const Matrix4& mat) {
        GLint loc = glGetUniformLocation(m_program_id, name);
        if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, mat.Data());
    }

    void SetUniformVector4(const char* name, const Vector4& vec) {
        GLint loc = glGetUniformLocation(m_program_id, name);
        if (loc >= 0) glUniform4f(loc, vec.x, vec.y, vec.z, vec.w);
    }

    void SetUniformInt(const char* name, int value) {
        GLint loc = glGetUniformLocation(m_program_id, name);
        if (loc >= 0) glUniform1i(loc, value);
    }

private:
    GLuint m_program_id;

    GLuint CompileShader(GLenum type, const std::string& src) {
        GLuint shader = glCreateShader(type);
        const char* c_src = src.c_str();
        glShaderSource(shader, 1, &c_src, nullptr);
        glCompileShader(shader);
        return shader;
    }
};

} // namespace Caver
```

---

## 3. Rendering Context & State Cache

```cpp
#pragma once
#include <stack>
#include <memory>
#include <unordered_map>
#include "caver/graphics/shader_program.h"
#include "caver/math/caver_math.h"

namespace Caver {

class RenderingContext {
public:
    RenderingContext() {
        m_model_matrix.push(Matrix4::Identity());
        m_view_matrix = Matrix4::Identity();
        m_projection_matrix = Matrix4::Identity();
    }

    void SetViewport(int x, int y, int width, int height) {
        glViewport(x, y, width, height);
    }

    void PushModelMatrix() {
        m_model_matrix.push(m_model_matrix.top());
    }

    void PopModelMatrix() {
        if (m_model_matrix.size() > 1) {
            m_model_matrix.pop();
        }
    }

    void Translate(const Vector3& offset) {
        m_model_matrix.top() = m_model_matrix.top() * Matrix4::Translate(offset);
    }

    void RotateZ(float angle_deg) {
        m_model_matrix.top() = m_model_matrix.top() * Matrix4::RotateZ(angle_deg);
    }

    void Scale(const Vector3& scale) {
        m_model_matrix.top() = m_model_matrix.top() * Matrix4::Scale(scale);
    }

    void SetProjectionMatrix(const Matrix4& proj) { m_projection_matrix = proj; }
    void SetViewMatrix(const Matrix4& view) { m_view_matrix = view; }

    Matrix4 GetMVP() const {
        return m_projection_matrix * m_view_matrix * m_model_matrix.top();
    }

    void BindTextureByName(const std::string& tex_name, uint32_t slot = 0) {
        glActiveTexture(GL_TEXTURE0 + slot);
        auto it = m_textures.find(tex_name);
        if (it != m_textures.end()) {
            glBindTexture(GL_TEXTURE_2D, it->second);
        }
    }

    void DrawFullscreenQuad() {
        // Issue quad draw call using active shader program
    }

private:
    std::stack<Matrix4> m_model_matrix;
    Matrix4 m_view_matrix;
    Matrix4 m_projection_matrix;

    std::unordered_map<std::string, GLuint> m_textures;
};

} // namespace Caver
```

---

## 4. Standard GLES2 Shader Definitions

### Vertex Shader (`caver_default.vert`)
```glsl
attribute vec3 a_position;
attribute vec2 a_texcoord;
attribute vec4 a_color;

uniform mat4 u_mvp;
uniform vec2 u_uv_offset;

varying vec2 v_texcoord;
varying vec4 v_color;

void main() {
    v_texcoord = a_texcoord + u_uv_offset;
    v_color = a_color;
    gl_Position = u_mvp * vec4(a_position, 1.0);
}
```

### Fragment Shader (`caver_default.frag`)
```glsl
precision mediump float;

varying vec2 v_texcoord;
varying vec4 v_color;

uniform sampler2D u_texture;
uniform vec4 u_tint_color;

void main() {
    vec4 tex_color = texture2D(u_texture, v_texcoord);
    gl_FragColor = tex_color * v_color * u_tint_color;
}
```
