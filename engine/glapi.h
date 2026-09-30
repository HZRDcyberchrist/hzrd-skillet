// A minimal OpenGL 4.5 core loader for the engine. It declares its own types
// and constants inside skillet::gl and loads through a host-supplied proc
// lookup, so it never collides with GLEW (the FFGL SDK) or a system gl.h.
#pragma once
#include <cstddef>
#include <cstdint>

namespace skillet {
namespace gl {

using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLfloat = float;
using GLchar = char;
using GLubyte = unsigned char;
using GLintptr = std::ptrdiff_t;
using GLsizeiptr = std::ptrdiff_t;

#if defined(_WIN32)
#define SKILLET_APIENTRY __stdcall
#else
#define SKILLET_APIENTRY
#endif

// name, return, params
#define SKILLET_GL_FUNCS(X)                                                                          \
  X(GetString, const GLubyte*, (GLenum))                                                            \
  X(GetStringi, const GLubyte*, (GLenum, GLuint))                                                   \
  X(GetIntegerv, void, (GLenum, GLint*))                                                             \
  X(GetError, GLenum, (void))                                                                       \
  X(Enable, void, (GLenum))                                                                          \
  X(Disable, void, (GLenum))                                                                         \
  X(IsEnabled, GLboolean, (GLenum))                                                                  \
  X(Viewport, void, (GLint, GLint, GLsizei, GLsizei))                                                \
  X(ClearColor, void, (GLfloat, GLfloat, GLfloat, GLfloat))                                          \
  X(Clear, void, (GLbitfield))                                                                       \
  X(PixelStorei, void, (GLenum, GLint))                                                              \
  X(ReadPixels, void, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*))                       \
  X(CreateShader, GLuint, (GLenum))                                                                  \
  X(ShaderSource, void, (GLuint, GLsizei, const GLchar* const*, const GLint*))                       \
  X(CompileShader, void, (GLuint))                                                                   \
  X(GetShaderiv, void, (GLuint, GLenum, GLint*))                                                     \
  X(GetShaderInfoLog, void, (GLuint, GLsizei, GLsizei*, GLchar*))                                    \
  X(DeleteShader, void, (GLuint))                                                                    \
  X(CreateProgram, GLuint, (void))                                                                   \
  X(AttachShader, void, (GLuint, GLuint))                                                            \
  X(LinkProgram, void, (GLuint))                                                                     \
  X(GetProgramiv, void, (GLuint, GLenum, GLint*))                                                    \
  X(GetProgramInfoLog, void, (GLuint, GLsizei, GLsizei*, GLchar*))                                   \
  X(DeleteProgram, void, (GLuint))                                                                   \
  X(UseProgram, void, (GLuint))                                                                      \
  X(GenBuffers, void, (GLsizei, GLuint*))                                                            \
  X(DeleteBuffers, void, (GLsizei, const GLuint*))                                                   \
  X(BindBuffer, void, (GLenum, GLuint))                                                              \
  X(BufferData, void, (GLenum, GLsizeiptr, const void*, GLenum))                                     \
  X(BufferSubData, void, (GLenum, GLintptr, GLsizeiptr, const void*))                                \
  X(GetBufferSubData, void, (GLenum, GLintptr, GLsizeiptr, void*))                                   \
  X(BindBufferBase, void, (GLenum, GLuint, GLuint))                                                  \
  X(CopyBufferSubData, void, (GLenum, GLenum, GLintptr, GLintptr, GLsizeiptr))                       \
  X(ClearBufferData, void, (GLenum, GLenum, GLenum, GLenum, const void*))                            \
  X(GenTextures, void, (GLsizei, GLuint*))                                                           \
  X(DeleteTextures, void, (GLsizei, const GLuint*))                                                  \
  X(BindTexture, void, (GLenum, GLuint))                                                             \
  X(ActiveTexture, void, (GLenum))                                                                   \
  X(TexStorage2D, void, (GLenum, GLsizei, GLenum, GLsizei, GLsizei))                                 \
  X(TexSubImage2D, void, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*)) \
  X(TexParameteri, void, (GLenum, GLenum, GLint))                                                    \
  X(TextureView, void, (GLuint, GLenum, GLuint, GLenum, GLuint, GLuint, GLuint, GLuint))             \
  X(BindImageTexture, void, (GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum))               \
  X(GenSamplers, void, (GLsizei, GLuint*))                                                           \
  X(DeleteSamplers, void, (GLsizei, const GLuint*))                                                  \
  X(SamplerParameteri, void, (GLuint, GLenum, GLint))                                                \
  X(BindSampler, void, (GLuint, GLuint))                                                             \
  X(DispatchCompute, void, (GLuint, GLuint, GLuint))                                                 \
  X(MemBarrier, void, (GLbitfield)) /* glMemoryBarrier: <windows.h> defines MemoryBarrier */                                                               \
  X(GenFramebuffers, void, (GLsizei, GLuint*))                                                       \
  X(DeleteFramebuffers, void, (GLsizei, const GLuint*))                                              \
  X(BindFramebuffer, void, (GLenum, GLuint))                                                         \
  X(FramebufferTexture2D, void, (GLenum, GLenum, GLenum, GLuint, GLint))                             \
  X(CheckFramebufferStatus, GLenum, (GLenum))                                                        \
  X(BlitFramebuffer, void, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
  X(GenVertexArrays, void, (GLsizei, GLuint*))                                                       \
  X(DeleteVertexArrays, void, (GLsizei, const GLuint*))                                              \
  X(BindVertexArray, void, (GLuint))                                                                 \
  X(DrawArrays, void, (GLenum, GLint, GLsizei))                                                      \
  X(ColorMaski, void, (GLuint, GLboolean, GLboolean, GLboolean, GLboolean))                        \
  X(GetUniformLocation, GLint, (GLuint, const GLchar*))                                              \
  X(Uniform1i, void, (GLint, GLint))                                                                 \
  X(Uniform1f, void, (GLint, GLfloat))                                                               \
  X(Uniform2f, void, (GLint, GLfloat, GLfloat))                                                      \
  X(Uniform4f, void, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))                                    \
  X(BlendFunc, void, (GLenum, GLenum))                                                               \
  X(GetFloatv, void, (GLenum, GLfloat*))

#define SKILLET_DECLARE(name, ret, params) \
  using PFN_##name = ret(SKILLET_APIENTRY*) params; \
  extern PFN_##name name;
SKILLET_GL_FUNCS(SKILLET_DECLARE)
#undef SKILLET_DECLARE

using GetProcFn = void* (*)(const char* name);
// Loads every function above; returns the name of the first one missing, or
// nullptr on success.
const char* load(GetProcFn getProc);

// constants
constexpr GLenum NO_ERROR_ = 0;
constexpr GLenum VENDOR = 0x1F00, RENDERER = 0x1F01, VERSION = 0x1F02, EXTENSIONS = 0x1F03, NUM_EXTENSIONS = 0x821D;
constexpr GLenum COMPUTE_SHADER = 0x91B9, VERTEX_SHADER = 0x8B31, FRAGMENT_SHADER = 0x8B30;
constexpr GLenum COMPILE_STATUS = 0x8B81, LINK_STATUS = 0x8B82, INFO_LOG_LENGTH = 0x8B84;
constexpr GLenum UNIFORM_BUFFER = 0x8A11, SHADER_STORAGE_BUFFER = 0x90D2, COPY_READ_BUFFER = 0x8F36, COPY_WRITE_BUFFER = 0x8F37;
constexpr GLenum ARRAY_BUFFER = 0x8892, PIXEL_UNPACK_BUFFER = 0x88EC, PIXEL_PACK_BUFFER = 0x88EB;
constexpr GLenum DYNAMIC_DRAW = 0x88E8, STATIC_DRAW = 0x88E4, DYNAMIC_COPY = 0x88EA;
constexpr GLenum TEXTURE_2D = 0x0DE1, TEXTURE0 = 0x84C0;
constexpr GLenum RGBA8 = 0x8058, SRGB8_ALPHA8 = 0x8C43, R32F = 0x822E, RGBA = 0x1908, RED = 0x1903, UNSIGNED_BYTE = 0x1401, FLOAT = 0x1406;
constexpr GLenum TEXTURE_MIN_FILTER = 0x2801, TEXTURE_MAG_FILTER = 0x2800, TEXTURE_WRAP_S = 0x2802, TEXTURE_WRAP_T = 0x2803;
constexpr GLint LINEAR = 0x2601, NEAREST = 0x2600, CLAMP_TO_EDGE = 0x812F, REPEAT = 0x2901, MIRRORED_REPEAT = 0x8370;
constexpr GLenum RGBA16F = 0x881A, HALF_FLOAT = 0x140B, SRC_ALPHA = 0x0302, ONE_MINUS_SRC_ALPHA = 0x0303;
constexpr GLenum READ_ONLY = 0x88B8, WRITE_ONLY = 0x88B9, READ_WRITE = 0x88BA;
constexpr GLbitfield ALL_BARRIER_BITS = 0xFFFFFFFF;
constexpr GLenum FRAMEBUFFER = 0x8D40, READ_FRAMEBUFFER = 0x8CA8, DRAW_FRAMEBUFFER = 0x8CA9, COLOR_ATTACHMENT0 = 0x8CE0;
constexpr GLenum FRAMEBUFFER_COMPLETE = 0x8CD5;
constexpr GLbitfield COLOR_BUFFER_BIT = 0x00004000;
constexpr GLenum TRIANGLES = 0x0004;
constexpr GLenum BLEND = 0x0BE2, DEPTH_TEST = 0x0B71, SCISSOR_TEST = 0x0C11, CULL_FACE = 0x0B44, FRAMEBUFFER_SRGB = 0x8DB9;
constexpr GLenum PACK_ALIGNMENT = 0x0D05, UNPACK_ALIGNMENT = 0x0CF5, UNPACK_ROW_LENGTH = 0x0CF2;
constexpr GLenum CURRENT_PROGRAM = 0x8B8D, FRAMEBUFFER_BINDING = 0x8CA6, READ_FRAMEBUFFER_BINDING = 0x8CAA, VIEWPORT = 0x0BA2;
constexpr GLenum COLOR_CLEAR_VALUE = 0x0C22;
constexpr GLenum ACTIVE_TEXTURE = 0x84E0, TEXTURE_BINDING_2D = 0x8069, VERTEX_ARRAY_BINDING = 0x85B5;

} // namespace gl
} // namespace skillet
