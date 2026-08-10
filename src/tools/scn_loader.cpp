// scn_loader.cpp — .scn text-scene parser.
//
// The reference (scenes/scene_helper.c) reads the file with fscanf in a fixed
// field order. We replicate that order with a whitespace token stream, which
// is robust to the file's line breaks and matches the reference exactly.
// Transforms are stored row-major in the file and transposed to column-major
// here (av::mat4 convention).

#include "scn_loader.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace av {
namespace {

bool file_exists(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return static_cast<bool>(f);
}

// Resolve an asset path: base_dir/path, else parent(base_dir)/path (demo
// scenes sit in subfolders but reference "azura/... " paths), else base best
// effort. "null" / empty → "".
std::string resolve_path(const std::string& base_dir, const std::string& raw) {
    if (raw.empty() || raw == "null") return "";
    if (!raw.empty() && raw[0] == '/') return raw;
    std::string joined = base_dir + "/" + raw;
    if (file_exists(joined)) return joined;
    size_t slash = base_dir.find_last_of('/');
    if (slash != std::string::npos) {
        std::string parent = base_dir.substr(0, slash);
        std::string joined2 = parent + "/" + raw;
        if (file_exists(joined2)) return joined2;
    }
    return joined;   // best effort — the caller's loaders will report failure
}

struct Tok {
    std::vector<std::string> t;
    size_t i = 0;
    bool has() const { return i < t.size(); }
    std::string next() { return i < t.size() ? t[i++] : std::string(); }
    float nextf() { std::string s = next(); return s.empty() ? 0.0f : static_cast<float>(atof(s.c_str())); }
    int nexti() { std::string s = next(); return s.empty() ? 0 : atoi(s.c_str()); }
    // consume a literal token if it matches (return true); else leave it.
    bool match(const char* lit) {
        if (i < t.size() && t[i] == lit) { ++i; return true; }
        return false;
    }
};

} // namespace

bool scn_load(const std::string& path, const std::string& base_dir,
              ScnScene& out, std::string* err) {
    std::ifstream f(path);
    if (!f) {
        if (err) *err = "cannot open " + path;
        return false;
    }
    Tok tok;
    std::string word;
    while (f >> word) tok.t.push_back(word);

    ScnScene scn;

    if (!tok.match("type:")) {
        if (err) *err = "missing 'type:' header";
        return false;
    }
    scn.type = tok.next();
    if (scn.type != "blinn" && scn.type != "pbrm" && scn.type != "pbrs") {
        if (err) *err = "unknown scene type '" + scn.type + "'";
        return false;
    }

    // lighting:
    if (!tok.match("lighting:")) { if (err) *err = "missing 'lighting:'"; return false; }
    if (!tok.match("background:")) { if (err) *err = "missing 'background:'"; return false; }
    scn.background[0] = tok.nextf(); scn.background[1] = tok.nextf(); scn.background[2] = tok.nextf();
    if (!tok.match("environment:")) { if (err) *err = "missing 'environment:'"; return false; }
    scn.environment = tok.next();
    if (!tok.match("skybox:")) { if (err) *err = "missing 'skybox:'"; return false; }
    scn.skybox = tok.next();
    if (!tok.match("shadow:")) { if (err) *err = "missing 'shadow:'"; return false; }
    scn.shadow = tok.next();
    if (!tok.match("ambient:")) { if (err) *err = "missing 'ambient:'"; return false; }
    scn.ambient = tok.nextf();
    if (!tok.match("punctual:")) { if (err) *err = "missing 'punctual:'"; return false; }
    scn.punctual = tok.nextf();

    // materials N:
    if (!tok.match("materials")) { if (err) *err = "missing 'materials N:'"; return false; }
    int num_materials = tok.nexti();
    for (int i = 0; i < num_materials; ++i) {
        if (!tok.match("material")) { if (err) *err = "missing 'material i:'"; return false; }
        int idx = tok.nexti();
        if (idx != i) { if (err) *err = "material index out of order"; return false; }
        ScnMaterial m;
        m.kind = (scn.type == "pbrm") ? 1 : (scn.type == "pbrs") ? 2 : 0;
        if (m.kind == 0) {                       // blinn
            if (!tok.match("basecolor:")) { if (err) *err = "material: missing basecolor"; return false; }
            m.base_color[0] = tok.nextf(); m.base_color[1] = tok.nextf();
            m.base_color[2] = tok.nextf(); m.base_color[3] = tok.nextf();
            if (!tok.match("shininess:")) { if (err) *err = "material: missing shininess"; return false; }
            m.shininess = tok.nextf();
            if (!tok.match("diffuse_map:")) { if (err) *err = "material: missing diffuse_map"; return false; }
            m.diffuse_map = tok.next();
            if (!tok.match("specular_map:")) { if (err) *err = "material: missing specular_map"; return false; }
            m.specular_map = tok.next();
            if (!tok.match("emission_map:")) { if (err) *err = "material: missing emission_map"; return false; }
            m.emission_map = tok.next();
        } else if (m.kind == 1) {                // pbrm
            if (!tok.match("basecolor_factor:")) { if (err) *err = "material: missing basecolor_factor"; return false; }
            m.base_color[0] = tok.nextf(); m.base_color[1] = tok.nextf();
            m.base_color[2] = tok.nextf(); m.base_color[3] = tok.nextf();
            if (!tok.match("metalness_factor:")) { if (err) *err = "material: missing metalness_factor"; return false; }
            m.metalness = tok.nextf();
            if (!tok.match("roughness_factor:")) { if (err) *err = "material: missing roughness_factor"; return false; }
            m.roughness = tok.nextf();
            if (!tok.match("basecolor_map:")) { if (err) *err = "material: missing basecolor_map"; return false; }
            m.basecolor_map = tok.next();
            if (!tok.match("metalness_map:")) { if (err) *err = "material: missing metalness_map"; return false; }
            m.metalness_map = tok.next();
            if (!tok.match("roughness_map:")) { if (err) *err = "material: missing roughness_map"; return false; }
            m.roughness_map = tok.next();
            if (!tok.match("normal_map:")) { if (err) *err = "material: missing normal_map"; return false; }
            m.normal_map = tok.next();
            if (!tok.match("occlusion_map:")) { if (err) *err = "material: missing occlusion_map"; return false; }
            m.occlusion_map = tok.next();
            if (!tok.match("emission_map:")) { if (err) *err = "material: missing emission_map"; return false; }
            m.emission_map = tok.next();
        } else {                                 // pbrs
            if (!tok.match("diffuse_factor:")) { if (err) *err = "material: missing diffuse_factor"; return false; }
            m.base_color[0] = tok.nextf(); m.base_color[1] = tok.nextf();
            m.base_color[2] = tok.nextf(); m.base_color[3] = tok.nextf();
            if (!tok.match("specular_factor:")) { if (err) *err = "material: missing specular_factor"; return false; }
            m.specular[0] = tok.nextf(); m.specular[1] = tok.nextf(); m.specular[2] = tok.nextf();
            if (!tok.match("glossiness_factor:")) { if (err) *err = "material: missing glossiness_factor"; return false; }
            m.glossiness = tok.nextf();
            if (!tok.match("diffuse_map:")) { if (err) *err = "material: missing diffuse_map"; return false; }
            m.diffuse_map = tok.next();
            if (!tok.match("specular_map:")) { if (err) *err = "material: missing specular_map"; return false; }
            m.specular_map = tok.next();
            if (!tok.match("glossiness_map:")) { if (err) *err = "material: missing glossiness_map"; return false; }
            m.glossiness_map = tok.next();
            if (!tok.match("normal_map:")) { if (err) *err = "material: missing normal_map"; return false; }
            m.normal_map = tok.next();
            if (!tok.match("occlusion_map:")) { if (err) *err = "material: missing occlusion_map"; return false; }
            m.occlusion_map = tok.next();
            if (!tok.match("emission_map:")) { if (err) *err = "material: missing emission_map"; return false; }
            m.emission_map = tok.next();
        }
        if (!tok.match("double_sided:")) { if (err) *err = "material: missing double_sided"; return false; }
        m.double_sided = (tok.next() == "on");
        if (!tok.match("enable_blend:")) { if (err) *err = "material: missing enable_blend"; return false; }
        m.enable_blend = (tok.next() == "on");
        if (!tok.match("alpha_cutoff:")) { if (err) *err = "material: missing alpha_cutoff"; return false; }
        m.alpha_cutoff = tok.nextf();

        m.diffuse_res    = resolve_path(base_dir, m.diffuse_map);
        m.specular_res   = resolve_path(base_dir, m.specular_map);
        m.emission_res   = resolve_path(base_dir, m.emission_map);
        m.basecolor_res  = resolve_path(base_dir, m.basecolor_map);
        m.metalness_res  = resolve_path(base_dir, m.metalness_map);
        m.roughness_res  = resolve_path(base_dir, m.roughness_map);
        m.normal_res     = resolve_path(base_dir, m.normal_map);
        m.occlusion_res  = resolve_path(base_dir, m.occlusion_map);
        m.glossiness_res = resolve_path(base_dir, m.glossiness_map);
        scn.materials.push_back(std::move(m));
    }

    // transforms N:
    if (!tok.match("transforms")) { if (err) *err = "missing 'transforms N:'"; return false; }
    int num_transforms = tok.nexti();
    for (int i = 0; i < num_transforms; ++i) {
        if (!tok.match("transform")) { if (err) *err = "missing 'transform i:'"; return false; }
        int idx = tok.nexti();
        if (idx != i) { if (err) *err = "transform index out of order"; return false; }
        ScnTransform tr;
        float row[4][4];
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) row[r][c] = tok.nextf();
        // file is row-major → column-major
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) tr.m[c * 4 + r] = row[r][c];
        scn.transforms.push_back(tr);
    }

    // models N:
    if (!tok.match("models")) { if (err) *err = "missing 'models N:'"; return false; }
    int num_models = tok.nexti();
    for (int i = 0; i < num_models; ++i) {
        if (!tok.match("model")) { if (err) *err = "missing 'model i:'"; return false; }
        int idx = tok.nexti();
        if (idx != i) { if (err) *err = "model index out of order"; return false; }
        ScnModelRef m;
        if (!tok.match("mesh:")) { if (err) *err = "model: missing mesh"; return false; }
        m.mesh_path = tok.next();
        if (!tok.match("skeleton:")) { if (err) *err = "model: missing skeleton"; return false; }
        m.skeleton_path = tok.next();
        if (!tok.match("attached:")) { if (err) *err = "model: missing attached"; return false; }
        m.attached = tok.nexti();
        if (!tok.match("material:")) { if (err) *err = "model: missing material"; return false; }
        m.material = tok.nexti();
        if (!tok.match("transform:")) { if (err) *err = "model: missing transform"; return false; }
        m.transform = tok.nexti();
        m.mesh_res = resolve_path(base_dir, m.mesh_path);
        m.skeleton_res = resolve_path(base_dir, m.skeleton_path);
        scn.models.push_back(std::move(m));
    }

    out = std::move(scn);
    return true;
}

} // namespace av
