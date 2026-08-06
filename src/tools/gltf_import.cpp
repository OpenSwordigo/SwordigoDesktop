// gltf_import.cpp — glTF 2.0 GLB importer for POD models.
// Parses a .glb back into a PODModel so edits made in Blender can be written
// back to .pod + .pvr game assets. Inverse of gltf_export.cpp.
//
// Round-trip mapping:
//   glTF node index  -> POD node index (same order)
//   mesh node         -> POD mesh node (object_index set, identity transform)
//   skin.joints       -> POD bone_batches.indices (batch-local JOINTS_0 values)
//   JOINTS_0/WEIGHTS_0-> POD bone_indices/bone_weights
//   animations        -> dense per-frame anim_* streams (num_frames, fps)
//   embedded PNG/JPEG -> GLTFImageBuffer for back-conversion to .pvr

#include "gltf_glb.h"
#include "pod_loader.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace av {
namespace {

// ─── Minimal JSON parser (DOM) ─────────────────────────────────────────
struct JsonNode;
using JsonArray = std::vector<JsonNode>;

struct JsonNode {
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ } type = NUL;
    bool b = false;
    double n = 0;
    std::string s;
    JsonArray arr;
    std::vector<std::pair<std::string, JsonNode>> obj;

    bool is_arr() const { return type == ARR; }
    bool is_obj() const { return type == OBJ; }
    bool is_num() const { return type == NUM; }
    bool is_str() const { return type == STR; }
    size_t size() const { return type == ARR ? arr.size() : (type == OBJ ? obj.size() : 0); }

    const JsonNode* get(const std::string& key) const {
        if (type != OBJ) return nullptr;
        for (const auto& kv : obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    double num(double def = 0) const { return type == NUM ? n : def; }
    int numi(int def = 0) const { return type == NUM ? static_cast<int>(std::lround(n)) : def; }
    const std::string& str(const std::string& def = "") const {
        static const std::string empty;
        return type == STR ? s : def;
    }
};

class JsonParser {
public:
    JsonParser(const char* data, size_t len) : p_(data), end_(data + len) {}

    bool parse(JsonNode& out) {
        skip_ws();
        if (!parse_value(out)) return false;
        skip_ws();
        return p_ == end_;
    }

private:
    const char* p_;
    const char* end_;

    void skip_ws() { while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_; }
    bool eof() const { return p_ >= end_; }
    char peek() const { return eof() ? 0 : *p_; }

    bool parse_value(JsonNode& out) {
        char c = peek();
        if (c == '{') return parse_obj(out);
        if (c == '[') return parse_arr(out);
        if (c == '"') return parse_string(out.s), out.type = JsonNode::STR, true;
        if (c == 't') { out.type = JsonNode::BOOL; out.b = true; p_ += 4; return true; }
        if (c == 'f') { out.type = JsonNode::BOOL; out.b = false; p_ += 5; return true; }
        if (c == 'n') { out.type = JsonNode::NUL; p_ += 4; return true; }
        return parse_number(out);
    }

    bool parse_obj(JsonNode& out) {
        ++p_; // {
        out.type = JsonNode::OBJ;
        skip_ws();
        if (peek() == '}') { ++p_; return true; }
        while (!eof()) {
            skip_ws();
            if (peek() != '"') return false;
            std::string key;
            parse_string(key);
            skip_ws();
            if (peek() != ':') return false;
            ++p_;
            skip_ws();
            JsonNode val;
            if (!parse_value(val)) return false;
            out.obj.emplace_back(std::move(key), std::move(val));
            skip_ws();
            if (peek() == ',') { ++p_; continue; }
            if (peek() == '}') { ++p_; return true; }
            return false;
        }
        return false;
    }

    bool parse_arr(JsonNode& out) {
        ++p_; // [
        out.type = JsonNode::ARR;
        skip_ws();
        if (peek() == ']') { ++p_; return true; }
        while (!eof()) {
            skip_ws();
            JsonNode val;
            if (!parse_value(val)) return false;
            out.arr.push_back(std::move(val));
            skip_ws();
            if (peek() == ',') { ++p_; continue; }
            if (peek() == ']') { ++p_; return true; }
            return false;
        }
        return false;
    }

    void parse_string(std::string& out) {
        ++p_; // "
        out.clear();
        while (!eof() && *p_ != '"') {
            if (*p_ == '\\') {
                ++p_;
                char c = *p_++;
                switch (c) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        unsigned v = 0;
                        for (int i = 0; i < 4 && !eof(); ++i) {
                            char h = *p_++;
                            v <<= 4;
                            v += (h >= '0' && h <= '9') ? (h - '0') : (h >= 'a' && h <= 'f') ? (h - 'a' + 10) : (h - 'A' + 10);
                        }
                        if (v < 0x80) out += static_cast<char>(v);
                        else if (v < 0x800) { out += static_cast<char>(0xC0 | (v >> 6)); out += static_cast<char>(0x80 | (v & 0x3F)); }
                        else { out += static_cast<char>(0xE0 | (v >> 12)); out += static_cast<char>(0x80 | ((v >> 6) & 0x3F)); out += static_cast<char>(0x80 | (v & 0x3F)); }
                        break;
                    }
                    default: out += c;
                }
            } else {
                out += *p_++;
            }
        }
        if (!eof()) ++p_; // trailing "
    }

    bool parse_number(JsonNode& out) {
        const char* start = p_;
        bool has_digit = false;
        while (!eof() && (isdigit((unsigned char)*p_) || *p_ == '-' || *p_ == '+' || *p_ == '.' || *p_ == 'e' || *p_ == 'E')) {
            if (isdigit((unsigned char)*p_)) has_digit = true;
            ++p_;
        }
        if (!has_digit) return false;
        out.type = JsonNode::NUM;
        out.n = strtod(start, nullptr);
        return true;
    }
};

// ─── Accessor decoding ─────────────────────────────────────────────────
int component_size(int ct) {
    switch (ct) {
        case 5120: case 5121: return 1;   // byte / unsigned byte
        case 5122: case 5123: return 2;   // short / unsigned short
        case 5125: case 5126: return 4;   // uint / float
        default: return 4;
    }
}
int component_count(const std::string& type) {
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    if (type == "MAT4") return 16;
    return 1;
}

struct BufferView {
    int buffer = 0;
    int byteOffset = 0;
    int byteLength = 0;
    int byteStride = 0;
};
struct Accessor {
    int bufferView = -1;
    int byteOffset = 0;
    int componentType = 5126;
    std::string type;
    int count = 0;
    bool normalized = false;
};

struct GltfSkin {
    int ibm_accessor = -1;
    std::vector<int> joints;
    int skeleton = -1;
};
struct GltfPrimitive {
    int material = -1;
    int pos = -1, nrm = -1, uv = -1, joints = -1, weights = -1, idx = -1;
};
struct GltfMesh {
    std::vector<GltfPrimitive> prims;
};
struct GltfNode {
    std::string name;
    int mesh = -1;
    int skin = -1;
    std::vector<int> children;
    // transform: either TRS or matrix (matrix wins)
    bool has_trs = false;
    float translation[3] = {0, 0, 0};
    float rotation[4] = {0, 0, 0, 1};
    float scale[3] = {1, 1, 1};
    bool has_matrix = false;
    float matrix[16];
};
struct GltfAnimChannel {
    int sampler = -1;
    int node = -1;
    std::string path;
};
struct GltfAnimSampler {
    int input = -1;
    int output = -1;
    std::string interp = "LINEAR";
};
struct GltfAnim {
    std::string name;
    std::vector<GltfAnimChannel> channels;
    std::vector<GltfAnimSampler> samplers;
};

struct GltfDocument {
    std::vector<BufferView> bufferViews;
    std::vector<Accessor> accessors;
    std::vector<GltfSkin> skins;
    std::vector<GltfMesh> meshes;
    std::vector<GltfNode> nodes;
    std::vector<GltfAnim> animations;
    // materials
    std::vector<std::string> mat_names;
    std::vector<float> mat_diffuse; // 3 floats per material
    std::vector<float> mat_opacity;
    std::vector<int> mat_base_tex;  // glTF texture index (-1 none)
    // textures -> images
    std::vector<int> tex_image;     // per glTF texture, image index
    // images
    std::vector<std::string> img_names;
    std::vector<std::vector<uint8_t>> img_data;
    std::vector<std::string> img_mime;
};

bool parse_document(const uint8_t* json, size_t json_len, const uint8_t* bin, size_t bin_len, GltfDocument& doc) {
    JsonNode root;
    JsonParser parser(reinterpret_cast<const char*>(json), json_len);
    if (!parser.parse(root) || !root.is_obj()) return false;

    // buffers (only first / GLB-stored is used)
    const JsonNode* buffers = root.get("buffers");
    if (buffers && buffers->is_arr()) {
        for (const auto& b : buffers->arr) {
            (void)b;
            // buffer 0 is the BIN chunk; others would need URI loading (unsupported)
        }
    }

    const JsonNode* bvs = root.get("bufferViews");
    if (bvs && bvs->is_arr()) {
        for (const auto& bv : bvs->arr) {
            BufferView v;
            v.buffer = bv.get("buffer") ? bv.get("buffer")->numi(0) : 0;
            v.byteOffset = bv.get("byteOffset") ? bv.get("byteOffset")->numi(0) : 0;
            v.byteLength = bv.get("byteLength") ? bv.get("byteLength")->numi(0) : 0;
            v.byteStride = bv.get("byteStride") ? bv.get("byteStride")->numi(0) : 0;
            doc.bufferViews.push_back(v);
        }
    }

    const JsonNode* accs = root.get("accessors");
    if (accs && accs->is_arr()) {
        for (const auto& a : accs->arr) {
            Accessor ac;
            ac.bufferView = a.get("bufferView") ? a.get("bufferView")->numi(-1) : -1;
            ac.byteOffset = a.get("byteOffset") ? a.get("byteOffset")->numi(0) : 0;
            ac.componentType = a.get("componentType") ? a.get("componentType")->numi(5126) : 5126;
            ac.type = a.get("type") ? a.get("type")->str("SCALAR") : "SCALAR";
            ac.count = a.get("count") ? a.get("count")->numi(0) : 0;
            ac.normalized = a.get("normalized") && a.get("normalized")->b;
            doc.accessors.push_back(ac);
        }
    }

    // skins
    const JsonNode* skins = root.get("skins");
    if (skins && skins->is_arr()) {
        for (const auto& s : skins->arr) {
            GltfSkin skin;
            skin.ibm_accessor = s.get("inverseBindMatrices") ? s.get("inverseBindMatrices")->numi(-1) : -1;
            skin.skeleton = s.get("skeleton") ? s.get("skeleton")->numi(-1) : -1;
            const JsonNode* joints = s.get("joints");
            if (joints && joints->is_arr())
                for (const auto& j : joints->arr) skin.joints.push_back(j.numi(-1));
            doc.skins.push_back(std::move(skin));
        }
    }

    // meshes
    const JsonNode* meshes = root.get("meshes");
    if (meshes && meshes->is_arr()) {
        for (const auto& m : meshes->arr) {
            GltfMesh gm;
            const JsonNode* prims = m.get("primitives");
            if (prims && prims->is_arr()) {
                for (const auto& p : prims->arr) {
                    GltfPrimitive gp;
                    gp.material = p.get("material") ? p.get("material")->numi(-1) : -1;
                    gp.idx = p.get("indices") ? p.get("indices")->numi(-1) : -1;
                    const JsonNode* attrs = p.get("attributes");
                    if (attrs && attrs->is_obj()) {
                        const JsonNode* v;
                        if ((v = attrs->get("POSITION"))) gp.pos = v->numi(-1);
                        if ((v = attrs->get("NORMAL"))) gp.nrm = v->numi(-1);
                        if ((v = attrs->get("TEXCOORD_0"))) gp.uv = v->numi(-1);
                        if ((v = attrs->get("JOINTS_0"))) gp.joints = v->numi(-1);
                        if ((v = attrs->get("WEIGHTS_0"))) gp.weights = v->numi(-1);
                    }
                    gm.prims.push_back(gp);
                }
            }
            doc.meshes.push_back(std::move(gm));
        }
    }

    // nodes
    const JsonNode* nodes = root.get("nodes");
    if (nodes && nodes->is_arr()) {
        for (const auto& n : nodes->arr) {
            GltfNode gn;
            if (n.get("name")) gn.name = n.get("name")->str();
            gn.mesh = n.get("mesh") ? n.get("mesh")->numi(-1) : -1;
            gn.skin = n.get("skin") ? n.get("skin")->numi(-1) : -1;
            const JsonNode* children = n.get("children");
            if (children && children->is_arr())
                for (const auto& c : children->arr) gn.children.push_back(c.numi(-1));
            const JsonNode* tr = n.get("translation");
            if (tr && tr->is_arr() && tr->size() >= 3) {
                gn.has_trs = true;
                for (int i = 0; i < 3; ++i) gn.translation[i] = static_cast<float>(tr->arr[i].num());
            }
            const JsonNode* rot = n.get("rotation");
            if (rot && rot->is_arr() && rot->size() >= 4) {
                gn.has_trs = true;
                for (int i = 0; i < 4; ++i) gn.rotation[i] = static_cast<float>(rot->arr[i].num());
            }
            const JsonNode* sc = n.get("scale");
            if (sc && sc->is_arr() && sc->size() >= 3) {
                gn.has_trs = true;
                for (int i = 0; i < 3; ++i) gn.scale[i] = static_cast<float>(sc->arr[i].num());
            }
            const JsonNode* mx = n.get("matrix");
            if (mx && mx->is_arr() && mx->size() >= 16) {
                gn.has_matrix = true;
                for (int i = 0; i < 16; ++i) gn.matrix[i] = static_cast<float>(mx->arr[i].num());
            }
            doc.nodes.push_back(std::move(gn));
        }
    }

    // animations
    const JsonNode* anims = root.get("animations");
    if (anims && anims->is_arr()) {
        for (const auto& a : anims->arr) {
            GltfAnim ga;
            if (a.get("name")) ga.name = a.get("name")->str();
            const JsonNode* chans = a.get("channels");
            if (chans && chans->is_arr()) {
                for (const auto& c : chans->arr) {
                    GltfAnimChannel ch;
                    ch.sampler = c.get("sampler") ? c.get("sampler")->numi(-1) : -1;
                    const JsonNode* target = c.get("target");
                    if (target) {
                        ch.node = target->get("node") ? target->get("node")->numi(-1) : -1;
                        ch.path = target->get("path") ? target->get("path")->str("") : "";
                    }
                    ga.channels.push_back(ch);
                }
            }
            const JsonNode* samps = a.get("samplers");
            if (samps && samps->is_arr()) {
                for (const auto& s : samps->arr) {
                    GltfAnimSampler sm;
                    sm.input = s.get("input") ? s.get("input")->numi(-1) : -1;
                    sm.output = s.get("output") ? s.get("output")->numi(-1) : -1;
                    sm.interp = s.get("interpolation") ? s.get("interpolation")->str("LINEAR") : "LINEAR";
                    ga.samplers.push_back(sm);
                }
            }
            doc.animations.push_back(std::move(ga));
        }
    }

    // materials
    const JsonNode* mats = root.get("materials");
    if (mats && mats->is_arr()) {
        for (const auto& m : mats->arr) {
            doc.mat_names.push_back(m.get("name") ? m.get("name")->str("") : "");
            float d[3] = {1, 1, 1};
            float op = 1.0f;
            int tex = -1;
            const JsonNode* pbr = m.get("pbrMetallicRoughness");
            if (pbr) {
                const JsonNode* bcf = pbr->get("baseColorFactor");
                if (bcf && bcf->is_arr() && bcf->size() >= 4) {
                    d[0] = static_cast<float>(bcf->arr[0].num());
                    d[1] = static_cast<float>(bcf->arr[1].num());
                    d[2] = static_cast<float>(bcf->arr[2].num());
                    op = static_cast<float>(bcf->arr[3].num());
                }
                const JsonNode* bct = pbr->get("baseColorTexture");
                if (bct && bct->get("index")) tex = bct->get("index")->numi(-1);
            }
            const JsonNode* am = m.get("alphaMode");
            if (am && am->is_str() && am->s == "BLEND") { /* opacity already read */ }
            doc.mat_diffuse.push_back(d[0]); doc.mat_diffuse.push_back(d[1]); doc.mat_diffuse.push_back(d[2]);
            doc.mat_opacity.push_back(op);
            doc.mat_base_tex.push_back(tex);
        }
    }

    // textures -> images
    const JsonNode* textures = root.get("textures");
    if (textures && textures->is_arr()) {
        for (const auto& t : textures->arr) {
            doc.tex_image.push_back(t.get("source") ? t.get("source")->numi(-1) : -1);
        }
    }

    // images
    const JsonNode* images = root.get("images");
    if (images && images->is_arr()) {
        for (const auto& im : images->arr) {
            doc.img_names.push_back(im.get("name") ? im.get("name")->str("") : "");
            doc.img_mime.push_back(im.get("mimeType") ? im.get("mimeType")->str("image/png") : "image/png");
            const JsonNode* bv = im.get("bufferView");
            if (bv) {
                int view = bv->numi(-1);
                if (view >= 0 && view < (int)doc.bufferViews.size()) {
                    const auto& vb = doc.bufferViews[view];
                    int off = vb.byteOffset;
                    int len = vb.byteLength;
                    if (off + len <= (int)bin_len) {
                        doc.img_data.emplace_back(bin + off, bin + off + len);
                        continue;
                    }
                }
            }
            const JsonNode* uri = im.get("uri");
            if (uri && uri->is_str()) {
                const std::string& u = uri->s;
                if (u.rfind("data:", 0) == 0) {
                    // data:image/png;base64,...
                    size_t comma = u.find(',');
                    if (comma != std::string::npos) {
                        std::string b64 = u.substr(comma + 1);
                        std::vector<uint8_t> raw;
                        static const std::string table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                        size_t len = b64.size();
                        for (size_t i = 0; i + 1 < len; i += 4) {
                            auto d = [&](char c) -> int { if (c == '=') return 0; size_t p = table.find(c); return p == std::string::npos ? 0 : (int)p; };
                            int a = d(b64[i]), b_ = d(b64[i+1]), cc = i + 2 < len ? d(b64[i+2]) : 0, dd = i + 3 < len ? d(b64[i+3]) : 0;
                            raw.push_back(static_cast<uint8_t>((a << 2) | (b_ >> 4)));
                            if (i + 2 < len && b64[i+2] != '=') raw.push_back(static_cast<uint8_t>(((b_ & 0xF) << 4) | (cc >> 2)));
                            if (i + 3 < len && b64[i+3] != '=') raw.push_back(static_cast<uint8_t>(((cc & 0x3) << 6) | dd));
                        }
                        // strip data: header whitespace/newlines
                        std::vector<uint8_t> cleaned;
                        for (uint8_t x : raw) if (x != '\n' && x != '\r') cleaned.push_back(x);
                        doc.img_data.push_back(std::move(cleaned));
                        continue;
                    }
                }
            }
            doc.img_data.emplace_back(); // no payload
        }
    }

    return true;
}

// Read a single accessor as floats (normalized handled for u16/u8 joints).
bool read_accessor(const GltfDocument& doc, int idx, const uint8_t* bin, size_t bin_len, std::vector<float>& out) {
    if (idx < 0 || idx >= (int)doc.accessors.size()) return false;
    const auto& ac = doc.accessors[idx];
    if (ac.bufferView < 0 || ac.bufferView >= (int)doc.bufferViews.size()) return false;
    const auto& bv = doc.bufferViews[ac.bufferView];
    if (bv.buffer != 0) return false;
    size_t base = static_cast<size_t>(bv.byteOffset) + static_cast<size_t>(ac.byteOffset);
    if (base >= bin_len) return false;
    const int comps = component_count(ac.type);
    const int csize = component_size(ac.componentType);
    const int stride = bv.byteStride > 0 ? bv.byteStride : comps * csize;
    out.reserve(static_cast<size_t>(ac.count) * comps);
    for (int i = 0; i < ac.count; ++i) {
        const uint8_t* p = bin + base + static_cast<size_t>(i) * stride;
        for (int c = 0; c < comps; ++c) {
            const uint8_t* q = p + static_cast<size_t>(c) * csize;
            switch (ac.componentType) {
                case 5126: { float v; std::memcpy(&v, q, 4); out.push_back(v); break; }
                case 5123: { uint16_t v = static_cast<uint16_t>(q[0] | (q[1] << 8)); out.push_back(ac.normalized ? v / 65535.0f : (float)v); break; }
                case 5125: { uint32_t v = static_cast<uint32_t>(q[0] | (q[1] << 8) | (q[2] << 16) | (q[3] << 24)); out.push_back((float)v); break; }
                case 5121: { out.push_back((float)*q); break; }
                case 5120: { out.push_back((float)(int8_t)*q); break; }
                case 5122: { int16_t v = static_cast<int16_t>(q[0] | (q[1] << 8)); out.push_back((float)v); break; }
                default: out.push_back(0.0f);
            }
        }
    }
    return true;
}

// Decompose a column-major 4x4 into TRS (matches glTF column-major order).
void mat_decompose_col(const float m[16], float t[3], float q[4], float s[3]) {
    t[0] = m[12]; t[1] = m[13]; t[2] = m[14];
    s[0] = std::sqrt(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
    s[1] = std::sqrt(m[4]*m[4] + m[5]*m[5] + m[6]*m[6]);
    s[2] = std::sqrt(m[8]*m[8] + m[9]*m[9] + m[10]*m[10]);
    if (s[0] < 1e-8f) s[0] = 1.0f; if (s[1] < 1e-8f) s[1] = 1.0f; if (s[2] < 1e-8f) s[2] = 1.0f;
    float r[9] = {m[0]/s[0], m[1]/s[0], m[2]/s[0],
                  m[4]/s[1], m[5]/s[1], m[6]/s[1],
                  m[8]/s[2], m[9]/s[2], m[10]/s[2]};
    float trace = r[0] + r[5] + r[10];
    if (trace > 0.0f) {
        float S = std::sqrt(trace + 1.0f) * 2.0f;
        q[0] = (r[7] - r[6]) / S; q[1] = (r[2] - r[8]) / S; q[2] = (r[3] - r[1]) / S; q[3] = 0.25f * S;
    } else if (r[0] > r[5] && r[0] > r[10]) {
        float S = std::sqrt(1.0f + r[0] - r[5] - r[10]) * 2.0f;
        q[0] = 0.25f * S; q[1] = (r[3] + r[1]) / S; q[2] = (r[2] + r[8]) / S; q[3] = (r[7] - r[6]) / S;
    } else if (r[5] > r[10]) {
        float S = std::sqrt(1.0f + r[5] - r[0] - r[10]) * 2.0f;
        q[0] = (r[3] + r[1]) / S; q[1] = 0.25f * S; q[2] = (r[7] + r[6]) / S; q[3] = (r[2] - r[8]) / S;
    } else {
        float S = std::sqrt(1.0f + r[10] - r[0] - r[5]) * 2.0f;
        q[0] = (r[2] + r[8]) / S; q[1] = (r[7] + r[6]) / S; q[2] = 0.25f * S; q[3] = (r[3] - r[1]) / S;
    }
    float n = std::sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
    if (n > 1e-8f) { q[0] /= n; q[1] /= n; q[2] /= n; q[3] /= n; }
    if (n == 0.0f) q[3] = 1.0f;
}

} // namespace

bool gltf_import_glb(const std::string& path, PODModel& out,
                     std::vector<GLTFImageBuffer>& images, std::string* err) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) { if (err) *err = "cannot open: " + path; return false; }
    std::streamsize size = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> file(static_cast<size_t>(size));
    if (size > 0) f.read(reinterpret_cast<char*>(file.data()), size);
    if (!f) { if (err) *err = "read failed"; return false; }

    // GLB container: magic, version, length, then chunks.
    if (file.size() < 12 || std::memcmp(file.data(), "glTF", 4) != 0) {
        if (err) *err = "not a GLB file"; return false;
    }
    uint32_t ver = (uint32_t)file[4] | ((uint32_t)file[5] << 8) | ((uint32_t)file[6] << 16) | ((uint32_t)file[7] << 24);
    if (ver != 2) { if (err) *err = "unsupported GLB version"; return false; }

    const uint8_t* json = nullptr;
    size_t json_len = 0;
    const uint8_t* bin = nullptr;
    size_t bin_len = 0;

    size_t pos = 12;
    int chunk = 0;
    while (pos + 8 <= file.size() && chunk < 2) {
        uint32_t len = (uint32_t)file[pos] | ((uint32_t)file[pos+1] << 8) | ((uint32_t)file[pos+2] << 16) | ((uint32_t)file[pos+3] << 24);
        std::string type((const char*)file.data() + pos + 4, 4);
        if (type == "JSON" && !json) { json = file.data() + pos + 8; json_len = len; }
        else if (type.rfind("BIN", 0) == 0 && !bin) { bin = file.data() + pos + 8; bin_len = len; }
        ++chunk;
        pos += 8 + len;
    }
    if (!json) { if (err) *err = "missing JSON chunk"; return false; }

    GltfDocument doc;
    if (!parse_document(json, json_len, bin, bin_len, doc)) {
        if (err) *err = "failed to parse glTF JSON"; return false;
    }

    // ── Rebuild PODModel ───────────────────────────────────────────────
    std::vector<int> parent_of(doc.nodes.size(), -1);
    for (size_t i = 0; i < doc.nodes.size(); ++i)
        for (int c : doc.nodes[i].children)
            if (c >= 0 && c < (int)doc.nodes.size()) parent_of[c] = static_cast<int>(i);

    std::vector<float> accbuf;
    auto load = [&](int acc_idx) -> const std::vector<float>& {
        accbuf.clear();
        if (!read_accessor(doc, acc_idx, bin, bin_len, accbuf)) accbuf.clear();
        return accbuf;
    };

    // ── Meshes ─────────────────────────────────────────────────────────
    out.meshes.clear();
    for (const auto& gm : doc.meshes) {
        for (const auto& p : gm.prims) {
            PODMesh m;
            const std::vector<float>& pos = load(p.pos);
            if (!pos.empty()) {
                int nv = doc.accessors[p.pos].count;
                m.num_vertices = nv;
                m.positions = pos;
            }
            const std::vector<float>& nrm = load(p.nrm);
            if (!nrm.empty()) m.normals = nrm;
            const std::vector<float>& uv = load(p.uv);
            if (!uv.empty()) m.uvs = uv;
            const std::vector<float>& idx = load(p.idx);
            if (!idx.empty()) {
                m.indices.reserve(idx.size());
                for (float x : idx) m.indices.push_back(static_cast<uint32_t>(x));
                m.num_faces = static_cast<int>(m.indices.size() / 3);
            }

            // skinning
            const std::vector<float>& joints = load(p.joints);
            const std::vector<float>& weights = load(p.weights);
            if (!joints.empty() && m.num_vertices > 0) {
                // joints component count from accessor type
                int jcomps = (p.joints >= 0 && p.joints < (int)doc.accessors.size())
                             ? component_count(doc.accessors[p.joints].type) : 4;
                int nverts = m.num_vertices;
                int max_inf = 0;
                for (int v = 0; v < nverts; ++v) {
                    int infl = 0;
                    for (int k = 0; k < jcomps; ++k) {
                        size_t i = (size_t)v * jcomps + k;
                        float w = weights.size() > i ? weights[i] : 0.0f;
                        if (w > 0.0f) infl = k + 1;
                    }
                    max_inf = std::max(max_inf, infl);
                }
                max_inf = std::clamp(max_inf, 1, 4);
                m.bones_per_vertex = max_inf;
                m.bone_indices.assign((size_t)nverts * max_inf, 0.0f);
                m.bone_weights.assign((size_t)nverts * max_inf, 0.0f);
                for (int v = 0; v < nverts; ++v) {
                    for (int k = 0; k < max_inf; ++k) {
                        size_t src = (size_t)v * jcomps + k;
                        size_t dst = (size_t)v * max_inf + k;
                        m.bone_indices[dst] = joints.size() > src ? joints[src] : 0.0f;
                        m.bone_weights[dst] = weights.size() > src ? weights[src] : 0.0f;
                    }
                }
                m.has_bone_batches = true;
                m.bone_batches.count = 1;
                m.bone_batches.offsets = {0};
                m.bone_batches.counts = {1};
                m.bone_batches.max_bones = 1;
                // indices filled per-skin below once node→skin known
            }

            m.num_vertices = m.num_vertices ? m.num_vertices : (int)(m.positions.size() / 3);
            if (m.num_vertices == 0 && !m.positions.empty()) m.num_vertices = (int)(m.positions.size() / 3);
            if (m.num_faces == 0) m.num_faces = m.indices.empty()
                ? (m.num_vertices > 0 ? m.num_vertices / 3 : 0)   // non-indexed triangle soup
                : (int)(m.indices.size() / 3);
            out.meshes.push_back(std::move(m));
        }
    }

    // ── Nodes ──────────────────────────────────────────────────────────
    out.nodes.clear();
    // glTF mesh index -> POD mesh index. Shared glTF meshes share one POD mesh.
    std::vector<int> pod_mesh_of_gltf_mesh(doc.meshes.size(), -1);
    {
        int cursor = 0;
        for (size_t m = 0; m < doc.meshes.size(); ++m) {
            pod_mesh_of_gltf_mesh[m] = cursor;
            cursor += (int)doc.meshes[m].prims.size();
        }
    }
    std::vector<int> node_mesh_begin(doc.nodes.size(), -1);
    std::vector<int> node_mesh_end(doc.nodes.size(), -1);
    for (size_t n = 0; n < doc.nodes.size(); ++n) {
        if (doc.nodes[n].mesh >= 0 && doc.nodes[n].mesh < (int)doc.meshes.size()) {
            int m = doc.nodes[n].mesh;
            node_mesh_begin[n] = pod_mesh_of_gltf_mesh[m];
            node_mesh_end[n] = pod_mesh_of_gltf_mesh[m] + (int)doc.meshes[m].prims.size();
        }
    }

    for (size_t i = 0; i < doc.nodes.size(); ++i) {
        const GltfNode& gn = doc.nodes[i];
        PODNode n;
        n.name = gn.name;
        n.parent_index = parent_of[i];
        if (gn.mesh >= 0 && gn.mesh < (int)doc.meshes.size()) {
            n.object_index = pod_mesh_of_gltf_mesh[gn.mesh];
            // POD stores the material on the mesh node; exporter wrote it as the
            // prim's material, so recover it from the first prim of the node's mesh.
            if (!doc.meshes[gn.mesh].prims.empty())
                n.material_index = doc.meshes[gn.mesh].prims[0].material;
        }
        if (gn.has_matrix) {
            n.has_matrix = true;
            // glTF matrix is column-major; POD stores row-major? POD is column-major too
            // (get_node_matrix writes local[col*4+row] style). Store raw.
            std::memcpy(n.matrix, gn.matrix, 16 * sizeof(float));
        } else if (gn.has_trs) {
            n.has_translation = true;
            std::memcpy(n.translation, gn.translation, sizeof(float) * 3);
            n.has_rotation = true;
            std::memcpy(n.rotation, gn.rotation, sizeof(float) * 4);
            n.has_scale = true;
            std::memcpy(n.scale, gn.scale, sizeof(float) * 3);
        }
        out.nodes.push_back(std::move(n));
    }

    // ── Skins: fill bone batches from skin.joints ──────────────────────
    for (size_t i = 0; i < doc.nodes.size(); ++i) {
        if (doc.nodes[i].skin < 0 || doc.nodes[i].skin >= (int)doc.skins.size()) continue;
        const auto& skin = doc.skins[doc.nodes[i].skin];
        if (skin.joints.empty()) continue;
        int mesh_begin = node_mesh_begin[i];
        int mesh_end = node_mesh_end[i];
        if (mesh_begin < 0) continue;
        for (int mi = mesh_begin; mi < mesh_end; ++mi) {
            PODMesh& m = out.meshes[mi];
            if (m.bones_per_vertex <= 0) continue;
            m.bone_batches.indices.assign(skin.joints.begin(), skin.joints.end());
            m.bone_batches.counts = {static_cast<uint32_t>(skin.joints.size())};
            m.bone_batches.offsets = {0};
            m.bone_batches.count = 1;
            m.bone_batches.max_bones = static_cast<int>(skin.joints.size());
            m.has_bone_batches = true;
        }
    }

    // ── Materials / textures ───────────────────────────────────────────
    out.materials.clear();
    out.texture_filenames.clear();
    images.clear();
    for (size_t i = 0; i < doc.mat_names.size(); ++i) {
        PODMaterial mat;
        mat.name = doc.mat_names[i];
        mat.diffuse[0] = doc.mat_diffuse[i * 3 + 0];
        mat.diffuse[1] = doc.mat_diffuse[i * 3 + 1];
        mat.diffuse[2] = doc.mat_diffuse[i * 3 + 2];
        mat.opacity = doc.mat_opacity[i];
        int tex = doc.mat_base_tex[i];
        if (tex >= 0 && tex < (int)doc.tex_image.size()) {
            int img = doc.tex_image[tex];
            if (img >= 0 && img < (int)doc.img_data.size() && !doc.img_data[img].empty()) {
                // find or create POD texture filename
                std::string name = doc.img_names[img];
                if (name.empty()) name = "texture" + std::to_string(i) + ".png";
                auto it = std::find(out.texture_filenames.begin(), out.texture_filenames.end(), name);
                int ti = (int)(it - out.texture_filenames.begin());
                if (it == out.texture_filenames.end()) {
                    out.texture_filenames.push_back(name);
                    GLTFImageBuffer ib;
                    ib.mime = doc.img_mime[img];
                    ib.data = doc.img_data[img];
                    images.push_back(std::move(ib));
                }
                mat.diffuse_texture_index = ti;
            }
        }
        out.materials.push_back(std::move(mat));
    }

    // ── Animations: dense per-frame streams ────────────────────────────
    out.num_frames = 0;
    out.fps = 30.0f;
    if (!doc.animations.empty()) {
        // collect max time/key count across all samplers
        int max_keys = 0;
        for (const auto& anim : doc.animations)
            for (const auto& s : anim.samplers) {
                if (s.input < 0 || s.input >= (int)doc.accessors.size()) continue;
                max_keys = std::max(max_keys, doc.accessors[s.input].count);
            }
        // determine fps from the first time accessor's spacing
        if (max_keys > 0) {
            float t0 = 0.0f, t1 = 0.0f;
            for (const auto& anim : doc.animations)
                for (const auto& s : anim.samplers) {
                    if (s.input < 0) continue;
                    const std::vector<float>& times = load(s.input);
                    if (times.size() >= 2) { t0 = times[0]; t1 = times[1]; break; }
                }
            if (t1 > t0) {
                float fps = std::round(1.0f / (t1 - t0));
                if (fps >= 1.0f && fps <= 240.0f) out.fps = fps;
            }
            out.num_frames = max_keys;
        }

        std::vector<int> node_has_anim(out.nodes.size(), 0);
        for (const auto& anim : doc.animations) {
            for (const auto& ch : anim.channels) {
                if (ch.node < 0 || ch.node >= (int)out.nodes.size()) continue;
                if (ch.sampler < 0 || ch.sampler >= (int)anim.samplers.size()) continue;
                const auto& sm = anim.samplers[ch.sampler];
                // load() returns a reference to a shared buffer; copy so later
                // loads (output) don't invalidate the times data.
                const std::vector<float>& times_ref = load(sm.input);
                std::vector<float> times(times_ref.begin(), times_ref.end());
                const std::vector<float>& vals = load(sm.output);
                if (vals.empty()) continue;
                int comps = 1;
                if (ch.path == "translation" || ch.path == "scale") comps = 3;
                else if (ch.path == "rotation") comps = 4;
                PODNode& node = out.nodes[ch.node];
                std::vector<float> dense((size_t)out.num_frames * comps, 0.0f);
                // place keys at integer-frame times (times = k / fps)
                for (size_t k = 0; k < times.size() && k < vals.size() / (size_t)comps; ++k) {
                    int frame = static_cast<int>(std::lround(times[k] * out.fps));
                    frame = std::clamp(frame, 0, out.num_frames - 1);
                    for (int c = 0; c < comps; ++c)
                        dense[(size_t)frame * comps + c] = vals[k * (size_t)comps + c];
                }
                if (ch.path == "translation") { node.anim_translation = dense; }
                else if (ch.path == "rotation") { node.anim_rotation = dense; }
                else if (ch.path == "scale") { node.anim_scale = dense; }
                node_has_anim[ch.node] = 1;
            }
        }
        // Any node with animation but no scale/rot channel still needs static defaults
        for (size_t i = 0; i < out.nodes.size(); ++i) {
            if (!node_has_anim[i]) continue;
            PODNode& node = out.nodes[i];
            if (node.anim_scale.empty() && node.anim_rotation.empty() && node.anim_translation.empty())
                node_has_anim[i] = 0;
        }
    }

    out.num_mesh_nodes = 0;
    for (size_t i = 0; i < out.nodes.size(); ++i)
        if (out.nodes[i].object_index >= 0) out.num_mesh_nodes = (int)(i + 1);

    // Node-less meshes still need valid mesh metadata
    for (auto& mesh : out.meshes) {
        if (mesh.num_vertices == 0) mesh.num_vertices = (int)(mesh.positions.size() / 3);
        if (mesh.num_faces == 0) mesh.num_faces = (int)(mesh.indices.size() / 3);
    }

    return true;
}

} // namespace av
