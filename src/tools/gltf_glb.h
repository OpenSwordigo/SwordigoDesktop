#pragma once
// gltf_glb.h — glTF 2.0 / GLB round-trip for POD assets
//
// gltf_export_glb(): serialize a PODModel (mesh, skin, animation, materials)
// into a self-contained .glb (JSON + binary chunks, textures embedded as PNG).
// gltf_import_glb(): parse a .glb back into a PODModel so edits made in
// Blender can be written back to .pod + .pvr game assets.
//
// Standalone modules. No OpenGL, ImGui, Blender or external JSON dependency.

#include <cstdint>
#include <string>
#include <vector>

namespace av {

struct PODModel;

// RGBA texture for embedding into the GLB. `name` matches an entry in
// PODModel::texture_filenames (bare filename, no directory).
struct GLTFTextureImage {
    std::string name;
    std::vector<uint8_t> rgba;
    int w = 0;
    int h = 0;
};

// Serialize `model` to `output_path` as a GLB file. `images` are the textures
// referenced by the model; missing entries simply leave materials untextured.
bool gltf_export_glb(const PODModel& model,
                     const std::vector<GLTFTextureImage>& images,
                     const std::string& output_path,
                     std::string* err = nullptr);

// A texture image extracted from a GLB (PNG or JPEG payload) ready for
// conversion back into game texture formats (e.g. .pvr / .tex.png).
struct GLTFImageBuffer {
    std::string mime;            // "image/png" | "image/jpeg"
    std::vector<uint8_t> data;
};

// Parse a GLB file into a PODModel. Embedded images are returned in `images`
// (in material/texture order). Returns true on success.
bool gltf_import_glb(const std::string& path,
                     PODModel& out,
                     std::vector<GLTFImageBuffer>& images,
                     std::string* err = nullptr);

} // namespace av
