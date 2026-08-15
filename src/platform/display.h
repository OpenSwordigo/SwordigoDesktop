#ifndef DISPLAY_H
#define DISPLAY_H

#include <SDL3/SDL.h>
#include <stdint.h>
#include <string>

class Display {
public:
    Display();
    ~Display();
    
    bool init(int width, int height, const std::string& title);
#ifdef VULKAN_BACKEND
    bool init_vulkan(int width, int height, const std::string& title);
#endif
    void swap();
    void poll_events();
    bool should_close() const { return quit; }
    
    int get_width() const { return width; }
    int get_height() const { return height; }
    SDL_Window* get_window() const { return window; }
    SDL_GLContext get_gl_context() const { return gl_context; }

private:
    SDL_Window* window = nullptr;
    SDL_GLContext gl_context = nullptr;
    int width = 800;
    int height = 480;
    bool quit = false;
#ifdef VULKAN_BACKEND
    bool use_vulkan = false;
#endif
};

#endif
