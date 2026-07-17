#include "video_background.h"
#include "platform/data_path.h"
#include "platform/gl_inc.h"
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <filesystem>
#include <unordered_map>
#include <vector>
#include <iostream>
#include <cstdlib>
#include <cstdio>

// Define global void fill color
VoidFillColor g_void_fill_color;
bool g_video_background_enabled = false; // Vanilla by default

namespace VideoBackground {
    struct PlayerState {
        uint32_t tex_id;
        std::string mp4_path;
        std::string temp_dir;
        std::string vanilla_path;
        int width;
        int height;
        int frame_count;
        double current_time;
        int last_frame_idx;
        double fps;
        bool vanilla_restored;
    };

    static std::unordered_map<uint32_t, PlayerState> g_players;

    void register_texture_maybe(uint32_t tex_id, const std::string& asset_filename, int width, int height) {
        std::string base = asset_filename;
        // Strip texture extensions
        std::string clean_bare = asset_filename;
        std::vector<std::string> extensions = { ".tex.png", ".pvr.png", ".png", ".pvr", ".jpg", ".jpeg" };
        for (const auto& ext : extensions) {
            if (clean_bare.size() >= ext.size() && clean_bare.compare(clean_bare.size() - ext.size(), ext.size(), ext) == 0) {
                clean_bare = clean_bare.substr(0, clean_bare.size() - ext.size());
                break;
            }
        }

        // Strip virtual folder prefixes
        if (clean_bare.rfind("assets/resources/", 0) == 0) {
            clean_bare = clean_bare.substr(17);
        } else if (clean_bare.rfind("resources/", 0) == 0) {
            clean_bare = clean_bare.substr(10);
        }

        // Get filename only
        std::string filename_only = clean_bare;
        size_t last_slash = clean_bare.rfind('/');
        if (last_slash != std::string::npos) {
            filename_only = clean_bare.substr(last_slash + 1);
        }

        // Generate name variations (with and without _2x suffix) to handle DPI/Retina asset mismatches
        std::vector<std::string> bares = { clean_bare };
        size_t pos_2x = clean_bare.rfind("_2x");
        if (pos_2x != std::string::npos && pos_2x == clean_bare.size() - 3) {
            bares.push_back(clean_bare.substr(0, clean_bare.size() - 3));
        } else {
            bares.push_back(clean_bare + "_2x");
        }

        std::vector<std::string> filenames = { filename_only };
        size_t pos_2x_f = filename_only.rfind("_2x");
        if (pos_2x_f != std::string::npos && pos_2x_f == filename_only.size() - 3) {
            filenames.push_back(filename_only.substr(0, filename_only.size() - 3));
        } else {
            filenames.push_back(filename_only + "_2x");
        }

        std::string mp4_path;
        {
            std::vector<std::string> candidates;
            for (const auto& b : bares) {
                candidates.push_back(get_data_path("assets/resources/" + b + ".mp4"));
                candidates.push_back(get_data_path("assets/resources/background/" + b + ".mp4"));
                candidates.push_back(get_data_path("assets/resources/backgrounds/" + b + ".mp4"));
            }
            for (const auto& f : filenames) {
                candidates.push_back(get_data_path("assets/resources/background/" + f + ".mp4"));
                candidates.push_back(get_data_path("assets/resources/backgrounds/" + f + ".mp4"));
            }

            for (const auto& cand : candidates) {
                if (std::filesystem::exists(cand)) {
                    mp4_path = cand;
                    break;
                }
            }

            if (mp4_path.empty()) {
                // No video companion found. Try to sample the vanilla texture if it's a background texture.
                if (base.find("background") != std::string::npos || base.find("bg") != std::string::npos || base.find("menu") != std::string::npos || base.find("back") != std::string::npos) {
                    std::string vanilla_path = get_data_path("assets/" + base);
                    if (std::filesystem::exists(vanilla_path)) {
                        SDL_Surface* surf = IMG_Load(vanilla_path.c_str());
                        if (surf) {
                            SDL_Surface* rgba = SDL_ConvertSurface(surf, SDL_PIXELFORMAT_ABGR8888);
                            SDL_DestroySurface(surf);
                            if (rgba) {
                                const uint8_t* pix    = (const uint8_t*)rgba->pixels;
                                const int       pitch  = rgba->pitch;
                                const int       W      = rgba->w;
                                const int       H      = rgba->h;
                                const int       STRIDE = 8;
                                const int       DEPTH  = 4;

                                long long sumR = 0, sumG = 0, sumB = 0, cnt = 0;

                                // Bottom edge
                                for (int d = 0; d < DEPTH; d++)
                                    for (int x = 0; x < W; x += STRIDE) {
                                        const uint8_t* p = pix + d * pitch + x * 4;
                                        sumR += p[0]; sumG += p[1]; sumB += p[2]; cnt++;
                                    }
                                // Top edge
                                for (int d = 0; d < DEPTH; d++)
                                    for (int x = 0; x < W; x += STRIDE) {
                                        const uint8_t* p = pix + (H - 1 - d) * pitch + x * 4;
                                        sumR += p[0]; sumG += p[1]; sumB += p[2]; cnt++;
                                    }
                                // Left edge
                                for (int d = 0; d < DEPTH; d++)
                                    for (int y = 0; y < H; y += STRIDE) {
                                        const uint8_t* p = pix + y * pitch + d * 4;
                                        sumR += p[0]; sumG += p[1]; sumB += p[2]; cnt++;
                                    }
                                // Right edge
                                for (int d = 0; d < DEPTH; d++)
                                    for (int y = 0; y < H; y += STRIDE) {
                                        const uint8_t* p = pix + y * pitch + (W - 1 - d) * 4;
                                        sumR += p[0]; sumG += p[1]; sumB += p[2]; cnt++;
                                    }

                                if (cnt > 0) {
                                    g_void_fill_color.r = sumR / (255.0f * cnt);
                                    g_void_fill_color.g = sumG / (255.0f * cnt);
                                    g_void_fill_color.b = sumB / (255.0f * cnt);
                                    g_void_fill_color.active = true;
                                    std::cout << "[VideoBackground] Sampled vanilla edge color for '" << asset_filename 
                                              << "' -> (" << g_void_fill_color.r << ", " << g_void_fill_color.g << ", " << g_void_fill_color.b << ")" << std::endl;
                                    // Apply clear color immediately
                                    glClearColor(g_void_fill_color.r, g_void_fill_color.g, g_void_fill_color.b, 1.0f);
                                }
                                SDL_DestroySurface(rgba);
                            }
                        }
                    }
                }
                return; // No video companion found — use vanilla texture
            }
        }

        std::cout << "[VideoBackground] Found video for '" << asset_filename
                  << "' -> " << mp4_path << " (tex=" << tex_id << " " << width << "x" << height << ")" << std::endl;

        // Clean up existing player state for this tex_id if any
        if (g_players.count(tex_id)) {
            try { std::filesystem::remove_all(g_players[tex_id].temp_dir); } catch (...) {}
            g_players.erase(tex_id);
        }

        // Create a unique temporary directory for this texture's frames
        std::string temp_dir = "/tmp/swordigo_bg_" + std::to_string(tex_id);
        try { std::filesystem::create_directories(temp_dir); } catch (...) {
            std::cerr << "[VideoBackground] Failed to create temp dir: " << temp_dir << std::endl;
            return;
        }

        // Run FFmpeg shell command to extract frames:
        //   - scale to fit inside WxH preserving aspect ratio (no zoom/stretch)
        //   - pad to exact WxH with transparent edges so texture upload always matches
        //   - vflip → OpenGL/Swordigo textures are stored bottom-up, pre-flip once here
        //   - -r 30 → 30 fps output
        std::string vf = "scale=" + std::to_string(width) + ":" + std::to_string(height)
                       + ":force_original_aspect_ratio=decrease"
                       + ",pad=" + std::to_string(width) + ":" + std::to_string(height)
                       + ":(ow-iw)/2:(oh-ih)/2:color=black"
                       + ",vflip";
        std::string cmd = "ffmpeg -y -i \"" + mp4_path + "\" -vf \"" + vf + "\" -r 30 \""
                        + temp_dir + "/frame_%04d.png\" > /dev/null 2>&1";
        
        std::cout << "[VideoBackground] Extracting frames with FFmpeg..." << std::endl;
        int ret = std::system(cmd.c_str());
        if (ret != 0) {
            std::cerr << "[VideoBackground] FFmpeg frame extraction failed. Falling back to static texture." << std::endl;
            try { std::filesystem::remove_all(temp_dir); } catch (...) {}
            return;
        }

        // Count extracted frames
        int frame_count = 0;
        while (true) {
            char path[512];
            snprintf(path, sizeof(path), "%s/frame_%04d.png", temp_dir.c_str(), frame_count + 1);
            if (std::filesystem::exists(path)) {
                frame_count++;
            } else {
                break;
            }
        }

        if (frame_count == 0) {
            std::cerr << "[VideoBackground] No frames extracted. Falling back to static texture." << std::endl;
            try { std::filesystem::remove_all(temp_dir); } catch (...) {}
            return;
        }

        PlayerState state;
        state.tex_id = tex_id;
        state.mp4_path = mp4_path;
        state.temp_dir = temp_dir;
        state.vanilla_path = get_data_path("assets/" + base);
        state.width = width;
        state.height = height;
        state.frame_count = frame_count;
        state.current_time = 0.0;
        state.last_frame_idx = -1;
        state.fps = 30.0; // matching -r 30 in ffmpeg
        state.vanilla_restored = true; // Default starts as vanilla

        g_players[tex_id] = state;
        std::cout << "[VideoBackground] Registered video texture " << tex_id 
                  << " with " << frame_count << " frames." << std::endl;
    }

    void update_texture_maybe(uint32_t tex_id) {
        if (!g_players.count(tex_id)) return;

        PlayerState& state = g_players[tex_id];

        if (!g_video_background_enabled) {
            if (!state.vanilla_restored) {
                // Re-upload vanilla background texture to GPU
                SDL_Surface* surf = IMG_Load(state.vanilla_path.c_str());
                if (surf) {
                    SDL_Surface* rgba = SDL_ConvertSurface(surf, SDL_PIXELFORMAT_ABGR8888);
                    SDL_DestroySurface(surf);
                    if (rgba) {
                        SDL_Surface* sized = rgba;
                        if (rgba->w != state.width || rgba->h != state.height) {
                            sized = SDL_ScaleSurface(rgba, state.width, state.height, SDL_SCALEMODE_LINEAR);
                            SDL_DestroySurface(rgba);
                        }
                        if (sized) {
                            glBindTexture(GL_TEXTURE_2D, tex_id);
                            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, sized->w, sized->h,
                                            GL_RGBA, GL_UNSIGNED_BYTE, sized->pixels);
                            SDL_DestroySurface(sized);
                        }
                    }
                } else {
                    std::cerr << "[VideoBackground] Error: Failed to load vanilla background from path: " 
                              << state.vanilla_path << " - SDL_image error: " << SDL_GetError() << std::endl;
                }
                state.vanilla_restored = true;
                state.last_frame_idx = -1;
                std::cout << "[VideoBackground] Restored vanilla background texture for tex_id " << tex_id << std::endl;
            }
            return;
        }

        // Track time delta
        static Uint64 last_ticks = 0;
        Uint64 now_ticks = SDL_GetTicks();
        float dt = 0.0f;
        if (last_ticks != 0) {
            dt = (now_ticks - last_ticks) / 1000.0f;
            if (dt > 0.1f) dt = 0.016666668f;
            if (dt < 0.0f) dt = 0.0f;
        } else {
            dt = 0.016666668f;
        }
        last_ticks = now_ticks;

        // Play at 0.4x speed
        state.current_time += dt * 0.4;

        // Calculate frame index (1-based)
        int frame_idx = (int)(state.current_time * state.fps) % state.frame_count + 1;

        if (frame_idx != state.last_frame_idx) {
            char path[512];
            snprintf(path, sizeof(path), "%s/frame_%04d.png", state.temp_dir.c_str(), frame_idx);

            SDL_Surface* surf = IMG_Load(path);
            if (surf) {
                // Safety resize: ensure frame exactly matches the registered texture dimensions
                SDL_Surface* sized = surf;
                if (surf->w != state.width || surf->h != state.height) {
                    sized = SDL_ScaleSurface(surf, state.width, state.height, SDL_SCALEMODE_LINEAR);
                    SDL_DestroySurface(surf);
                    if (!sized) { state.last_frame_idx = frame_idx; return; }
                }

                // Convert to ABGR8888 (matches glTexSubImage2D GL_RGBA on little-endian)
                SDL_Surface* rgba = SDL_ConvertSurface(sized, SDL_PIXELFORMAT_ABGR8888);
                if (sized != surf) SDL_DestroySurface(sized);
                else SDL_DestroySurface(surf);

                if (rgba) {
                    glBindTexture(GL_TEXTURE_2D, tex_id);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, rgba->w, rgba->h,
                                    GL_RGBA, GL_UNSIGNED_BYTE, rgba->pixels);

                    // We successfully uploaded a video frame; flag vanilla as needing restore
                    state.vanilla_restored = false;

                    // ---- Edge-void fill ----
                    // Sample the four edges of the uploaded frame, then set glClearColor
                    // to the average colour.  When the camera pans beyond the background
                    // quad (player jumps, etc.) OpenGL's clear colour fills the void with
                    // a hue that blends naturally instead of showing raw black.
                    //
                    // SDL_PIXELFORMAT_ABGR8888 on little-endian: bytes in memory are
                    // [R, G, B, A] per pixel, matching GL_RGBA / GL_UNSIGNED_BYTE.
                    {
                        const uint8_t* pix    = (const uint8_t*)rgba->pixels;
                        const int       pitch  = rgba->pitch;
                        const int       W      = rgba->w;
                        const int       H      = rgba->h;
                        const int       STRIDE = 8;   // sample every 8 px along each edge
                        const int       DEPTH  = 4;   // sample 4 rows/cols in from each edge

                        long long sumR = 0, sumG = 0, sumB = 0, cnt = 0;

                        // Bottom edge (row 0 in memory = bottom of displayed quad post-vflip)
                        for (int d = 0; d < DEPTH; d++)
                            for (int x = 0; x < W; x += STRIDE) {
                                const uint8_t* p = pix + d * pitch + x * 4;
                                sumR += p[0]; sumG += p[1]; sumB += p[2]; cnt++;
                            }
                        // Top edge
                        for (int d = 0; d < DEPTH; d++)
                            for (int x = 0; x < W; x += STRIDE) {
                                const uint8_t* p = pix + (H - 1 - d) * pitch + x * 4;
                                sumR += p[0]; sumG += p[1]; sumB += p[2]; cnt++;
                            }
                        // Left edge
                        for (int d = 0; d < DEPTH; d++)
                            for (int y = 0; y < H; y += STRIDE) {
                                const uint8_t* p = pix + y * pitch + d * 4;
                                sumR += p[0]; sumG += p[1]; sumB += p[2]; cnt++;
                            }
                        // Right edge
                        for (int d = 0; d < DEPTH; d++)
                            for (int y = 0; y < H; y += STRIDE) {
                                const uint8_t* p = pix + y * pitch + (W - 1 - d) * 4;
                                sumR += p[0]; sumG += p[1]; sumB += p[2]; cnt++;
                            }

                        if (cnt > 0) {
                            g_void_fill_color.r = sumR / (255.0f * cnt);
                            g_void_fill_color.g = sumG / (255.0f * cnt);
                            g_void_fill_color.b = sumB / (255.0f * cnt);
                            g_void_fill_color.active = true;
                            glClearColor(g_void_fill_color.r, g_void_fill_color.g, g_void_fill_color.b, 1.0f);
                        }
                    }
                    // ---- end edge-void fill ----

                    SDL_DestroySurface(rgba);
                }

            }
            state.last_frame_idx = frame_idx;
        }
    }

    void cleanup() {
        for (auto& pair : g_players) {
            try { std::filesystem::remove_all(pair.second.temp_dir); } catch (...) {}
        }
        g_players.clear();
    }
}

extern "C" void reset_void_fill_color() {
    g_void_fill_color.active = false;
}


