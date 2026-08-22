// loading_screen.cpp — Swordigo Desktop boot loading screen
//
// Renders hiro POD animation + scenery background + logo + progress bar
// during the gap between window creation and game first frame.
//
// Fixed: hiro size/position, background fallback, slideshow cycling,
//        proper GL 2.1 fixed-function POD rendering.

#include "loading_screen.h"
#include "gl_inc.h"
#include "data_path.h"
#include "pvr_loader.h"
#include "embedded_assets.h"
#include "tools/pod_loader.h"

#include <imgui.h>
#include "imgui/backends/imgui_impl_sdl3.h"
#include "imgui/backends/imgui_impl_opengl3.h"
#include <SDL3/SDL.h>
#include "stb/stb_image.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>
#include <filesystem>
#include <iostream>
#include <algorithm>

namespace fs = std::filesystem;

// ── Scenery backgrounds (game .tex.png files) ────────────────────────────
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
static const int kNumBackgrounds = sizeof(kBackgrounds) / sizeof(kBackgrounds[0]);

// ── Flavor quotes ────────────────────────────────────────────────────────
static const char* kFlavorLines[] = {
    "Loading... probably.",
    "Still faster than a dial-up connection.",
    "Hero is stretching. Give him a sec.",
    "Booting up the ancient gods...",
    "Convincing the pixels to cooperate.",
    "The mushrooms are not happy about this.",
    "Definitely not mining crypto.",
    "Asking the elder scrolls for directions.",
    "The loading bar is loading itself.",
    "Hero tripped over a polygon. Respawn in 3... 2...",
    "Injecting feels into the code.",
    "The PVR textures are staging a revolt.",
    "Hero forgot where he put his sword.",
    "The dungeon master is rolling dice.",
    "Plotting world domination... I mean, world generation.",
    "Warning: May contain traces of fun.",
    "The dragon is stuck in traffic.",
    "Converting coffee into code...",
    "The render distance is rendering itself.",
    "If this takes too long, blame the intern.",
    "Please wait. The pot on the stove is almost ready.",
    "Loading screen is loading. Recursive? Perhaps.",
    "Calibrating the wave function...",
    "The physics engine is having an existential crisis.",
    "Compiling excuses for the delay...",
    "Hero is doing push-ups to pass the time.",
    "The music hasn't loaded yet, so enjoy the silence.",
    "Asking the NPCs for tech support.",
    "The pot on the stove is almost ready.",
};
static const int kNumFlavors = sizeof(kFlavorLines) / sizeof(kFlavorLines[0]);

// ── Mote (ambient floating particle) ─────────────────────────────────────
struct Mote {
    float x, y;       // normalized 0..1 screen position
    float vx, vy;     // velocity per second
    float size;        // radius in pixels
    float alpha;       // 0..1
    float phase;       // sin phase offset
};

// ── Texture loading helpers ──────────────────────────────────────────────

static std::string get_assets_dir() {
    const char* home = getenv("HOME");
    if (!home) home = ".";
    return std::string(home) + "/.local/share/swordigo-desktop/assets/resources/";
}

static unsigned int load_texture_png(const char* path, int* out_w, int* out_h) {
    int w = 0, h = 0, ch = 0;
    unsigned char* px = stbi_load(path, &w, &h, &ch, 4);
    if (!px || w == 0 || h == 0) return 0;

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    stbi_image_free(px);

    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return tex;
}

static unsigned int load_game_texture(const char* name, int* out_w, int* out_h) {
    std::string base = get_assets_dir();

    // Try .tex.png first (stbi loads these directly)
    std::string png_path = base + name;
    unsigned int tex = load_texture_png(png_path.c_str(), out_w, out_h);
    if (tex) return tex;

    // Try .pvr fallback (strip _2x if present)
    std::string pvr_name = name;
    auto pos = pvr_name.find("_2x");
    if (pos != std::string::npos) pvr_name.erase(pos, 3);
    tex = pvr_load_texture((base + pvr_name).c_str(), out_w, out_h);
    if (tex) return tex;

    // Try original name as .pvr
    tex = pvr_load_texture((base + std::string(name)).c_str(), out_w, out_h);
    return tex;
}

static unsigned int upload_rgba_tex(const unsigned char* rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) return 0;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return tex;
}

// ── Fullscreen quad (GL 2.1 fixed-function) ──────────────────────────────

static void draw_fullscreen_quad(unsigned int tex, int tw, int th, int win_w, int win_h) {
    if (!tex || tw <= 0 || th <= 0) return;
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);

    float img_ar = (float)tw / (float)th;
    float win_ar = (float)win_w / (float)win_h;
    float qw, qh;
    if (win_ar > img_ar) {
        qw = (float)win_w;
        qh = qw / img_ar;
    } else {
        qh = (float)win_h;
        qw = qh * img_ar;
    }
    float ox = ((float)win_w - qw) * 0.5f;
    float oy = ((float)win_h - qh) * 0.5f;

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, win_w, win_h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glBegin(GL_QUADS);
    glTexCoord2f(0, 1); glVertex2f(ox, oy);
    glTexCoord2f(1, 1); glVertex2f(ox + qw, oy);
    glTexCoord2f(1, 0); glVertex2f(ox + qw, oy + qh);
    glTexCoord2f(0, 0); glVertex2f(ox, oy + qh);
    glEnd();

    glDisable(GL_TEXTURE_2D);
}

// ── POD model GL 2.1 renderer (simple, no shaders) ───────────────────────

static void draw_pod_gl21(const av::PODModel& model, int frame_idx,
                           const std::vector<GLuint>& mat_tex,
                           float cx, float cy, float scale, float time) {
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1, 1, -1, 1, -10, 10);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    // Simple walk cycle: bob up/down
    float bob = sinf(time * 6.0f) * 0.02f;
    float lean = sinf(time * 3.0f) * 0.05f;

    glTranslatef(cx, cy + bob, 0);
    glRotatef(lean * 57.3f, 0, 0, 1);
    glScalef(scale, scale, scale);

    for (size_t mi = 0; mi < model.meshes.size(); mi++) {
        const av::PODMesh& mesh = model.meshes[mi];
        if (mesh.num_vertices <= 0) continue;

        // Bind material texture
        int mat_idx = (mi < model.nodes.size()) ? model.nodes[mi].material_index : -1;
        if (mat_idx >= 0 && mat_idx < (int)mat_tex.size() && mat_tex[mat_idx]) {
            glBindTexture(GL_TEXTURE_2D, mat_tex[mat_idx]);
        } else {
            glBindTexture(GL_TEXTURE_2D, 0);
        }

        // Draw triangles
        if (!mesh.indices.empty()) {
            glBegin(GL_TRIANGLES);
            for (size_t fi = 0; fi + 2 < mesh.indices.size(); fi += 3) {
                for (int corner = 0; corner < 3; corner++) {
                    uint32_t vi = mesh.indices[fi + corner];
                    if (vi * 3 + 2 < mesh.positions.size()) {
                        float px = mesh.positions[vi * 3 + 0];
                        float py = mesh.positions[vi * 3 + 1];
                        if (vi * 2 + 1 < mesh.uvs.size()) {
                            glTexCoord2f(mesh.uvs[vi * 2], mesh.uvs[vi * 2 + 1]);
                        }
                        if (vi * 3 + 2 < mesh.normals.size()) {
                            glNormal3f(mesh.normals[vi * 3], mesh.normals[vi * 3 + 1],
                                       mesh.normals[vi * 3 + 2]);
                        }
                        glVertex3f(px, py, 0);
                    }
                }
            }
            glEnd();
        }
    }

    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
}

// ── LoadingScreen implementation ──────────────────────────────────────────

LoadingScreen::~LoadingScreen() { shutdown(); }

bool LoadingScreen::init(SDL_Window* window, void* imgui_ctx) {
    if (!window || !imgui_ctx) return false;
    window_ = window;
    imgui_ctx_ = imgui_ctx;
    inited_ = true;
    start_time_ = SDL_GetTicks() / 1000.0;

    srand((unsigned)time(nullptr));

    std::string base = get_assets_dir();

    // ── Logo: try swordigo_title PVR first, then embedded PNGs ──
    logo_tex_ = pvr_load_texture((base + "swordigo_title_2x.pvr").c_str(),
                                  &logo_w_, &logo_h_);
    if (!logo_tex_) {
        const char* fallbacks[] = {"icon_app.png", "launcher_icon.png", "startup.png"};
        for (const char* name : fallbacks) {
            std::string path = get_user_data_dir() + "/" + name;
            logo_tex_ = load_texture_png(path.c_str(), &logo_w_, &logo_h_);
            if (logo_tex_) break;
        }
    }
    if (!logo_tex_) {
        // Embedded PNG fallback
        const unsigned char* raw = nullptr;
        size_t raw_sz = 0;
        if (embedded_asset("icon_app.png", &raw, &raw_sz)) {
            unsigned char* px = nullptr;
            int w = 0, h = 0;
            if (asset_decode_image(raw, raw_sz, &px, &w, &h) && px && w > 0 && h > 0) {
                logo_tex_ = upload_rgba_tex(px, w, h);
                logo_w_ = w;
                logo_h_ = h;
                asset_image_free(px);
            }
        }
    }
    std::cout << "[LoadingScreen] Logo: " << (logo_tex_ ? "OK" : "MISSING")
              << " (" << logo_w_ << "x" << logo_h_ << ")" << std::endl;

    // ── Background scenery ──
    int bg_idx = rand() % kNumBackgrounds;
    bg_tex_ = load_game_texture(kBackgrounds[bg_idx], &bg_w_, &bg_h_);
    if (!bg_tex_) {
        // Fallback: try embedded launcher background
        const unsigned char* raw = nullptr;
        size_t raw_sz = 0;
        if (embedded_asset("launcher_bg.png", &raw, &raw_sz)) {
            unsigned char* px = nullptr;
            int w = 0, h = 0;
            if (asset_decode_image(raw, raw_sz, &px, &w, &h) && px && w > 0 && h > 0) {
                bg_tex_ = upload_rgba_tex(px, w, h);
                bg_w_ = w;
                bg_h_ = h;
                asset_image_free(px);
            }
        }
    }
    bg_slideshow_idx_ = bg_idx;
    bg_slideshow_next_ = SDL_GetTicks() / 1000.0 + 8.0; // first swap at 8s
    std::cout << "[LoadingScreen] Background: " << kBackgrounds[bg_idx]
              << " (" << bg_w_ << "x" << bg_h_ << ")" << std::endl;

    // ── Spark sprite ──
    spark_tex_ = load_texture_png((base + "spark.png").c_str(), nullptr, nullptr);

    // ── Hiro POD animation ──
    load_hiro_animation();

    // ── Motes ──
    mote_count_ = 12;
    auto* motes = new std::vector<Mote>(mote_count_);
    for (int i = 0; i < mote_count_; i++) {
        Mote& m = (*motes)[i];
        m.x = (float)(rand() % 1000) / 1000.0f;
        m.y = (float)(rand() % 1000) / 1000.0f;
        m.vx = ((float)(rand() % 200) - 100.0f) / 500.0f;
        m.vy = -((float)(rand() % 100) + 50.0f) / 500.0f;
        m.size = 1.5f + (float)(rand() % 30) / 10.0f;
        m.alpha = 0.15f + (float)(rand() % 20) / 100.0f;
        m.phase = (float)(rand() % 628) / 100.0f;
    }
    motes_ = motes;

    // ── Flavor ──
    flavor_idx_ = rand() % kNumFlavors;
    flavor_next_swap_ = SDL_GetTicks() / 1000.0 + 5.0;

    return true;
}

bool LoadingScreen::load_hiro_animation() {
    // Try multiple paths for the hiro POD
    std::string base = get_assets_dir();
    const char* candidates[] = {
        "hiro_run.pod",
        "hiro.pod",
        "hero.pod",
    };

    for (const char* name : candidates) {
        fs::path pod_path = base + name;
        if (!fs::exists(pod_path)) continue;

        try {
            auto model = new av::PODModel(av::pod_load(pod_path.string()));
            if (!model->meshes.empty()) {
                hiro_model_ = model;

                // Load per-material textures
                auto* tex_vec = new std::vector<GLuint>();
                for (const auto& mat : model->materials) {
                    GLuint tex = 0;
                    if (mat.diffuse_texture_index >= 0 &&
                        mat.diffuse_texture_index < (int)model->texture_filenames.size()) {
                        std::string tex_name = model->texture_filenames[mat.diffuse_texture_index];
                        tex = pvr_load_texture((base + tex_name).c_str());
                        if (!tex) {
                            // Try without _2x
                            auto pos = tex_name.find("_2x");
                            if (pos != std::string::npos) {
                                std::string alt = tex_name;
                                alt.erase(pos, 3);
                                tex = pvr_load_texture((base + alt).c_str());
                            }
                        }
                    }
                    tex_vec->push_back(tex);
                }
                hiro_mat_tex_ = tex_vec;
                hiro_ok_ = true;
                hiro_frame_ = 0.0f;
                std::cout << "[LoadingScreen] Hiro loaded: " << name
                          << " (" << model->meshes.size() << " meshes, "
                          << tex_vec->size() << " materials)" << std::endl;
                return true;
            }
            delete model;
        } catch (...) {
            // Non-fatal
        }
    }

    std::cout << "[LoadingScreen] Hiro not found, using spinner fallback" << std::endl;
    return false;
}

void LoadingScreen::set_progress(float p01) {
    progress_.store(p01 < 0.0f ? 0.0f : (p01 > 1.0f ? 1.0f : p01));
}

void LoadingScreen::set_stage(const std::string& stage) {
    std::lock_guard<std::mutex> lock(stage_mtx_);
    stage_ = stage;
}

void LoadingScreen::frame() {
    if (!inited_) return;

    int win_w = 0, win_h = 0;
    SDL_GetWindowSize(window_, &win_w, &win_h);
    if (win_w <= 0 || win_h <= 0) return;

    float time = (float)(SDL_GetTicks() / 1000.0 - start_time_);

    // ── GL clear ──
    glViewport(0, 0, win_w, win_h);
    glClearColor(0.02f, 0.03f, 0.05f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // ── Background scenery ──
    draw_fullscreen_quad(bg_tex_, bg_w_, bg_h_, win_w, win_h);

    // ── Dark scrim (top + bottom) for readability ──
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, win_w, win_h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    // Top gradient (stronger for logo area)
    glBegin(GL_QUADS);
    glColor4f(0.0f, 0.0f, 0.0f, 0.70f);
    glVertex2f(0, 0);
    glVertex2f(win_w, 0);
    glColor4f(0.0f, 0.0f, 0.0f, 0.30f);
    glVertex2f(win_w, win_h * 0.5f);
    glVertex2f(0, win_h * 0.5f);
    glEnd();

    // Bottom gradient
    glBegin(GL_QUADS);
    glColor4f(0.0f, 0.0f, 0.0f, 0.0f);
    glVertex2f(0, win_h * 0.6f);
    glVertex2f(win_w, win_h * 0.6f);
    glColor4f(0.0f, 0.0f, 0.0f, 0.65f);
    glVertex2f(win_w, win_h);
    glVertex2f(0, win_h);
    glEnd();

    glDisable(GL_BLEND);

    // ── Hiro animation (center, bottom third) ──
    if (hiro_ok_ && hiro_model_) {
        float hiro_scale = 0.30f; // size multiplier
        float hiro_cx = 0.0f;    // center X in NDC
        float hiro_cy = -0.15f;  // slightly below center in NDC
        hiro_frame_ += 0.03f;

        auto* model = static_cast<av::PODModel*>(hiro_model_);
        auto* tex_vec = static_cast<std::vector<GLuint>*>(hiro_mat_tex_);
        draw_pod_gl21(*model, (int)hiro_frame_, *tex_vec,
                       hiro_cx, hiro_cy, hiro_scale, time);
    } else {
        // Spinner fallback
        glDisable(GL_TEXTURE_2D);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, win_w, win_h, 0, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        float cx = win_w * 0.5f;
        float cy = win_h * 0.55f;
        float r = 20.0f;
        float angle = time * 3.0f;

        glLineWidth(3.0f);
        glBegin(GL_LINE_LOOP);
        for (int i = 0; i < 24; i++) {
            float a = angle + (float)i / 24.0f * 6.2832f;
            float alpha = 0.2f + 0.6f * ((float)i / 24.0f);
            glColor4f(0.91f, 0.27f, 0.38f, alpha);
            glVertex2f(cx + cosf(a) * r, cy + sinf(a) * r);
        }
        glEnd();
        glLineWidth(1.0f);
        glDisable(GL_BLEND);
    }

    // ── Ambient motes ──
    if (motes_ && mote_count_ > 0) {
        auto* motes = static_cast<std::vector<Mote>*>(motes_);
        glDisable(GL_TEXTURE_2D);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, win_w, win_h, 0, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        for (int i = 0; i < mote_count_; i++) {
            Mote& m = (*motes)[i];
            m.x += m.vx * 0.016f;
            m.y += m.vy * 0.016f;
            if (m.x < -0.1f) m.x = 1.1f;
            if (m.x > 1.1f) m.x = -0.1f;
            if (m.y < -0.1f) m.y = 1.1f;
            if (m.y > 1.1f) m.y = -0.1f;

            float px = m.x * win_w;
            float py = m.y * win_h;
            float pulse = 0.5f + 0.5f * sinf(time * 2.0f + m.phase);
            float a = m.alpha * pulse;

            glBegin(GL_TRIANGLE_FAN);
            glColor4f(0.91f, 0.27f, 0.38f, a);
            glVertex2f(px, py);
            for (int s = 0; s <= 8; s++) {
                float ang = (float)s / 8.0f * 6.2832f;
                glVertex2f(px + cosf(ang) * m.size, py + sinf(ang) * m.size);
            }
            glEnd();
        }
        glDisable(GL_BLEND);
    }

    // ── ImGui overlay ──
    draw_overlay();

    SDL_GL_SwapWindow(window_);
}

void LoadingScreen::draw_overlay() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    int win_w = 0, win_h = 0;
    SDL_GetWindowSize(window_, &win_w, &win_h);
    ImVec2 ds((float)win_w, (float)win_h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    double now = SDL_GetTicks() / 1000.0;

    // ── Flavor line rotation ──
    if (now > flavor_next_swap_) {
        flavor_idx_ = (flavor_idx_ + 1) % kNumFlavors;
        flavor_next_swap_ = now + 5.0;
    }

    // ── Slideshow background cycling ──
    if (bg_slideshow_mode_ && now > bg_slideshow_next_) {
        bg_slideshow_idx_ = (bg_slideshow_idx_ + 1) % kNumBackgrounds;
        // Delete old texture
        if (bg_tex_) glDeleteTextures(1, &bg_tex_);
        bg_tex_ = load_game_texture(kBackgrounds[bg_slideshow_idx_], &bg_w_, &bg_h_);
        bg_slideshow_next_ = now + 8.0;
    }

    // ── Logo (centered, upper third) ──
    if (logo_tex_ && logo_w_ > 0 && logo_h_ > 0) {
        float max_logo_w = ds.x * 0.45f;
        float max_logo_h = ds.y * 0.20f;
        float ar = (float)logo_w_ / (float)logo_h_;
        float lw, lh;
        if (max_logo_w / ar <= max_logo_h) {
            lw = max_logo_w;
            lh = lw / ar;
        } else {
            lh = max_logo_h;
            lw = lh * ar;
        }
        float lx = (ds.x - lw) * 0.5f;
        float ly = ds.y * 0.08f;

        // Spark glow behind logo
        if (spark_tex_) {
            float glow_s = lw * 1.2f;
            float glow_a = 0.15f + 0.05f * sinf(now * 2.0f);
            dl->AddImage((ImTextureID)(intptr_t)spark_tex_,
                          ImVec2(lx + lw * 0.5f - glow_s * 0.5f, ly + lh * 0.3f),
                          ImVec2(lx + lw * 0.5f + glow_s * 0.5f, ly + lh * 0.3f + glow_s),
                          ImVec2(0, 0), ImVec2(1, 1),
                          IM_COL32(233, 69, 96, (int)(glow_a * 255)));
        }

        dl->AddImage((ImTextureID)(intptr_t)logo_tex_,
                      ImVec2(lx, ly), ImVec2(lx + lw, ly + lh));
    }

    // ── Flavor line (centered, below logo) ──
    const char* flavor = kFlavorLines[flavor_idx_];
    ImVec2 fsz = ImGui::CalcTextSize(flavor);
    float flavor_alpha = 0.4f + 0.3f * sinf(now * 1.5f);
    dl->AddText(ImVec2((ds.x - fsz.x) * 0.5f, ds.y * 0.32f),
                IM_COL32(180, 190, 210, (int)(flavor_alpha * 255)), flavor);

    // ── Progress bar (bottom area) ──
    float bar_w = ds.x * 0.50f;
    float bar_h = 6.0f;
    float bar_x = (ds.x - bar_w) * 0.5f;
    float bar_y = ds.y - 70.0f;

    // Track
    dl->AddRectFilled(ImVec2(bar_x, bar_y), ImVec2(bar_x + bar_w, bar_y + bar_h),
                       IM_COL32(40, 50, 70, 200), 3.0f);
    // Fill
    float pct = progress_.load();
    if (pct > 0.001f) {
        dl->AddRectFilled(ImVec2(bar_x, bar_y), ImVec2(bar_x + bar_w * pct, bar_y + bar_h),
                           IM_COL32(233, 69, 96, 230), 3.0f);
        // Glow on fill tip
        if (spark_tex_) {
            float tip_x = bar_x + bar_w * pct;
            float glow_s = 12.0f;
            float glow_a = 0.3f + 0.2f * sinf(now * 4.0f);
            dl->AddImage((ImTextureID)(intptr_t)spark_tex_,
                          ImVec2(tip_x - glow_s, bar_y - glow_s),
                          ImVec2(tip_x + glow_s, bar_y + bar_h + glow_s),
                          ImVec2(0, 0), ImVec2(1, 1),
                          IM_COL32(233, 69, 96, (int)(glow_a * 255)));
        }
    }

    // Stage text above bar
    std::string stage_text;
    {
        std::lock_guard<std::mutex> lock(stage_mtx_);
        stage_text = stage_;
    }
    ImVec2 ssz = ImGui::CalcTextSize(stage_text.c_str());
    dl->AddText(ImVec2((ds.x - ssz.x) * 0.5f, bar_y - 22.0f),
                IM_COL32(130, 140, 160, 180), stage_text.c_str());

    // ── Watchdog messages ──
    float elapsed_secs = (float)now - (float)start_time_;
    if (elapsed_secs > 30.0f && last_watchdog_ < 2) {
        dl->AddText(ImVec2(16, ds.y - 40), IM_COL32(200, 100, 80, 160),
                    "Taking longer than expected... Hang tight!");
        last_watchdog_ = 2;
    } else if (elapsed_secs > 20.0f && last_watchdog_ < 1) {
        dl->AddText(ImVec2(16, ds.y - 40), IM_COL32(180, 150, 80, 140),
                    "Still loading... The dungeon is deep.");
        last_watchdog_ = 1;
    } else if (elapsed_secs > 10.0f && last_watchdog_ < 0) {
        dl->AddText(ImVec2(16, ds.y - 40), IM_COL32(150, 150, 160, 120),
                    "Loading...");
        last_watchdog_ = 0;
    }

    // Version tag
    dl->AddText(ImVec2(ds.x - 120.0f, ds.y - 20.0f),
                IM_COL32(80, 90, 110, 100), "v8.0 Remaster");

    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void LoadingScreen::finish() {
    last_watchdog_ = 99; // suppress further watchdog
}

void LoadingScreen::shutdown() {
    if (!inited_) return;

    if (bg_tex_) { glDeleteTextures(1, &bg_tex_); bg_tex_ = 0; }
    if (logo_tex_) { glDeleteTextures(1, &logo_tex_); logo_tex_ = 0; }
    if (spark_tex_) { glDeleteTextures(1, &spark_tex_); spark_tex_ = 0; }

    if (hiro_model_) {
        delete static_cast<av::PODModel*>(hiro_model_);
        hiro_model_ = nullptr;
    }
    if (hiro_mat_tex_) {
        auto* tex_vec = static_cast<std::vector<GLuint>*>(hiro_mat_tex_);
        for (GLuint t : *tex_vec) {
            if (t) glDeleteTextures(1, &t);
        }
        delete tex_vec;
        hiro_mat_tex_ = nullptr;
    }

    if (motes_) {
        delete static_cast<std::vector<Mote>*>(motes_);
        motes_ = nullptr;
    }

    inited_ = false;
}

float LoadingScreen::elapsed() const {
    if (!inited_) return 0.0f;
    return (float)(SDL_GetTicks() / 1000.0 - start_time_);
}
