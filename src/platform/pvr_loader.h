/* pvr_loader.h — PVR texture file loader for Swordigo Desktop
 *
 * Loads PVR v2/v3 files containing ETC1 compressed textures.
 * Decodes ETC1 → RGBA and uploads to an OpenGL texture.
 *
 * Usage:
 *   GLuint tex = pvr_load_texture("path/to/file.pvr");
 *   if (tex) glBindTexture(GL_TEXTURE_2D, tex);
 */

#ifndef PVR_LOADER_H
#define PVR_LOADER_H

#include "platform/gl_inc.h"
#include <stdint.h>

/* Load a PVR file and return a GL texture ID. Returns 0 on failure.
 * Supports PVR v2 (44-byte header) and v3 (52-byte header).
 * Decodes ETC1 compressed pixel data to RGBA8.
 * Also returns width/height if out pointers are non-null. */
#include <vector>

/* Decode a PVR or gzipped .tex file buffer directly to RGBA8888.
 * Handles automatic gzip decompression.
 * Returns true on success. */
bool pvr_decode_to_rgba(const uint8_t* file_data, size_t file_size, std::vector<uint8_t>& rgba_out, int& width, int& height);
bool pvr_upload_compressed_to_tex(const uint8_t* file_data, size_t file_size, GLuint tex_name, int& out_w, int& out_h);

extern bool g_pvr_software_decode;

GLuint pvr_load_texture(const char* path, int* out_width = nullptr, int* out_height = nullptr);

#endif /* PVR_LOADER_H */
