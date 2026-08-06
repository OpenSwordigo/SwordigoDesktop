// VULKAN_BACKEND compile guard now required — see init_vulkan below
#include "platform/display.h"
#include "platform/data_path.h"
#include "platform/embedded_assets.h"
#include <iostream>
#include "platform/gl_inc.h"
#include <SDL3_image/SDL_image.h>

// Load the launcher icon from the binary's embedded assets (permanent fix —
// works even when deb/rpm packaging drops the launcher/ asset directory).
static SDL_Surface* load_embedded_icon() {
    const unsigned char* data = nullptr;
    size_t size = 0;
    if (!embedded_asset("icon_gnome.png", &data, &size) &&
        !embedded_asset("launcer_icon.png", &data, &size))
        return nullptr;
    int w = 0, h = 0;
    unsigned char* px = nullptr;
    if (!asset_decode_image(data, size, &px, &w, &h)) return nullptr;
    SDL_Surface* surf = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_ABGR8888, px, w * 4);
    if (!surf) { asset_image_free(px); return nullptr; }
    SDL_Surface* copy = SDL_DuplicateSurface(surf);
    SDL_DestroySurface(surf);
    asset_image_free(px);
    return copy;
}

Display::Display() {}

Display::~Display() {
#ifdef VULKAN_BACKEND
    // When using Vulkan, there is no GL context to delete.
    // VulkanBackend::destroy() handles Vulkan cleanup separately.
    if (!use_vulkan && gl_context) SDL_GL_DestroyContext(gl_context);
#else
    if (gl_context) SDL_GL_DestroyContext(gl_context);
#endif
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
}

bool Display::init(int w, int h, const std::string& title) {
    width = w;
    height = h;
    
    // Set app ID so Wayland/GNOME matches this window to Swordigo.desktop
    // (SDL_SetWindowIcon doesn't work on Wayland — icon comes from .desktop file)
    SDL_SetHint(SDL_HINT_APP_ID, "Swordigo");
    
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "[Display] SDL_Init failed: " << SDL_GetError() << std::endl;
        return false;
    }
    
    // Request OpenGL Compatibility Profile for GLES 1.x fixed-function support
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    
    window = SDL_CreateWindow(
        title.c_str(),
        width, height,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY
    );
    
    if (!window) {
        std::cerr << "[Display] Window creation failed: " << SDL_GetError() << std::endl;
        return false;
    }
    
    gl_context = SDL_GL_CreateContext(window);
    if (!gl_context) {
        std::cerr << "[Display] GL context creation failed: " << SDL_GetError() << std::endl;
        return false;
    }
    
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1); // VSync
    
    // Log both logical and physical pixel dimensions for HiDPI awareness
    int phys_w, phys_h;
    SDL_GetWindowSizeInPixels(window, &phys_w, &phys_h);
    std::cout << "[Display] Window created: " << width << "x" << height
              << " (drawable: " << phys_w << "x" << phys_h << ")" << std::endl;
    std::cout << "[Display] GL Vendor:   " << glGetString(GL_VENDOR) << std::endl;
    std::cout << "[Display] GL Renderer: " << glGetString(GL_RENDERER) << std::endl;
    std::cout << "[Display] GL Version:  " << glGetString(GL_VERSION) << std::endl;
    // Set window icon — embedded first (permanent), then disk fallbacks
    {
        SDL_Surface* icon = load_embedded_icon();
        if (icon) std::cout << "[Display] Window icon set (embedded)" << std::endl;
        else {
            // Try via get_data_path (handles AppImage, RPM, DEB, dev)
            std::string icon_via_data = get_data_path("src/assets/icon_gnome.png");
            const char* icon_paths[] = {
                icon_via_data.c_str(),
                "src/assets/icon_gnome.png",
                "/usr/share/icons/hicolor/128x128/apps/swordigo-desktop.png",
                "/usr/share/pixmaps/swordigo-desktop.png",
                nullptr
            };
            for (int i = 0; icon_paths[i]; i++) {
                icon = IMG_Load(icon_paths[i]);
                if (icon) { std::cout << "[Display] Window icon set (disk)" << std::endl; break; }
            }
        }
        if (icon) {
            SDL_SetWindowIcon(window, icon);
            SDL_DestroySurface(icon);
        } else {
            std::cerr << "[Display] Could not load icon: " << SDL_GetError() << std::endl;
        }
    }

    return true;
}

void Display::swap() {
#ifdef VULKAN_BACKEND
    // VulkanBackend::end_frame_and_present() handles Vulkan presentation
    if (use_vulkan) return;
#endif
    SDL_GL_SwapWindow(window);
}

#ifdef VULKAN_BACKEND
bool Display::init_vulkan(int w, int h, const std::string& title) {
    width = w;
    height = h;
    use_vulkan = true;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "[Display] SDL_Init failed: " << SDL_GetError() << std::endl;
        return false;
    }

    window = SDL_CreateWindow(
        title.c_str(),
        width, height,
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE
    );

    if (!window) {
        std::cerr << "[Display] Vulkan window creation failed: " << SDL_GetError() << std::endl;
        return false;
    }

    // No GL context needed — VulkanBackend::init() creates the Vulkan device,
    // swapchain, etc. using this SDL window.

    std::cout << "[Display] Vulkan window created: " << width << "x" << height << std::endl;

    // Set window icon — embedded first (permanent), then disk fallbacks
    {
        SDL_Surface* icon = load_embedded_icon();
        if (icon) std::cout << "[Display] Vulkan window icon set (embedded)" << std::endl;
        else {
            std::string icon_via_data = get_data_path("src/assets/icon_gnome.png");
            const char* icon_paths[] = {
                icon_via_data.c_str(),
                "src/assets/icon_gnome.png",
                "/usr/share/icons/hicolor/128x128/apps/swordigo-desktop.png",
                "/usr/share/pixmaps/swordigo-desktop.png",
                nullptr
            };
            for (int i = 0; icon_paths[i]; i++) {
                icon = IMG_Load(icon_paths[i]);
                if (icon) break;
            }
        }
        if (icon) {
            SDL_SetWindowIcon(window, icon);
            SDL_DestroySurface(icon);
        } else {
            std::cerr << "[Display] Could not load icon: " << SDL_GetError() << std::endl;
        }
    }

    return true;
}
#endif

void Display::poll_events() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_QUIT:
                quit = true;
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                quit = true;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (event.key.key == SDLK_ESCAPE) {
                    quit = true;
                }
                break;
        }
    }
}
