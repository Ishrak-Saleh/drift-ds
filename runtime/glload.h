// OpenGL function loader for the GPU 3D drawing (OpenGL 4.5 core).
//
// Where it fits: Windows only gives OpenGL 1.1 directly (opengl32.dll). Everything newer is looked
// up by name at run time, after a context exists. This file declares those functions and fills
// them in (gl_load). glctx.c calls it once the 4.5 context is current.
// Why: the GPU drawing needs shaders, framebuffers and fences, which are all newer than 1.1.
//
// The list is one table (GL_FUNCS): the declaration, the definition and the lookup are made from
// it, so a function is added in one place.
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <GL/gl.h>

typedef ptrdiff_t GLsizeiptr_;
typedef ptrdiff_t GLintptr_;
typedef char GLchar_;
typedef struct __GLsync_* GLsync_;

// constants that are not in the OpenGL 1.1 header
#define GLX_VERTEX_SHADER 0x8B31
#define GLX_FRAGMENT_SHADER 0x8B30
#define GLX_COMPILE_STATUS 0x8B81
#define GLX_LINK_STATUS 0x8B82
#define GLX_INFO_LOG_LENGTH 0x8B84
#define GLX_ARRAY_BUFFER 0x8892
#define GLX_UNIFORM_BUFFER 0x8A11
#define GLX_STATIC_DRAW 0x88E4
#define GLX_DYNAMIC_DRAW 0x88E8
#define GLX_STREAM_DRAW 0x88E0
#define GLX_FRAMEBUFFER 0x8D40
#define GLX_READ_FRAMEBUFFER 0x8CA8
#define GLX_DRAW_FRAMEBUFFER 0x8CA9
#define GLX_COLOR_ATTACHMENT0 0x8CE0
#define GLX_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GLX_FRAMEBUFFER_COMPLETE 0x8CD5
#define GLX_RGBA8UI 0x8D7C
#define GLX_RGBA16UI 0x8D76
#define GLX_R32UI 0x8236
#define GLX_R8UI 0x8232
#define GLX_RG32UI 0x823C
#define GLX_RGBA_INTEGER 0x8D99
#define GLX_RED_INTEGER 0x8D94
#define GLX_UNSIGNED_INT_8_8_8_8_REV 0x8367
#define GLX_TEXTURE0 0x84C0
#define GLX_MAJOR_VERSION 0x821B
#define GLX_MINOR_VERSION 0x821C
#define GLX_ALL_BARRIER_BITS 0xFFFFFFFFu
#define GLX_SHADER_IMAGE_ACCESS_BARRIER_BIT 0x00000020
#define GLX_FRAMEBUFFER_BARRIER_BIT 0x00000400
#define GLX_TEXTURE_FETCH_BARRIER_BIT 0x00000008
#define GLX_READ_ONLY 0x88B8
#define GLX_WRITE_ONLY 0x88B9
#define GLX_READ_WRITE 0x88BA
#define GLX_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define GLX_SYNC_FLUSH_COMMANDS_BIT 0x00000001
#define GLX_ALREADY_SIGNALED 0x911A
#define GLX_CONDITION_SATISFIED 0x911C
#define GLX_PIXEL_PACK_BUFFER 0x88EB
#define GLX_MAP_READ_BIT 0x0001
#define GLX_STREAM_READ 0x88E1
#define GLX_TEXTURE_2D_ARRAY 0x8C1A

#define GL_FUNCS(X)                                                                                \
  X(GLuint, glCreateShader, (GLenum type))                                                         \
  X(void, glShaderSource, (GLuint s, GLsizei n, const GLchar_* const* src, const GLint* len))      \
  X(void, glCompileShader, (GLuint s))                                                             \
  X(void, glGetShaderiv, (GLuint s, GLenum pname, GLint * out))                                    \
  X(void, glGetShaderInfoLog, (GLuint s, GLsizei max, GLsizei * len, GLchar_ * log))               \
  X(void, glDeleteShader, (GLuint s))                                                              \
  X(GLuint, glCreateProgram, (void))                                                               \
  X(void, glAttachShader, (GLuint p, GLuint s))                                                    \
  X(void, glLinkProgram, (GLuint p))                                                               \
  X(void, glGetProgramiv, (GLuint p, GLenum pname, GLint * out))                                   \
  X(void, glGetProgramInfoLog, (GLuint p, GLsizei max, GLsizei * len, GLchar_ * log))              \
  X(void, glUseProgram, (GLuint p))                                                                \
  X(void, glDeleteProgram, (GLuint p))                                                             \
  X(GLint, glGetUniformLocation, (GLuint p, const GLchar_* name))                                  \
  X(void, glUniform1i, (GLint loc, GLint v))                                                       \
  X(void, glUniform1ui, (GLint loc, GLuint v))                                                     \
  X(void, glUniform1f, (GLint loc, GLfloat v))                                                     \
  X(void, glUniform1uiv, (GLint loc, GLsizei n, const GLuint* v))                                  \
  X(void, glUniform2f, (GLint loc, GLfloat a, GLfloat b))                                          \
  X(void, glUniform2i, (GLint loc, GLint a, GLint b))                                              \
  X(void, glUniform4i, (GLint loc, GLint a, GLint b, GLint c, GLint d))                            \
  X(void, glUniform4f, (GLint loc, GLfloat a, GLfloat b, GLfloat c, GLfloat d))                    \
  X(void, glGenBuffers, (GLsizei n, GLuint * ids))                                                 \
  X(void, glBindBuffer, (GLenum target, GLuint id))                                                \
  X(void, glBindBufferBase, (GLenum target, GLuint index, GLuint id))                              \
  X(void, glBufferData, (GLenum target, GLsizeiptr_ size, const void* data, GLenum usage))         \
  X(void, glBufferSubData, (GLenum target, GLintptr_ off, GLsizeiptr_ size, const void* data))     \
  X(void, glDeleteBuffers, (GLsizei n, const GLuint* ids))                                         \
  X(void, glGenVertexArrays, (GLsizei n, GLuint * ids))                                            \
  X(void, glBindVertexArray, (GLuint id))                                                          \
  X(void, glDeleteVertexArrays, (GLsizei n, const GLuint* ids))                                    \
  X(void, glEnableVertexAttribArray, (GLuint i))                                                   \
  X(void, glVertexAttribPointer,                                                                   \
    (GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void* p))            \
  X(void, glVertexAttribIPointer,                                                                  \
    (GLuint i, GLint size, GLenum type, GLsizei stride, const void* p))                            \
  X(void, glGenFramebuffers, (GLsizei n, GLuint * ids))                                            \
  X(void, glBindFramebuffer, (GLenum target, GLuint id))                                           \
  X(void, glDeleteFramebuffers, (GLsizei n, const GLuint* ids))                                    \
  X(void, glFramebufferTexture2D,                                                                  \
    (GLenum target, GLenum att, GLenum textarget, GLuint tex, GLint level))                        \
  X(GLenum, glCheckFramebufferStatus, (GLenum target))                                             \
  X(void, glDrawBuffers, (GLsizei n, const GLenum* bufs))                                          \
  X(void, glTexStorage2D, (GLenum target, GLsizei levels, GLenum fmt, GLsizei w, GLsizei h))       \
  X(void, glTexStorage3D,                                                                          \
    (GLenum target, GLsizei levels, GLenum fmt, GLsizei w, GLsizei h, GLsizei d))                  \
  X(void, glTexSubImage3D,                                                                         \
    (GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d,       \
     GLenum fmt, GLenum type, const void* px))                                                     \
  X(void, glActiveTexture, (GLenum unit))                                                          \
  X(void, glBindImageTexture,                                                                      \
    (GLuint unit, GLuint tex, GLint level, GLboolean layered, GLint layer, GLenum access,          \
     GLenum fmt))                                                                                  \
  X(void, glMemoryBarrier, (GLbitfield barriers))                                                  \
  X(void, glTextureBarrier, (void))                                                                \
  X(void, glMultiDrawArrays, (GLenum mode, const GLint* first, const GLsizei* count, GLsizei n))   \
  X(void, glClearBufferuiv, (GLenum buffer, GLint drawbuffer, const GLuint* value))                \
  X(void, glClearBufferfv, (GLenum buffer, GLint drawbuffer, const GLfloat* value))                \
  X(GLsync_, glFenceSync, (GLenum cond, GLbitfield flags))                                         \
  X(GLenum, glClientWaitSync, (GLsync_ s, GLbitfield flags, uint64_t timeout))                     \
  X(void, glDeleteSync, (GLsync_ s))                                                               \
  X(void*, glMapBufferRange, (GLenum target, GLintptr_ off, GLsizeiptr_ len, GLbitfield access))   \
  X(GLboolean, glUnmapBuffer, (GLenum target))

#define X(ret, name, args) extern ret(APIENTRY* name) args;
GL_FUNCS(X)
#undef X

// Looks every function of GL_FUNCS up. Needs a current OpenGL context. Returns 1 when all were
// found. Otherwise returns 0 and, when missing is not NULL, copies the name of the first missing
// function into it (at most missing_size bytes).
int gl_load(char* missing, size_t missing_size);
