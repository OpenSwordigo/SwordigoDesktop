#pragma once
// scn_loader.h — zauonlok/renderer .scn text scenes.
//
// Format (see scenes/scene_helper.c in the vendored reference):
//   type: blinn | pbrm | pbrs
//   lighting:
//       background: r g b
//       environment: <name|null>
//       skybox: off|on|ambient|blurred
//       shadow: off|on|<W>x<H>
//       ambient: f
//       punctual: f
//   materials N:
//       material i:  (fields depend on type — blinn/pbrm/pbrs)
//   transforms N:
//       transform i: 16 floats (row-major in the file)
//   models N:
//       model i: mesh: <obj|null> skeleton: <ani|null> attached: d material: i transform: i
//
// Material field sets (parsed per kind):
//   blinn: basecolor rgba, shininess, diffuse_map, specular_map, emission_map,
//          double_sided, enable_blend, alpha_cutoff
//   pbrm : basecolor_factor rgba, metalness_factor, roughness_factor,
//          basecolor_map, metalness_map, roughness_map, normal_map,
//          occlusion_map, emission_map, double_sided, enable_blend, alpha_cutoff
//   pbrs : diffuse_factor rgba, specular_factor rgb, glossiness_factor,
//          diffuse_map, specular_map, glossiness_map, normal_map,
//          occlusion_map, emission_map, double_sided, enable_blend, alpha_cutoff
//
// Asset paths inside the file are resolved against base_dir, falling back to
// base_dir's parent (the demo scenes live in a subfolder yet reference
// "azura/... " style paths relative to the assets root).
#include <string>
#include <vector>

namespace av {

struct ScnMaterial {
    int kind = 0;   // 0 blinn, 1 pbrm (metallic-roughness), 2 pbrs (spec-gloss)
    float base_color[4] = {1, 1, 1, 1};
    float shininess = 32.0f;
    float metalness = 1.0f, roughness = 1.0f;
    float specular[3] = {0.04f, 0.04f, 0.04f};
    float glossiness = 1.0f;
    // raw names as written
    std::string diffuse_map, specular_map, emission_map;
    std::string basecolor_map, metalness_map, roughness_map, normal_map, occlusion_map;
    std::string glossiness_map;
    bool double_sided = false, enable_blend = false;
    float alpha_cutoff = 0.0f;
    // resolved absolute paths (empty when absent/null)
    std::string diffuse_res, specular_res, emission_res;
    std::string basecolor_res, metalness_res, roughness_res, normal_res, occlusion_res;
    std::string glossiness_res;
};

struct ScnTransform { float m[16]; };   // column-major

struct ScnModelRef {
    int material = -1;
    int transform = -1;
    int attached = 0;
    std::string mesh_path, skeleton_path;     // as written
    std::string mesh_res, skeleton_res;       // resolved absolute ("" = none)
};

struct ScnScene {
    std::string type = "blinn";
    float background[3] = {0.15f, 0.15f, 0.15f};
    std::string environment;
    std::string skybox = "off";
    std::string shadow = "off";
    float ambient = 0.5f;
    float punctual = 0.7f;
    std::vector<ScnMaterial> materials;
    std::vector<ScnTransform> transforms;
    std::vector<ScnModelRef> models;
};

bool scn_load(const std::string& path, const std::string& base_dir,
              ScnScene& out, std::string* err = nullptr);

} // namespace av
