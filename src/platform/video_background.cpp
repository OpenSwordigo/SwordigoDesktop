#include "video_background.h"
#include "platform/data_path.h"
#include "platform/gl_inc.h"
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <filesystem>
#include <unordered_map>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>
#include <iostream>
#include <cstdlib>
#include <cstdio>
#include <chrono>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

// Define global void fill color
VoidFillColor g_void_fill_color;
bool g_video_background_enabled = false; // Vanilla by default

extern std::string g_instance_assets_dir;

namespace VideoBackground {

    // Direct memory-to-GPU multi-threaded FFmpeg decoder
    class VideoDecoder {
    private:
        std::string mp4_path;
        int dst_width;
        int dst_height;

        AVFormatContext* format_ctx = nullptr;
        AVCodecContext* codec_ctx = nullptr;
        int video_stream_idx = -1;
        SwsContext* sws_ctx = nullptr;
        AVFrame* av_frame = nullptr;
        AVPacket* av_packet = nullptr;

        std::deque<std::vector<uint8_t>> frame_queue;
        std::mutex queue_mutex;
        std::condition_variable queue_cv;
        std::thread decoder_thread;
        std::atomic<bool> stop_thread{false};

        void decoder_loop() {
            std::vector<uint8_t> rgb_buffer(dst_width * dst_height * 4);

            while (!stop_thread) {
                // Sleep if video backgrounds are disabled to save CPU cycles
                if (!g_video_background_enabled) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }

                // Limit queue capacity to 24 frames
                {
                    std::unique_lock<std::mutex> lock(queue_mutex);
                    queue_cv.wait(lock, [&]() {
                        return frame_queue.size() < 24 || stop_thread || !g_video_background_enabled;
                    });
                }
                if (stop_thread) break;
                if (!g_video_background_enabled) continue;

                int ret = av_read_frame(format_ctx, av_packet);
                if (ret < 0) {
                    // Loop the video seamlessly
                    av_seek_frame(format_ctx, video_stream_idx, 0, AVSEEK_FLAG_BACKWARD);
                    avcodec_flush_buffers(codec_ctx);
                    av_packet_unref(av_packet);
                    continue;
                }

                if (av_packet->stream_index == video_stream_idx) {
                    ret = avcodec_send_packet(codec_ctx, av_packet);
                    if (ret >= 0) {
                        while (ret >= 0) {
                            ret = avcodec_receive_frame(codec_ctx, av_frame);
                            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                                break;
                            } else if (ret < 0) {
                                break;
                            }

                            // Convert frame to RGBA & perform vertical flip inside sws_scale
                            uint8_t* flip_data[4] = { rgb_buffer.data() + (dst_height - 1) * dst_width * 4, nullptr, nullptr, nullptr };
                            int flip_linesize[4] = { -dst_width * 4, 0, 0, 0 };

                            sws_scale(sws_ctx, av_frame->data, av_frame->linesize, 0, codec_ctx->height,
                                      flip_data, flip_linesize);

                            {
                                std::lock_guard<std::mutex> lock(queue_mutex);
                                frame_queue.push_back(rgb_buffer);
                            }
                            queue_cv.notify_all();
                        }
                    }
                }
                av_packet_unref(av_packet);
            }
        }

    public:
        VideoDecoder(const std::string& path, int w, int h)
            : mp4_path(path), dst_width(w), dst_height(h) {}

        ~VideoDecoder() {
            stop();
            cleanup();
        }

        bool init() {
            int err = avformat_open_input(&format_ctx, mp4_path.c_str(), nullptr, nullptr);
            if (err != 0) {
                char err_buf[256];
                av_strerror(err, err_buf, sizeof(err_buf));
                std::cerr << "[VideoDecoder] Failed to open MP4: " << mp4_path 
                          << " (error: " << err_buf << ")" << std::endl;
                return false;
            }

            if (avformat_find_stream_info(format_ctx, nullptr) < 0) {
                std::cerr << "[VideoDecoder] Failed to find stream info for " << mp4_path << std::endl;
                return false;
            }

            for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
                if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                    video_stream_idx = i;
                    break;
                }
            }

            if (video_stream_idx == -1) {
                std::cerr << "[VideoDecoder] No video stream found in " << mp4_path << std::endl;
                return false;
            }

            AVCodecParameters* codecpar = format_ctx->streams[video_stream_idx]->codecpar;
            const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
            if (!codec) {
                std::cerr << "[VideoDecoder] Codec not found for " << mp4_path << std::endl;
                return false;
            }

            codec_ctx = avcodec_alloc_context3(codec);
            if (avcodec_parameters_to_context(codec_ctx, codecpar) < 0) {
                std::cerr << "[VideoDecoder] Failed to copy codec context for " << mp4_path << std::endl;
                return false;
            }

            codec_ctx->thread_count = 0; // Multithreaded decoding

            if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
                std::cerr << "[VideoDecoder] Failed to open codec for " << mp4_path << std::endl;
                return false;
            }

            av_frame = av_frame_alloc();
            av_packet = av_packet_alloc();
            if (!av_frame || !av_packet) {
                std::cerr << "[VideoDecoder] Failed to allocate frame or packet for " << mp4_path << std::endl;
                return false;
            }

            sws_ctx = sws_getContext(
                codec_ctx->width, codec_ctx->height, codec_ctx->pix_fmt,
                dst_width, dst_height, AV_PIX_FMT_RGBA,
                SWS_BILINEAR, nullptr, nullptr, nullptr
            );

            if (!sws_ctx) {
                std::cerr << "[VideoDecoder] Failed to create SwsContext for " << mp4_path << std::endl;
                return false;
            }

            return true;
        }

        bool decode_first_frame(std::vector<uint8_t>& out_pixels) {
            out_pixels.resize(dst_width * dst_height * 4);
            bool decoded = false;

            while (av_read_frame(format_ctx, av_packet) >= 0) {
                if (av_packet->stream_index == video_stream_idx) {
                    if (avcodec_send_packet(codec_ctx, av_packet) >= 0) {
                        if (avcodec_receive_frame(codec_ctx, av_frame) >= 0) {
                            uint8_t* flip_data[4] = { out_pixels.data() + (dst_height - 1) * dst_width * 4, nullptr, nullptr, nullptr };
                            int flip_linesize[4] = { -dst_width * 4, 0, 0, 0 };

                            sws_scale(sws_ctx, av_frame->data, av_frame->linesize, 0, codec_ctx->height,
                                      flip_data, flip_linesize);
                            decoded = true;
                            av_packet_unref(av_packet);
                            break;
                        }
                    }
                }
                av_packet_unref(av_packet);
            }

            return decoded;
        }

        void start() {
            stop_thread = false;
            decoder_thread = std::thread(&VideoDecoder::decoder_loop, this);
        }

        void stop() {
            stop_thread = true;
            queue_cv.notify_all();
            if (decoder_thread.joinable()) {
                decoder_thread.join();
            }
        }

        bool pop_frame(std::vector<uint8_t>& out_pixels) {
            std::lock_guard<std::mutex> lock(queue_mutex);
            if (frame_queue.empty()) {
                return false;
            }
            out_pixels = std::move(frame_queue.front());
            frame_queue.pop_front();
            queue_cv.notify_all();
            return true;
        }

        size_t get_queue_size() {
            std::lock_guard<std::mutex> lock(queue_mutex);
            return frame_queue.size();
        }

        bool is_running() {
            return !stop_thread;
        }

        void cleanup() {
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                frame_queue.clear();
            }
            if (sws_ctx) {
                sws_freeContext(sws_ctx);
                sws_ctx = nullptr;
            }
            if (av_frame) {
                av_frame_free(&av_frame);
                av_frame = nullptr;
            }
            if (av_packet) {
                av_packet_free(&av_packet);
                av_packet = nullptr;
            }
            if (codec_ctx) {
                avcodec_free_context(&codec_ctx);
                codec_ctx = nullptr;
            }
            if (format_ctx) {
                avformat_close_input(&format_ctx);
                format_ctx = nullptr;
            }
        }
    };

    struct PlayerState {
        uint32_t tex_id = 0;
        std::string mp4_path;
        std::string vanilla_path;
        int width = 0;
        int height = 0;
        bool vanilla_restored = true;
        uint64_t last_ticks = 0;

        std::vector<uint8_t> vanilla_pixels;
        uint32_t vanilla_format = 0;
        uint32_t vanilla_type = 0;

        std::unique_ptr<VideoDecoder> decoder;

        double frame_duration = 1.0 / 30.0;
        double time_accumulator = 0.0;
    };

    static std::unordered_map<uint32_t, std::unique_ptr<PlayerState>> g_players;

    void register_texture_maybe(uint32_t tex_id, const std::string& asset_filename, int width, int height, const void* pixels, uint32_t format, uint32_t type) {
        std::string base = asset_filename;
        std::string clean_bare = asset_filename;
        std::vector<std::string> extensions = { ".tex.png", ".pvr.png", ".png", ".pvr", ".jpg", ".jpeg" };
        for (const auto& ext : extensions) {
            if (clean_bare.size() >= ext.size() && clean_bare.compare(clean_bare.size() - ext.size(), ext.size(), ext) == 0) {
                clean_bare = clean_bare.substr(0, clean_bare.size() - ext.size());
                break;
            }
        }

        if (clean_bare.rfind("assets/resources/", 0) == 0) {
            clean_bare = clean_bare.substr(17);
        } else if (clean_bare.rfind("resources/", 0) == 0) {
            clean_bare = clean_bare.substr(10);
        }

        std::string filename_only = clean_bare;
        size_t last_slash = clean_bare.rfind('/');
        if (last_slash != std::string::npos) {
            filename_only = clean_bare.substr(last_slash + 1);
        }

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
                candidates.push_back(get_data_path(g_instance_assets_dir + "/resources/" + b + ".mp4"));
                candidates.push_back(get_data_path(g_instance_assets_dir + "/resources/background/" + b + ".mp4"));
                candidates.push_back(get_data_path(g_instance_assets_dir + "/resources/backgrounds/" + b + ".mp4"));
            }
            for (const auto& f : filenames) {
                candidates.push_back(get_data_path(g_instance_assets_dir + "/resources/background/" + f + ".mp4"));
                candidates.push_back(get_data_path(g_instance_assets_dir + "/resources/backgrounds/" + f + ".mp4"));
            }

            for (const auto& cand : candidates) {
                if (std::filesystem::exists(cand)) {
                    mp4_path = cand;
                    break;
                }
            }

            if (mp4_path.empty()) {
                if (base.find("background") != std::string::npos || base.find("bg") != std::string::npos || base.find("menu") != std::string::npos || base.find("back") != std::string::npos) {
                    // Sample vanilla edge color from the pixels passed
                    if (pixels) {
                        int channels = 4;
                        if (format == GL_RGB) channels = 3;
                        else if (format == GL_LUMINANCE_ALPHA) channels = 2;
                        else if (format == GL_LUMINANCE || format == GL_ALPHA) channels = 1;

                        int bytes_per_pixel = channels; // assume GL_UNSIGNED_BYTE
                        if (type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_4_4_4_4 || type == GL_UNSIGNED_SHORT_5_5_5_1) {
                            bytes_per_pixel = 2;
                        }

                        const uint8_t* pix    = (const uint8_t*)pixels;
                        const int       pitch  = width * bytes_per_pixel;
                        const int       W      = width;
                        const int       H      = height;
                        const int       STRIDE = 8;
                        const int       DEPTH  = 4;

                        long long sumR = 0, sumG = 0, sumB = 0, cnt = 0;

                        for (int d = 0; d < DEPTH; d++)
                            for (int x = 0; x < W; x += STRIDE) {
                                const uint8_t* p = pix + d * pitch + x * bytes_per_pixel;
                                sumR += p[0]; sumG += p[1]; sumB += p[2]; cnt++;
                            }
                        for (int d = 0; d < DEPTH; d++)
                            for (int x = 0; x < W; x += STRIDE) {
                                const uint8_t* p = pix + (H - 1 - d) * pitch + x * bytes_per_pixel;
                                sumR += p[0]; sumG += p[1]; sumB += p[2]; cnt++;
                            }
                        for (int d = 0; d < DEPTH; d++)
                            for (int y = 0; y < H; y += STRIDE) {
                                const uint8_t* p = pix + y * pitch + d * bytes_per_pixel;
                                sumR += p[0]; sumG += p[1]; sumB += p[2]; cnt++;
                            }
                        for (int d = 0; d < DEPTH; d++)
                            for (int y = 0; y < H; y += STRIDE) {
                                const uint8_t* p = pix + y * pitch + (W - 1 - d) * bytes_per_pixel;
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
                }
                return;
            }
        }

        std::cout << "[VideoBackground] Found video for '" << asset_filename
                  << "' -> " << mp4_path << " (tex=" << tex_id << " " << width << "x" << height << ")" << std::endl;

        if (g_players.count(tex_id)) {
            g_players.erase(tex_id);
        }

        // Create VideoDecoder
        auto decoder = std::make_unique<VideoDecoder>(mp4_path, width, height);
        if (!decoder->init()) {
            std::cerr << "[VideoBackground] Failed to initialize decoder for " << mp4_path << std::endl;
            return;
        }

        auto state = std::make_unique<PlayerState>();
        state->tex_id = tex_id;
        state->mp4_path = mp4_path;
        state->vanilla_path = get_data_path(g_instance_assets_dir + "/" + base);
        state->width = width;
        state->height = height;
        state->decoder = std::move(decoder);

        // Keep a copy of vanilla texture pixels for zero-I/O background toggle restoration
        if (pixels) {
            int channels = 4;
            if (format == GL_RGB) channels = 3;
            else if (format == GL_LUMINANCE_ALPHA) channels = 2;
            else if (format == GL_LUMINANCE || format == GL_ALPHA) channels = 1;

            int bytes_per_pixel = channels; // assume GL_UNSIGNED_BYTE
            if (type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_4_4_4_4 || type == GL_UNSIGNED_SHORT_5_5_5_1) {
                bytes_per_pixel = 2;
            }

            int size_bytes = width * height * bytes_per_pixel;
            state->vanilla_pixels.assign((const uint8_t*)pixels, (const uint8_t*)pixels + size_bytes);
            state->vanilla_format = format;
            state->vanilla_type = type;
        }

        // If video background is enabled from the start, decode and display first frame instantly
        if (g_video_background_enabled) {
            std::vector<uint8_t> first_frame_pixels;
            if (state->decoder->decode_first_frame(first_frame_pixels)) {
                glBindTexture(GL_TEXTURE_2D, tex_id);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, first_frame_pixels.data());

                // Sample edge void color
                {
                    const uint8_t* pix    = first_frame_pixels.data();
                    const int       pitch  = width * 4;
                    const int       W      = width;
                    const int       H      = height;
                    const int       STRIDE = 8;
                    const int       DEPTH  = 4;

                    long long sumR = 0, sumG = 0, sumB = 0, cnt = 0;

                    for (int d = 0; d < DEPTH; d++)
                        for (int x = 0; x < W; x += STRIDE) {
                            const uint8_t* p = pix + d * pitch + x * 4;
                            sumR += p[0]; sumG += p[1]; sumB += p[2]; cnt++;
                        }
                    for (int d = 0; d < DEPTH; d++)
                        for (int x = 0; x < W; x += STRIDE) {
                            const uint8_t* p = pix + (H - 1 - d) * pitch + x * 4;
                            sumR += p[0]; sumG += p[1]; sumB += p[2]; cnt++;
                        }
                    for (int d = 0; d < DEPTH; d++)
                        for (int y = 0; y < H; y += STRIDE) {
                            const uint8_t* p = pix + y * pitch + d * 4;
                            sumR += p[0]; sumG += p[1]; sumB += p[2]; cnt++;
                        }
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
            }
            state->vanilla_restored = false;
        } else {
            // Normal static background by default
            state->vanilla_restored = true;
        }

        // Start background decoding thread
        state->decoder->start();

        g_players[tex_id] = std::move(state);
        std::cout << "[VideoBackground] Registered video decoder for texture " << tex_id << std::endl;
    }

    void update_texture_maybe(uint32_t tex_id) {
        if (!g_players.count(tex_id)) return;

        PlayerState& state = *g_players[tex_id];

        if (!g_video_background_enabled) {
            if (!state.vanilla_restored) {
                if (!state.vanilla_pixels.empty()) {
                    glBindTexture(GL_TEXTURE_2D, tex_id);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, state.width, state.height,
                                    state.vanilla_format, state.vanilla_type, state.vanilla_pixels.data());
                }
                g_void_fill_color.active = false; // Turn off border color overrides
                state.vanilla_restored = true;
            }
            return;
        }

        // Track time delta
        Uint64 now_ticks = SDL_GetTicks();
        float dt = 0.0f;
        if (state.last_ticks != 0) {
            dt = (now_ticks - state.last_ticks) / 1000.0f;
            if (dt > 0.1f) dt = 0.016666668f;
            if (dt < 0.0f) dt = 0.0f;
        } else {
            dt = 0.016666668f;
        }
        state.last_ticks = now_ticks;

        // Play at 0.4x speed
        state.time_accumulator += dt * 0.4;

        bool updated = false;
        std::vector<uint8_t> frame_pixels;

        // Check if we need to advance frames
        while (state.time_accumulator >= state.frame_duration) {
            state.time_accumulator -= state.frame_duration;
            if (state.decoder->pop_frame(frame_pixels)) {
                updated = true;
            }
        }

        // If we switched from vanilla to VBG, force immediate display of first available frame
        if (state.vanilla_restored) {
            if (state.decoder->pop_frame(frame_pixels)) {
                updated = true;
            }
        }

        if (updated) {
            glBindTexture(GL_TEXTURE_2D, tex_id);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, state.width, state.height,
                            GL_RGBA, GL_UNSIGNED_BYTE, frame_pixels.data());

            state.vanilla_restored = false;

            // Update edge colors
            {
                const uint8_t* pix    = frame_pixels.data();
                const int       pitch  = state.width * 4;
                const int       W      = state.width;
                const int       H      = state.height;
                const int       STRIDE = 8;
                const int       DEPTH  = 4;

                long long sumR = 0, sumG = 0, sumB = 0, cnt = 0;

                for (int d = 0; d < DEPTH; d++)
                    for (int x = 0; x < W; x += STRIDE) {
                        const uint8_t* p = pix + d * pitch + x * 4;
                        sumR += p[0]; sumG += p[1]; sumB += p[2]; cnt++;
                    }
                for (int d = 0; d < DEPTH; d++)
                    for (int x = 0; x < W; x += STRIDE) {
                        const uint8_t* p = pix + (H - 1 - d) * pitch + x * 4;
                        sumR += p[0]; sumG += p[1]; sumB += p[2]; cnt++;
                    }
                for (int d = 0; d < DEPTH; d++)
                    for (int y = 0; y < H; y += STRIDE) {
                        const uint8_t* p = pix + y * pitch + d * 4;
                        sumR += p[0]; sumG += p[1]; sumB += p[2]; cnt++;
                    }
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
        }
    }

    void cleanup() {
        g_players.clear();
    }
}

extern "C" void reset_void_fill_color() {
    g_void_fill_color.active = false;
}
