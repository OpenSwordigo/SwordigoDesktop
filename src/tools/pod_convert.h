#pragma once
// pod_convert.h — FBX → game POD converter (+ texture encoding).
//
// Converts a model exported as an .fbx into a game-native .POD (PowerVR POD
// chunk format, see pod_writer.h) so modders can drop new models into the game
// and load them like any stock POD. It reuses the preview pipeline's FBX
// import (tools/fbx_import.cpp) and the batch converter's native ETC1 texture
// encoder so conversions behave exactly like their in-app counterparts:
//
//   - geometry + normals come from av::fbx_load (centered, unit-cube scaled),
//   - UVs are flipped (v → 1-v) because the game samples its POD UVs with
//     v = 0 at the BOTTOM of the texture, whereas FBX/glTF UVs have v = 0 at
//     the TOP (see comments in av/fbx_import.cpp),
//   - referenced textures are re-encoded to the game's primary .pvr container
//     (raw PVR v3 ETC1) — or with --tex-png a gzipped native TEX .tex-topng for
//     backgrounds — vertically flipped + premultiplied exactly as
//     batch_converter.cpp::do_import_file does for the game asset pipeline.
//
// Standalone module. Depends on tools/fbx_import.cpp + tools/pod_writer.cpp
// (which pull in pod_loader + ufbx) and zlib + stb_image.

#include <string>
#include <vector>

namespace av {

struct PodConvertOptions {
    // FBX UVs (v = 0 at top) → game POD UVs (v = 0 at bottom). Disable only
    // for FBX files whose author already authored bottom-up UVs.
    bool flip_v = true;

    // Re-encode every resolved diffuse texture next to the output POD and point
    // the POD at those files. Engine: raw PVR v3 ETC1 (.pvr) — the game's main
    // model texture container; the game uploads these via glCompressedTexImage2D.
    bool convert_textures = true;

    // When true: "<stem>.pvr" (raw PVR v3 ETC1 — the game's model-texture
    // format). When false: "<stem>.tex_topng" (gzipped native TEX container —
    // the format backgrounds use).
    bool output_pvr = true;

    // Overwrite existing output POD / texture files.
    bool overwrite = false;

    // Uniform scale factor applied to geometry, node translations, and animation.
    float scale = 1.0f;

    // FBX unit handling. ufbx converts the authored scene to meters at load
    // (target_unit_meters=1.0), so an FBX authored in centimetres is silently
    // divided by 100. `unit_scale` is an additional multiplier the user applies
    // on top of that conversion to reach game units (1 unit ≈ 1 inch ≈ 0.0254 m,
    // ≈ 39.4 units / metre). Default 1.0 = keep metres as-is.
    // Common values: 1.0 (source already metres), 100.0 (source in cm), 39.37 (m→in).
    float unit_scale = 1.0f;

    // Target PVR texture resolution (0 = original source resolution, 512, 1024 [HD], 2048 [Ultra HD], 4096).
    int pvr_resolution = 0;
};

// Convert an FBX into a game POD. On success the newly written game textures
// (relative paths) are appended to @p written_textures (if provided). Returns
// true and leaves *err untouched on success; returns false + reason on failure.
bool fbx_to_pod(const std::string& fbx_path,
                const std::string& pod_path,
                const PodConvertOptions& opts,
                std::vector<std::string>* written_textures = nullptr,
                std::string* err = nullptr);

// Convert a GLB/glTF into a game POD (with Swordigo-compatible bone naming,
// embedded/referenced texture extraction to .pvr, and clip extraction).
bool glb_to_pod(const std::string& glb_path,
                const std::string& pod_path,
                const PodConvertOptions& opts,
                std::vector<std::string>* written_textures = nullptr,
                std::vector<std::string>* written_clips = nullptr,
                std::string* err = nullptr);

// CLI entry point driven by `bin/ruby --fbx2pod …` or `bin/ruby --glb2pod …`.
int pod_convert_cli(int argc, char** argv);

} // namespace av