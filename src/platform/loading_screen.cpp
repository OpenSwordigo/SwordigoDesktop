// =============================================================================
// loading_screen.cpp — Swordigo Desktop boot loading screen implementation.
// v3 remaster: fixed Hiro scale/orient/ground, vignette scrim, shimmer bar.
// See loading_screen.h for the design/threading contract.
// =============================================================================

#include "platform/loading_screen.h"
#include "platform/swordfare_theme.h"
#include "platform/data_path.h"
#include "platform/embedded_assets.h"
#include "platform/pvr_loader.h"
#include "platform/IconsFontAwesome6.h"
#include "tools/pod_loader.h"

#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_sdl3.h"
#include "imgui/backends/imgui_impl_opengl3.h"

#include <SDL3/SDL.h>
#define GL_GLEXT_PROTOTYPES
#include "platform/gl_inc.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Opaque hiro model helpers -------------------------------------------------
static av::PODModel* as_model(void* p) { return static_cast<av::PODModel*>(p); }

// Randomized flavor lines shown under the progress bar for a bit of fun.
static const char* kFlavorLines[] = {
    "Waking Hiro from a long nap...",
    "Sharpening the sword...",
    "Translating ARM64 spells to x86...",
    "Warming up the JIT cache...",
    "Untangling atomic spinlocks...",
    "Polishing the pixels...",
    "Convincing the GPU to cooperate...",
    "Bridging JNI to the mortal realm...",
    "Reticulating dungeon splines...",
    "Charging the magic meter...",
    "Lighting the torches...",
    "Feeding the cave lurkers...",
    "Rolling out the forest fog...",
};
static const int kFlavorCount = (int)(sizeof(kFlavorLines) / sizeof(kFlavorLines[0]));

// Real in-game scenery backgrounds (under <assets>/resources). One is chosen
// at random each launch so the loading screen feels alive and varied.
static const char* kBackgrounds[] = {
    "forest_background_2x.tex.png",
    "grasslandsbackground_day_2x.tex.png",
    "grove_bg_2x.tex.png",
    "cavesbackground2_2x.tex.png",
    "florennum_night_bg_2x.tex.png",
    "wasteland_bg_2x.tex.png",
    "woodkeep_bg_2x.tex.png",
    "grassbg_night_2x.tex.png",
};
static const int kBackgroundCount = (int)(sizeof(kBackgrounds) / sizeof(kBackgrounds[0]));

// A floating spark/dust mote for ambient deco.
struct Mote { float x, y, spd, size, phase; };

LoadingScreen::~LoadingScreen() { shutdown(); }

float LoadingScreen::elapsed() const {
    return (float)(ImGui::GetTime() - start_time_);
}

void LoadingScreen::set_progress(float p01) {
    if (p01 < 0.0f) p01 = 0.0f;
    if (p01 > 1.0f) p01 = 1.0f;
    progress_.store(p01, std::memory_order_relaxed);
}

void LoadingScreen::set_stage(const std::string& stage) {
    std::lock_guard<std::mutex> lk(stage_mtx_);
    stage_ = stage;
}

void LoadingScreen::finish() {
    set_progress(1.0f);
}

// ---------------------------------------------------------------------------
// Texture helper: decode an embedded PNG into a GL texture (GL 2.1 safe).
// ---------------------------------------------------------------------------
static unsigned int upload_rgba_tex(const unsigned char* px, int w, int h) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return (unsigned int)tex;
}

// Resolve a real game asset under <assets>/resources and load it as a GL tex
// via the engine's PVR loader (which transparently gunzips .tex.png and decodes
// PVR v2/v3). Returns 0 if the asset is missing/undecodable.
static unsigned int load_game_texture(const char* name, int* out_w = nullptr, int* out_h = nullptr) {
    std::string p = get_data_path(std::string("assets/resources/") + name);
    int w = 0, h = 0;
    unsigned int tex = pvr_load_texture(p.c_str(), &w, &h);
    if (tex) { if (out_w) *out_w = w; if (out_h) *out_h = h; }
    return tex;
}

static unsigned int load_embedded_logo(int* out_w, int* out_h) {
    // 1) Prefer the REAL in-game title art (gzipped PVR).
    unsigned int t = load_game_texture("swordigo_title_2x.pvr", out_w, out_h);
    if (t) return t;
    // 2) Fall back to an embedded PNG logo/icon.
    const unsigned char* data = nullptr;
    size_t size = 0;
    const char* names[] = { "startup.png", "icon_app.png", "launcer_icon.png", "icon_gnome.png" };
    for (const char* n : names) {
        if (embedded_asset(n, &data, &size)) {
            int w = 0, h = 0;
            unsigned char* px = nullptr;
            if (asset_decode_image(data, size, &px, &w, &h)) {
                unsigned int tex = upload_rgba_tex(px, w, h);
                asset_image_free(px);
                if (out_w) *out_w = w;
                if (out_h) *out_h = h;
                return tex;
            }
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// hiro run animation load (non-fatal).
// ---------------------------------------------------------------------------
bool LoadingScreen::load_hiro_animation() {
    // hiro_run.POD is an animation-only POD; av::pod_load merges it with the
    // base hiro mesh automatically (merge hint "hiro"). Resolve through the
    // game data path first, then fall back to the raw name.
    std::string resolved = get_data_path("assets/resources/hiro_run.POD");
    av::PODModel m = av::pod_load(resolved, "hiro");
    if (m.meshes.empty()) m = av::pod_load("hiro_run.POD", "hiro");
    if (m.meshes.empty()) m = av::pod_load(get_data_path("assets/resources/hiro.POD"), "hiro");
    if (m.meshes.empty()) return false;

    hiro_model_ = new av::PODModel(std::move(m));
    av::PODModel* mm = as_model(hiro_model_);

    // hiro references FIVE distinct textures (brightbluefabric / face /
    // char_beta2_legs / char_beta2 / brown128), one per material. Binding a
    // single texture for the whole model is what made the mesh look "random".
    // Load one GL texture per material so each node samples the right image.
    //
    // The POD stores source names as PNGs (e.g. "face.png"); the shipped game
    // asset is the PowerVR-compressed 2x variant ("face_2x.pvr"). Map name ->
    // <stem>_2x.pvr, then fall back to the raw stored name.
    auto load_material_texture = [](const std::string& stored) -> unsigned int {
        if (stored.empty()) return 0;
        std::string stem = stored;
        size_t dot = stem.find_last_of('.');
        if (dot != std::string::npos) stem = stem.substr(0, dot);
        unsigned int t = load_game_texture((stem + "_2x.pvr").c_str());
        if (!t) t = load_game_texture((stem + ".pvr").c_str());
        if (!t) t = load_game_texture(stored.c_str());   // last resort: as-stored
        return t;
    };

    auto* mat_tex = new std::vector<unsigned int>(mm->materials.size(), 0u);
    for (size_t mi = 0; mi < mm->materials.size(); ++mi) {
        int ti = mm->materials[mi].diffuse_texture_index;
        if (ti >= 0 && ti < (int)mm->texture_filenames.size())
            (*mat_tex)[mi] = load_material_texture(mm->texture_filenames[ti]);
    }
    hiro_mat_tex_ = mat_tex;
    return true;
}

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------
bool LoadingScreen::init(SDL_Window* window, void* imgui_ctx) {
    if (inited_) return true;
    if (!imgui_ctx) return false;   // must reuse SwordfareGUI's context
    window_    = window;
    imgui_ctx_ = imgui_ctx;

    // CRITICAL: reuse the game GUI's already-initialized ImGui context and its
    // SDL3+OpenGL3 backends (see header). Never init/shutdown the backend here.
    own_imgui_ctx_ = false;
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(imgui_ctx_));

    // Real game art: title logo, a randomly-chosen scenery background, and a
    // soft spark sprite — all via the engine PVR loader (gunzips .tex.png too).
    logo_tex_  = load_embedded_logo(&logo_w_, &logo_h_);

    unsigned int seed = (unsigned int)(SDL_GetTicks() ^ (uintptr_t)this);
    const char* bgname = kBackgrounds[seed % (unsigned)kBackgroundCount];
    bg_tex_    = load_game_texture(bgname, &bg_w_, &bg_h_);

    spark_tex_ = load_game_texture("spark_particle_2x.pvr");
    if (!spark_tex_) spark_tex_ = load_game_texture("flash_particle_2x.pvr");
    if (!spark_tex_) spark_tex_ = load_game_texture("particle_2x.pvr");

    hiro_ok_   = load_hiro_animation();

    // Seed ambient floating motes.
    {
        auto* motes = new std::vector<Mote>();
        mote_count_ = 46;
        motes->reserve(mote_count_);
        unsigned int s = seed ? seed : 1u;
        auto rnd = [&s]() { s = s * 1664525u + 1013904223u; return (float)((s >> 8) & 0xFFFF) / 65535.0f; };
        for (int i = 0; i < mote_count_; ++i) {
            Mote m;
            m.x = rnd();
            m.y = rnd();
            m.spd = 0.010f + rnd() * 0.045f;   // upward drift (normalized/sec)
            m.size = 2.0f + rnd() * 5.0f;
            m.phase = rnd() * 6.2831853f;
            motes->push_back(m);
        }
        motes_ = motes;
    }

    start_time_       = ImGui::GetTime();
    flavor_next_swap_ = start_time_;
    inited_ = true;
    return true;
}

// ---------------------------------------------------------------------------
// hiro GL pass (fixed-function GL 2.1)
//
// Fixes vs. old code:
//  • dist = radius * 1.85 (was 3.2) → hiro fills the lower-third properly
//  • yaw = -90° (was +90°) — POD authored facing camera (+Z); running right
//    requires yaw -90° so local forward aligns to world +X
//  • py derived from bounding-box min_y (feet) so hiro stands on a real floor
//  • crossing time 4.5 s (was ~7 s) — feels alive, not sluggish
//  • soft shadow ellipse beneath feet via GL quads (blended)
//  • subtle bounce from animation phase (1% of radius)
// ---------------------------------------------------------------------------
void LoadingScreen::draw_hiro() {
    if (!hiro_ok_ || !hiro_model_) return;
    av::PODModel* m = as_model(hiro_model_);
    if (m->meshes.empty()) return;

    // ── Advance animation frame ──────────────────────────────────────────────
    float fps = m->fps > 0 ? m->fps : 30.0f;
    hiro_frame_ += ImGui::GetIO().DeltaTime * fps;
    if (m->num_frames > 1)
        while (hiro_frame_ >= (float)m->num_frames) hiro_frame_ -= (float)m->num_frames;
    else
        hiro_frame_ = 0.0f;

    int win_w = 0, win_h = 0;
    SDL_GetWindowSizeInPixels(window_, &win_w, &win_h);
    if (win_w <= 0 || win_h <= 0) return;

    float radius = m->radius > 0 ? m->radius : 1.0f;

    // ── Camera setup ─────────────────────────────────────────────────────────
    // Perspective: 45° FoV, hiro occupies ~30% of screen height.
    // dist chosen so the model radius projects to ≈ 14% of half-screen height.
    glViewport(0, 0, win_w, win_h);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    const float aspect = (float)win_w / (float)win_h;
    const float fov    = 45.0f * 3.14159265f / 180.0f;
    const float f      = 1.0f / std::tan(fov * 0.5f);
    const float znear  = 0.1f, zfar = 2000.0f;
    const float proj[16] = {
        f/aspect, 0, 0, 0,
        0, f, 0, 0,
        0, 0, (zfar+znear)/(znear-zfar), -1.0f,
        0, 0, (2*zfar*znear)/(znear-zfar),  0
    };
    glMultMatrixf(proj);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    // dist: place hiro so his height fills about 1/3 of the screen.
    // Chosen so radius projects to ≈ 0.30 * half_screen_height.
    const float dist      = radius * 1.85f;
    const float half_w_at = std::tan(fov * 0.5f) * dist * aspect;

    // ── Horizontal traversal: left → right in 4.5 s, seamless loop ──────────
    const double now   = ImGui::GetTime();
    const float  loop  = (float)std::fmod(now / 4.5, 1.0);   // 0..1 in 4.5 s
    const float  px    = (loop * 2.4f - 1.2f) * half_w_at;   // world X

    // ── Vertical: ground hiro's feet on the lower-third of the screen ────────
    // The model's bounding box min_y is the feet level in model space.
    // We want feet to sit at ~68% down the screen (world Y = -projected floor).
    // Project the screen's 68% point back to world Y at `dist`:
    //   world_y = tan(fov/2) * dist * (1 - 2*0.68) = -fov_half_h * 0.36
    const float fov_half_h = std::tan(fov * 0.5f) * dist;
    const float floor_y    = -fov_half_h * 0.36f;              // world Y of the floor
    // Offset so model min_y sits at floor_y:
    const float feet_local = m->min_y;  // feet in model space (typically negative)
    // run-bob: tiny vertical bounce derived from animation phase (not arbitrary sin)
    const float bob        = std::sin(hiro_frame_ * (3.14159f * 2.0f / (float)(m->num_frames > 0 ? m->num_frames : 30)))
                             * radius * 0.012f;
    const float py         = floor_y - feet_local + bob;

    // ── Shadow ellipse (GL 2.1 blended quad under hiro's feet) ───────────────
    // Draw before the model so depth test doesn't clip it.
    {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_TEXTURE_2D);
        // Shadow is a flat ellipse at floor_y, slightly in front of ground.
        // We approximate with GL_TRIANGLE_FAN.
        const float sx = px, sy = floor_y + 0.5f, sz = -dist + 0.1f;
        const float ew = radius * 0.55f;  // ellipse x half-width
        const float eh = radius * 0.08f;  // ellipse y half-height
        glColor4f(0.0f, 0.0f, 0.0f, 0.38f);
        glBegin(GL_TRIANGLE_FAN);
        glVertex3f(sx, sy, sz);
        for (int si = 0; si <= 24; ++si) {
            float a = si * (3.14159265f * 2.0f / 24.0f);
            glVertex3f(sx + std::cos(a) * ew, sy + std::sin(a) * eh, sz);
        }
        glEnd();
        glDisable(GL_BLEND);
    }

    // ── Model transform ──────────────────────────────────────────────────────
    // The hiro POD is authored Y-up, character facing the camera (+Z world).
    // To run toward screen-right (+X) we yaw -90° about Y.
    // A tiny lateral sway adds life without looking mechanical.
    const float run_sway = std::sin((float)now * fps * 0.18f) * 2.2f;
    glTranslatef(px, py, -dist);
    glRotatef(-90.0f + run_sway, 0, 1, 0);
    glTranslatef(-m->center_x, -m->center_y, -m->center_z);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    // Keep both faces — mixed winding across the 5 sub-meshes causes holes
    // if back-face culling is on.
    glDisable(GL_CULL_FACE);

    auto* mat_tex = static_cast<std::vector<unsigned int>*>(hiro_mat_tex_);

    for (int ni = 0; ni < m->num_mesh_nodes && ni < (int)m->nodes.size(); ++ni) {
        const av::PODNode& node = m->nodes[ni];
        const int mesh_idx = node.object_index;
        if (mesh_idx < 0 || mesh_idx >= (int)m->meshes.size()) continue;
        const av::PODMesh& mesh = m->meshes[mesh_idx];

        // Per-node material texture (body / face / legs / plate / trim)
        unsigned int tex = 0;
        if (mat_tex && node.material_index >= 0 &&
            node.material_index < (int)mat_tex->size())
            tex = (*mat_tex)[node.material_index];
        if (tex) {
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, tex);
            glColor4f(1, 1, 1, 1);
        } else {
            glDisable(GL_TEXTURE_2D);
            glColor4f(0.62f, 0.56f, 0.92f, 1.0f);
        }

        std::vector<float> pos, nrm;
        const std::vector<float>* P = &mesh.positions;
        const std::vector<float>* U = &mesh.uvs;
        bool rigid = false;
        if (av::skin_mesh(*m, ni, hiro_frame_, pos, nrm) && !pos.empty()) {
            P = &pos;
        } else {
            float mtx[16];
            av::get_node_matrix(*m, ni, hiro_frame_, mtx);
            glPushMatrix();
            glMultMatrixf(mtx);
            rigid = true;
        }

        glBegin(GL_TRIANGLES);
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            for (int k = 0; k < 3; ++k) {
                uint32_t vi = mesh.indices[i + k];
                if (tex && U && (size_t)(vi * 2 + 1) < U->size()) {
                    float u = (*U)[vi * 2];
                    float v = (*U)[vi * 2 + 1];
                    if (m->uv_v_flipped) v = 1.0f - v;
                    glTexCoord2f(u, v);
                }
                if ((size_t)(vi * 3 + 2) < P->size())
                    glVertex3f((*P)[vi * 3], (*P)[vi * 3 + 1], (*P)[vi * 3 + 2]);
            }
        }
        glEnd();
        if (rigid) glPopMatrix();
    }

    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glMatrixMode(GL_MODELVIEW);  glPopMatrix();
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
}

// ---------------------------------------------------------------------------
// Full-screen scenery background via a GL-textured quad (GL 2.1) so it renders
// UNDER the 3D hiro pass. Slow Ken-Burns pan (cover-fit). No scrim here — the
// dark cinematic scrim is drawn in the ImGui background list (over hiro).
// ---------------------------------------------------------------------------
static void draw_scenery_gl(unsigned int tex, int tw, int th, int win_w, int win_h, double t) {
    if (!tex || tw <= 0 || th <= 0 || win_w <= 0 || win_h <= 0) return;

    glViewport(0, 0, win_w, win_h);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    // pixel-space ortho: (0,0) top-left → (win_w,win_h) bottom-right
    glOrtho(0, win_w, win_h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    float sw = (float)win_w, sh = (float)win_h;
    float img_ar = (float)tw / (float)th;
    float scr_ar = sw / sh;
    float dw, dh;
    if (img_ar > scr_ar) { dh = sh; dw = sh * img_ar; }
    else                 { dw = sw; dh = sw / img_ar; }
    float zoom = 1.10f + 0.03f * (float)std::sin(t * 0.15);
    dw *= zoom; dh *= zoom;
    float x0 = (sw - dw) * 0.5f + (float)std::sin(t * 0.05) * (dw - sw) * 0.10f;
    float y0 = (sh - dh) * 0.5f + (float)std::cos(t * 0.04) * 12.0f;

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);
    glColor4f(0.62f, 0.66f, 0.72f, 1.0f);  // slightly dimmed for mood
    // Swordigo PVR/tex assets use PowerVR's bottom-left texture origin, so
    // they are stored V-inverted vs. our top-left screen space. Flip V here
    // (t=1 at the top vertex, t=0 at the bottom) so scenery renders upright.
    glBegin(GL_QUADS);
        glTexCoord2f(0, 1); glVertex2f(x0,      y0);
        glTexCoord2f(1, 1); glVertex2f(x0 + dw, y0);
        glTexCoord2f(1, 0); glVertex2f(x0 + dw, y0 + dh);
        glTexCoord2f(0, 0); glVertex2f(x0,      y0 + dh);
    glEnd();
    glDisable(GL_TEXTURE_2D);

    glMatrixMode(GL_MODELVIEW);   glPopMatrix();
    glMatrixMode(GL_PROJECTION);  glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
}

// ---------------------------------------------------------------------------
// ImGui overlay — cinematic loading screen HUD (scrim + logo + bar + motes).
// v3: vignette frame, separated flavor/stage text, shimmer sweep on bar fill.
// ---------------------------------------------------------------------------
void LoadingScreen::draw_overlay() {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 ds = io.DisplaySize;
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    const double now = ImGui::GetTime();
    const float  cx  = ds.x * 0.5f;
    const float  dt  = io.DeltaTime > 0 ? io.DeltaTime : 0.016f;

    // ===== 1. Layered scrim — dark top (logo area) + clear mid (hiro) + dark bottom =====
    // Top: strong vignette so logo reads cleanly.
    bg->AddRectFilledMultiColor(
        ImVec2(0, 0),              ImVec2(ds.x, ds.y * 0.40f),
        IM_COL32(4,  4,  8, 220), IM_COL32(4,  4,  8, 220),
        IM_COL32(4,  4,  8,  18), IM_COL32(4,  4,  8,  18));
    // Bottom: strong dark gradient so progress bar + text read against any bg.
    bg->AddRectFilledMultiColor(
        ImVec2(0, ds.y * 0.56f),   ds,
        IM_COL32(3,  3,  7,  12), IM_COL32(3,  3,  7,  12),
        IM_COL32(3,  3,  7, 230), IM_COL32(3,  3,  7, 230));
    // Hard screen-edge vignette (4 side strips, 60 px) for cinematic framing.
    const float vig = 60.0f;
    // left / right:
    bg->AddRectFilledMultiColor(ImVec2(0, 0),           ImVec2(vig, ds.y),
        IM_COL32(0,0,0,140), IM_COL32(0,0,0,0), IM_COL32(0,0,0,0), IM_COL32(0,0,0,140));
    bg->AddRectFilledMultiColor(ImVec2(ds.x-vig, 0),    ImVec2(ds.x, ds.y),
        IM_COL32(0,0,0,0), IM_COL32(0,0,0,140), IM_COL32(0,0,0,140), IM_COL32(0,0,0,0));

    // ===== 2. Ambient floating motes =====
    if (motes_) {
        auto* motes = static_cast<std::vector<Mote>*>(motes_);
        for (auto& mt : *motes) {
            mt.y -= mt.spd * dt;
            if (mt.y < -0.05f) { mt.y = 1.05f; mt.x = std::fmod(mt.x + 0.37f, 1.0f); }
            float tw = 0.35f + 0.65f * (0.5f + 0.5f * std::sin((float)now * 2.0f + mt.phase));
            float sw = std::sin((float)now * 0.6f + mt.phase) * 14.0f;
            ImVec2 mp(mt.x * ds.x + sw, mt.y * ds.y);
            float  r  = mt.size * (0.8f + 0.4f * tw);
            ImU32  col = sf_theme::Lerp(sf_theme::Col_AccentAlt, sf_theme::Col_Accent, mt.phase / 6.28f);
            col = (col & 0x00FFFFFF) | ((unsigned)(130 * tw) << 24);
            if (spark_tex_)
                fg->AddImage((ImTextureID)(intptr_t)spark_tex_,
                             ImVec2(mp.x-r, mp.y-r), ImVec2(mp.x+r, mp.y+r),
                             ImVec2(0,1), ImVec2(1,0), col);
            else
                fg->AddCircleFilled(mp, r * 0.5f, col);
        }
    }

    // ===== 3. Logo in upper third =====
    const float logo_cy = ds.y * 0.22f;
    if (logo_tex_ && logo_w_ > 0 && logo_h_ > 0) {
        const float target_w = std::min(ds.x * 0.44f, 580.0f);
        const float ar  = (float)logo_w_ / (float)logo_h_;
        const float lw  = target_w, lh = target_w / ar;
        const float bob = std::sin((float)now * 1.05f) * 3.5f;
        // Radial glow behind logo
        if (spark_tex_) {
            float g = lw * 1.0f;
            fg->AddImage((ImTextureID)(intptr_t)spark_tex_,
                         ImVec2(cx - g*0.5f, logo_cy - g*0.25f + bob),
                         ImVec2(cx + g*0.5f, logo_cy + g*0.25f + bob),
                         ImVec2(0,0), ImVec2(1,1), IM_COL32(90, 75, 200, 45));
        }
        fg->AddImage((ImTextureID)(intptr_t)logo_tex_,
                     ImVec2(cx - lw*0.5f, logo_cy - lh*0.5f + bob),
                     ImVec2(cx + lw*0.5f, logo_cy + lh*0.5f + bob),
                     ImVec2(0,1), ImVec2(1,0));
        // Subtitle
        const char* sub = "N A T I V E   R U N T I M E   E N G I N E";
        ImVec2 ssz = ImGui::CalcTextSize(sub);
        fg->AddText(ImVec2(cx - ssz.x*0.5f, logo_cy + lh*0.5f + bob + 10.0f),
                    sf_theme::Col_TextDim, sub);
    } else {
        const char* title = "SWORDIGO DESKTOP";
        ImVec2 tsz = ImGui::CalcTextSize(title);
        fg->AddText(ImVec2(cx - tsz.x*0.5f, logo_cy), sf_theme::Col_Text, title);
        const char* sub = "N A T I V E   R U N T I M E   E N G I N E";
        ImVec2 ssz = ImGui::CalcTextSize(sub);
        fg->AddText(ImVec2(cx - ssz.x*0.5f, logo_cy + 28.0f), sf_theme::Col_TextDim, sub);
    }

    // ===== 4. Progress bar (bottom HUD) =====
    const float margin  = std::max(64.0f, ds.x * 0.09f);
    const float bar_y   = ds.y - 88.0f;
    const ImVec2 bar_min(margin, bar_y);
    const ImVec2 bar_max(ds.x - margin, bar_y + 10.0f);
    const float  prog   = progress_.load(std::memory_order_relaxed);
    const float  bar_w  = bar_max.x - bar_min.x;

    // Track: dark glass panel
    sf_theme::GlassPanel(fg,
        ImVec2(bar_min.x - 1, bar_min.y - 2), ImVec2(bar_max.x + 1, bar_max.y + 2),
        6.0f, IM_COL32(8, 10, 16, 200), IM_COL32(255, 255, 255, 22));

    // Fill: accent gradient
    const ImVec2 fill_max(bar_min.x + bar_w * prog, bar_max.y);
    if (fill_max.x > bar_min.x + 2.0f) {
        sf_theme::AccentBar(fg, bar_min, fill_max, sf_theme::Pulse(0.55f));

        // Shimmer sweep: a bright narrow strip sweeping across the fill
        const float sweep_period = 1.8f;
        const float sweep_t = std::fmod((float)now, sweep_period) / sweep_period;
        const float sweep_x = bar_min.x + bar_w * prog * sweep_t;
        const float sw = 28.0f;
        if (sweep_x + sw > bar_min.x && sweep_x - sw < fill_max.x) {
            fg->AddRectFilledMultiColor(
                ImVec2(sweep_x - sw, bar_min.y),
                ImVec2(sweep_x + sw, bar_max.y),
                IM_COL32(255,255,255,0), IM_COL32(255,255,255,55),
                IM_COL32(255,255,255,55), IM_COL32(255,255,255,0));
        }

        // Leading edge glow
        const float gx = fill_max.x, gy = (bar_min.y + bar_max.y) * 0.5f;
        if (spark_tex_)
            fg->AddImage((ImTextureID)(intptr_t)spark_tex_,
                         ImVec2(gx-18, gy-18), ImVec2(gx+18, gy+18),
                         ImVec2(0,0), ImVec2(1,1), IM_COL32(160, 190, 255, 210));
        else
            fg->AddCircleFilled(ImVec2(gx, gy), 5.0f, IM_COL32(200, 220, 255, 220));
    }

    // Stage text (left-aligned) + percent (right-aligned)
    std::string stage;
    { std::lock_guard<std::mutex> lk(stage_mtx_); stage = stage_; }
    char pct[16];
    std::snprintf(pct, sizeof(pct), "%d%%", (int)(prog * 100.0f + 0.5f));
    const float text_y = bar_max.y + 8.0f;
    fg->AddText(ImVec2(bar_min.x, text_y), sf_theme::Col_Text, stage.c_str());
    const ImVec2 psz = ImGui::CalcTextSize(pct);
    fg->AddText(ImVec2(bar_max.x - psz.x, text_y), sf_theme::Col_AccentAlt, pct);

    // Flavor line: centered, on the line below stage/pct so they don't overlap
    if (now >= flavor_next_swap_) {
        flavor_idx_ = (flavor_idx_ + 1 + (int)(now * 7.0)) % kFlavorCount;
        if (flavor_idx_ < 0) flavor_idx_ += kFlavorCount;
        flavor_next_swap_ = now + 2.6;
    }
    {
        const char* fl = kFlavorLines[flavor_idx_];
        const ImVec2 fsz = ImGui::CalcTextSize(fl);
        // 1 line below stage text (not on the same line)
        fg->AddText(ImVec2(cx - fsz.x * 0.5f, text_y + ImGui::GetTextLineHeight() + 4.0f),
                    sf_theme::Col_TextMuted, fl);
    }

    // Spinner: bottom-right corner
    {
        ImGui::SetNextWindowPos(ImVec2(ds.x - 64.0f, ds.y - 64.0f));
        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::Begin("##ls_spin", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoFocusOnAppearing);
        sf_theme::Spinner("##sp", 14.0f, 3, sf_theme::Col_AccentAlt);
        ImGui::End();
    }

    // ===== 5. Watchdog messages =====
    const float el = (float)(now - start_time_);
    const char* wd   = nullptr;
    ImU32       wd_col = sf_theme::Col_TextMuted;
    if      (el >= 30.0f) { wd = "Game cannot load. Run swordfare in a terminal for logs."; wd_col = sf_theme::Col_Danger; }
    else if (el >= 20.0f) { wd = "The game might be stuck — give it a few more seconds...";
wd_col = sf_theme::Col_Warning; }
    else if (el >= 10.0f) { wd = "Loading is taking a bit longer than usual :)";          wd_col = sf_theme::Col_TextDim; }
    if (wd) {
        const ImVec2 wsz = ImGui::CalcTextSize(wd);
        fg->AddText(ImVec2(cx - wsz.x * 0.5f, ds.y - 24.0f), wd_col, wd);
    }
}

// ---------------------------------------------------------------------------
// frame
// ---------------------------------------------------------------------------
void LoadingScreen::frame() {
    if (!inited_) return;

    // Draw on the shared SwordfareGUI ImGui context/backend.
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(imgui_ctx_));

    // Correct layering (back → front):
    //   1. GL clear
    //   2. scenery quad (GL)          — draw_scenery_gl
    //   3. hiro run animation (GL)    — draw_hiro
    //   4. ImGui (scrim in bg list + logo/HUD/particles in fg list)
    int win_w = 0, win_h = 0;
    SDL_GetWindowSizeInPixels(window_, &win_w, &win_h);
    glViewport(0, 0, win_w, win_h);
    glClearColor(0.031f, 0.035f, 0.055f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    double t = ImGui::GetTime();
    draw_scenery_gl(bg_tex_, bg_w_, bg_h_, win_w, win_h, t);
    draw_hiro();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    draw_overlay();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    SDL_GL_SwapWindow(window_);
}

// ---------------------------------------------------------------------------
// shutdown
// ---------------------------------------------------------------------------
void LoadingScreen::shutdown() {
    if (!inited_) return;
    inited_ = false;

    // Only free OUR OWN GPU resources. We deliberately do NOT touch the ImGui
    // context or the SDL3/OpenGL3 backends here: they are owned by SwordfareGUI
    // and shared process-wide.
    if (hiro_mat_tex_) {
        auto* mt = static_cast<std::vector<unsigned int>*>(hiro_mat_tex_);
        for (unsigned int gt : *mt) { if (gt) { GLuint g = (GLuint)gt; glDeleteTextures(1, &g); } }
        delete mt;
        hiro_mat_tex_ = nullptr;
    }
    if (logo_tex_)  { GLuint t = (GLuint)logo_tex_;  glDeleteTextures(1, &t); logo_tex_  = 0; }
    if (bg_tex_)    { GLuint t = (GLuint)bg_tex_;    glDeleteTextures(1, &t); bg_tex_    = 0; }
    if (spark_tex_) { GLuint t = (GLuint)spark_tex_; glDeleteTextures(1, &t); spark_tex_ = 0; }
    if (hiro_model_) { delete as_model(hiro_model_); hiro_model_ = nullptr; }
    if (motes_)      { delete static_cast<std::vector<Mote>*>(motes_); motes_ = nullptr; }
}
