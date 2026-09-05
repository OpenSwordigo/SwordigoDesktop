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
// `flip_v` flips the V coordinate (v = 1 - v) so exported GLBs display textures
// correctly in standard DCC tools (Blender, etc.), which expect top-origin UVs.
// POD UVs are bottom-origin (v = 0 at bottom), so flip_v defaults to true.
bool gltf_export_glb(const PODModel& model,
                     const std::vector<GLTFTextureImage>& images,
                     const std::string& output_path,
                     std::string* err = nullptr,
                     bool flip_v = true);

// A texture image extracted from a GLB (PNG or JPEG payload) ready for
// conversion back into game texture formats (e.g. .pvr / .tex.png).
struct GLTFImageBuffer {
    std::string mime;            // "image/png" | "image/jpeg"
    std::vector<uint8_t> data;
};

// PBR material data recovered from the glTF materials (parallel to
// PODModel::materials). Texture slots reference images by glTF image index
// (see GLTFPBRInfo::image_gltf_index); -1 = no map. metallic-roughness is a
// single combined texture per glTF (B = metalness, G = roughness); the caller
// swizzles channels when building GL textures.
struct GLTFPBRMaterial {
    float base_color[4] = {1, 1, 1, 1};
    float metallic = 1.0f;
    float roughness = 1.0f;
    float occlusion = 1.0f;
    float emissive[3] = {0, 0, 0};
    int base_tex = -1;             // glTF image index
    int metalrough_tex = -1;       // glTF image index (combined B/G)
    int normal_tex = -1;           // glTF image index
    int occl_tex = -1;             // glTF image index
    int emissive_tex = -1;         // glTF image index
    float normal_scale = 1.0f;
    float alpha_cutoff = 0.5f;
    int alpha_mode = 0;            // 0 opaque, 1 mask, 2 blend
    bool double_sided = false;
};

// Full PBR material info + the image payloads the slots reference.
struct GLTFPBRInfo {
    struct Image {
        std::string mime;               // "image/png" | "image/jpeg"
        std::vector<uint8_t> data;
    };
    std::vector<GLTFPBRMaterial> materials;   // parallel to PODModel::materials
    std::vector<int> image_gltf_index;        // glTF image index of each payload
    std::vector<Image> images;                // deduped payloads (referenced only)
};

// Parse a GLB file into a PODModel. Embedded images are returned in `images`
// (in material/texture order). When @p pbr is non-null it is filled with the
// PBR material data (factors, map image indices, and the referenced payloads).
// Returns true on success.
bool gltf_import_glb(const std::string& path,
                     PODModel& out,
                     std::vector<GLTFImageBuffer>& images,
                     std::string* err = nullptr,
                     GLTFPBRInfo* pbr = nullptr,
                     float scale = 1.0f);

// Parse a bare .gltf JSON file into a PODModel (same outputs as the GLB
// importer). External resources are resolved relative to the .gltf file:
// buffer 0 URIs (data: base64 or a .bin file) and image URIs (data: base64
// or image files). PBR info fills exactly as in gltf_import_glb.
bool gltf_import_gltf(const std::string& path,
                      PODModel& out,
                      std::vector<GLTFImageBuffer>& images,
                      std::string* err = nullptr,
                      GLTFPBRInfo* pbr = nullptr,
                      float scale = 1.0f);

// Extract all animation clips inside a .glb / .gltf as individual PODModel
// objects (each containing the skeleton nodes + keyframed animation streams
// formatted as Swordigo clip PODs).
bool gltf_import_all_clips(const std::string& path,
                           std::vector<std::pair<std::string, PODModel>>& out_clips,
                           std::string* err = nullptr,
                           float scale = 1.0f);

} // namespace av
