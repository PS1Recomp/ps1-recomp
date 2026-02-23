#pragma once

#include "runtime/gpu/gpu.h"
#include <GLES3/gl3.h> // Common desktop/mobile GL
#include <SDL2/SDL.h>
#include <string>

namespace ps1::gpu {

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
