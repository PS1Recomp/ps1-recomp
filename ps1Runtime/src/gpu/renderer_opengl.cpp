#include "runtime/gpu/renderer_opengl.h"
#include <fmt/format.h>
#include <iostream>

// Use desktop GL via SDL GLAD or just default GL provided by system
#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES 1
#endif
#include <SDL2/SDL_opengl.h>

namespace ps1::gpu {

static const char *VERTEX_SHADER_SRC = R"(
#version 330 core
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec2 aTexCoord;

out vec2 TexCoord;

void main() {
    gl_Position = vec4(aPos.x, aPos.y, 0.0, 1.0);
    TexCoord = aTexCoord;
}
)";

static const char *FRAGMENT_SHADER_SRC = R"(
#version 330 core
out vec4 FragColor;
in vec2 TexCoord;

uniform sampler2D vramTex;
// Uniform storing (X / 1024.0, Y / 512.0, W / 1024.0, H / 512.0)
uniform vec4 uDisplayArea;

void main() {
    // Map normalized TexCoord (0..1) to the actual Display Area in VRAM
    vec2 displayCoord = vec2(
        uDisplayArea.x + TexCoord.x * uDisplayArea.z,
        uDisplayArea.y + TexCoord.y * uDisplayArea.w
    );
    
    FragColor = texture(vramTex, displayCoord);
}
)";

RendererOpenGL::RendererOpenGL(GPU &gpu)
    : gpu_(gpu), window_(nullptr), glContext_(nullptr), vao_(0), vbo_(0),
      ebo_(0), shaderProgram_(0), vramTexture_(0) {}

RendererOpenGL::~RendererOpenGL() { destroy(); }

bool RendererOpenGL::init(const std::string &windowTitle) {
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
    fmt::print(stderr, "[RendererGL] SDL_Init Error: {}\n", SDL_GetError());
    return false;
  }

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

  window_ = SDL_CreateWindow(
      windowTitle.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 800,
      600, // Initial window size (scaled up from native)
      SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);

  if (!window_) {
    fmt::print(stderr, "[RendererGL] Window Error: {}\n", SDL_GetError());
    return false;
  }

  glContext_ = SDL_GL_CreateContext(window_);
  if (!glContext_) {
    fmt::print(stderr, "[RendererGL] GL Context Error: {}\n", SDL_GetError());
    return false;
  }

  SDL_GL_SetSwapInterval(1); // VSync on

  GLuint vs = compileShader(GL_VERTEX_SHADER, VERTEX_SHADER_SRC);
  GLuint fs = compileShader(GL_FRAGMENT_SHADER, FRAGMENT_SHADER_SRC);
  if (!linkProgram(vs, fs)) {
    return false;
  }
  glDeleteShader(vs);
  glDeleteShader(fs);

  setupGeometry();
  setupTexture();

  return true;
}

GLuint RendererOpenGL::compileShader(GLenum type, const char *source) {
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);

  GLint success;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
  if (!success) {
    char infoLog[512];
    glGetShaderInfoLog(shader, 512, nullptr, infoLog);
    fmt::print(stderr, "[RendererGL] Shader Compile Error:\n{}\n", infoLog);
  }
  return shader;
}

bool RendererOpenGL::linkProgram(GLuint vShader, GLuint fShader) {
  shaderProgram_ = glCreateProgram();
  glAttachShader(shaderProgram_, vShader);
  glAttachShader(shaderProgram_, fShader);
  glLinkProgram(shaderProgram_);

  GLint success;
  glGetProgramiv(shaderProgram_, GL_LINK_STATUS, &success);
  if (!success) {
    char infoLog[512];
    glGetProgramInfoLog(shaderProgram_, 512, nullptr, infoLog);
    fmt::print(stderr, "[RendererGL] Program Link Error:\n{}\n", infoLog);
    return false;
  }
  return true;
}

void RendererOpenGL::setupGeometry() {
  // Vertices for a full screen quad
  float vertices[] = {
      // positions  // texcoords
      // Mapping the 320x240 display area of VRAM to the quad
      // For now, map entire 1024x512 for debugging
      1.0f,  1.0f,  1.0f, 0.0f, // top right
      1.0f,  -1.0f, 1.0f, 1.0f, // bottom right
      -1.0f, -1.0f, 0.0f, 1.0f, // bottom left
      -1.0f, 1.0f,  0.0f, 0.0f  // top left
  };
  unsigned int indices[] = {
      0, 1, 3, // first triangle
      1, 2, 3  // second triangle
  };

  glGenVertexArrays(1, &vao_);
  glGenBuffers(1, &vbo_);
  glGenBuffers(1, &ebo_);

  glBindVertexArray(vao_);

  glBindBuffer(GL_ARRAY_BUFFER, vbo_);
  glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices,
               GL_STATIC_DRAW);

  // Position
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
  glEnableVertexAttribArray(0);
  // TexCoord
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                        (void *)(2 * sizeof(float)));
  glEnableVertexAttribArray(1);
}

void RendererOpenGL::setupTexture() {
  glGenTextures(1, &vramTexture_);
  glBindTexture(GL_TEXTURE_2D, vramTexture_);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                  GL_NEAREST); // PS1 is pixelated
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

  // Allocate empty texture 1024x512
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB5_A1, GPU::VRAM_WIDTH, GPU::VRAM_HEIGHT,
               0, GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, nullptr);
}

void RendererOpenGL::renderFrame() {
  if (!window_)
    return;

  int w, h;
  SDL_GetWindowSize(window_, &w, &h);
  glViewport(0, 0, w, h);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  // Upload display snapshot (captured at VBlank) to texture.
  // Always upload regardless of the display-enable flag: our HLE BIOS does not
  // always re-send GP1(0x03) after a GPU reset, so isDisplayEnabled() can stay
  // false even though VRAM has valid content.
  glBindTexture(GL_TEXTURE_2D, vramTexture_);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GPU::VRAM_WIDTH, GPU::VRAM_HEIGHT,
                  GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV,
                  gpu_.getDisplayVRAM());

  glUseProgram(shaderProgram_);

  // Setup display area uniforms based on GP1
  uint32_t vramX, vramY;
  gpu_.getDisplayArea(vramX, vramY);

  // Never auto-detect vramX: the default (0,0) from reset is correct when the
  // game doesn't send GP1(0x05). Games that store textures at x=320+ would
  // fool a pixel-count heuristic into showing the texture page instead of the
  // actual framebuffer.

  // Derive width/height from GPUSTAT only when the game explicitly set
  // GP1(0x08). Otherwise keep the 320x240 default -- many games render 320px
  // wide without ever sending GP1(0x08) (the GPUSTAT reset value of hres=0
  // means "256px" but those bits are meaningless until the game configures them).
  uint32_t stat = gpu_.readGPUSTAT();
  int currentW = 320;
  int currentH = 240;
  if (gpu_.isDisplayModeSet()) {
    currentH = (stat & (1u << 20)) ? 480 : 240;
    int hres = (stat >> 17) & 3;
    if (stat & (1u << 16))
      currentW = 368;
    else if (hres == 0)
      currentW = 256;
    else if (hres == 1)
      currentW = 320;
    else if (hres == 2)
      currentW = 512;
    else
      currentW = 640;
  }

  float uX = vramX / 1024.0f;
  float uY = vramY / 512.0f;
  float uW = currentW / 1024.0f;
  float uH = currentH / 512.0f;

  GLint loc = glGetUniformLocation(shaderProgram_, "uDisplayArea");
  glUniform4f(loc, uX, uY, uW, uH);

  glBindVertexArray(vao_);
  glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);

  SDL_GL_SwapWindow(window_);
}

bool RendererOpenGL::processEvents() {
  // Main loop in main_host.cpp already handles SDL_PollEvent.
  // We don't want to steal SDL_QUIT or other events here.
  return true;
}

void RendererOpenGL::destroy() {
  if (vao_) {
    glDeleteVertexArrays(1, &vao_);
    vao_ = 0;
  }
  if (vbo_) {
    glDeleteBuffers(1, &vbo_);
    vbo_ = 0;
  }
  if (ebo_) {
    glDeleteBuffers(1, &ebo_);
    ebo_ = 0;
  }
  if (shaderProgram_) {
    glDeleteProgram(shaderProgram_);
    shaderProgram_ = 0;
  }
  if (vramTexture_) {
    glDeleteTextures(1, &vramTexture_);
    vramTexture_ = 0;
  }

  if (glContext_) {
    SDL_GL_DeleteContext(glContext_);
    glContext_ = nullptr;
  }
  if (window_) {
    SDL_DestroyWindow(window_);
    window_ = nullptr;
  }

  SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

} // namespace ps1::gpu
