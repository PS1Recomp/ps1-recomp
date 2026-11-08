#pragma once

#include "runtime/gpu/gpu.h"
#include <GLES3/gl3.h> // Common desktop/mobile GL
#include <SDL2/SDL.h>
#include <string>

namespace ps1::gpu {

/// Where the PS1 picture goes inside a window of `w` x `h` pixels: the
/// largest centred 4:3 box (the TV shape, whatever the pixel width).  Any
/// scale is fine -- the sharp-bilinear shader keeps pixels even.
struct Viewport {
  int x, y, w, h;
};
inline Viewport fitViewport(int w, int h) {
  int vh = h;
  int vw = vh * 4 / 3;
  if (vw > w) {
    vw = w;
    vh = w * 3 / 4;
  }
  return {(w - vw) / 2, (h - vh) / 2, vw, vh};
}

class RendererOpenGL {
public:
  RendererOpenGL(GPU &gpu);
  ~RendererOpenGL();

  // Initialize SDL Window, GL Context, and compile shaders
  bool init(const std::string &windowTitle);

  // Upload VRAM texture and swap buffers
  void renderFrame();

  // Process SDL Events (close window, etc.)
  bool processEvents();

  // Cleanup
  void destroy();

private:
  GPU &gpu_;
  SDL_Window *window_;
  SDL_GLContext glContext_;

  GLuint vao_, vbo_, ebo_;
  GLuint shaderProgram_;
  GLuint vramTexture_;

  // Compile a shader stage
  GLuint compileShader(GLenum type, const char *source);

  // Link the program
  bool linkProgram(GLuint vShader, GLuint fShader);

  void setupGeometry();
  void setupTexture();
};

} // namespace ps1::gpu
