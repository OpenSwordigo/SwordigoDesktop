#pragma once
#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  /* opengl32.lib only exports OpenGL 1.1. Route every modern GL call through
     GLEW (shared) so FBO/VBO/shader functions resolve at runtime. */
  #include <GL/glew.h>
#else
  #include <GL/gl.h>
#endif

/* Call immediately after an SDL_GL_CreateContext / MakeCurrent. On Windows it
   resolves the modern GL entry points through GLEW; elsewhere it is a no-op. */
#if defined(__cplusplus)
static inline void sword_init_gl_after(void) {
#ifdef _WIN32
    glewExperimental = (unsigned char)GL_TRUE;
    glewInit();
#else
    (void)0;
#endif
}
#endif
