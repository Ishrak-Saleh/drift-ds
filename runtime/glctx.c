//OpenGL 4.5 context on a hidden window. See glctx.h.
#include "glctx.h"
#include "glload.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct GlCtx
{
  HWND hwnd;
  HDC dc;
  HGLRC rc;
  int major, minor;
};

#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001

typedef HGLRC(WINAPI* CreateContextAttribs)(HDC, HGLRC, const int*);

static void fail(char* err, size_t n, const char* msg)
{
  if (err && n)
    snprintf(err, n, "%s", msg);
}

static void release(GlCtx* c)
{
  if (!c)
    return;
  wglMakeCurrent(NULL, NULL);
  if (c->rc)
    wglDeleteContext(c->rc);
  if (c->dc && c->hwnd)
    ReleaseDC(c->hwnd, c->dc);
  if (c->hwnd)
    DestroyWindow(c->hwnd);
  free(c);
}

GlCtx* glctx_create(char* err, size_t err_size)
{
  GlCtx* c = calloc(1, sizeof *c);
  if (!c)
  {
    fail(err, err_size, "out of memory");
    return NULL;
  }
  static const char* cls = "DriftDSGl";
  static int registered;
  HINSTANCE inst = GetModuleHandleA(NULL);
  if (!registered)
  {
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = inst;
    wc.lpszClassName = cls;
    if (!RegisterClassA(&wc))
    {
      fail(err, err_size, "could not register the hidden OpenGL window");
      free(c);
      return NULL;
    }
    registered = 1;
  }
  //a window that is never shown: only its device context is needed
  c->hwnd = CreateWindowExA(0, cls, "", WS_POPUP, 0, 0, 16, 16, NULL, NULL, inst, NULL);
  c->dc = c->hwnd ? GetDC(c->hwnd) : NULL;
  if (!c->dc)
  {
    fail(err, err_size, "could not make the hidden OpenGL window");
    release(c);
    return NULL;
  }
  PIXELFORMATDESCRIPTOR pfd;
  memset(&pfd, 0, sizeof pfd);
  pfd.nSize = sizeof pfd;
  pfd.nVersion = 1;
  pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL;
  pfd.iPixelType = PFD_TYPE_RGBA;
  pfd.cColorBits = 32;
  int fmt = ChoosePixelFormat(c->dc, &pfd);
  if (!fmt || !SetPixelFormat(c->dc, fmt, &pfd))
  {
    fail(err, err_size, "the graphics driver gave no usable pixel format");
    release(c);
    return NULL;
  }
  //an old style context first: only a current context can tell where the new function is
  HGLRC temp = wglCreateContext(c->dc);
  if (!temp || !wglMakeCurrent(c->dc, temp))
  {
    if (temp)
      wglDeleteContext(temp);
    fail(err, err_size, "the graphics driver could not start OpenGL");
    release(c);
    return NULL;
  }
  CreateContextAttribs create =
      (CreateContextAttribs)(void*)wglGetProcAddress("wglCreateContextAttribsARB");
  if (!create)
  {
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(temp);
    fail(err, err_size, "the graphics driver has no OpenGL 4.5 (modern contexts are missing)");
    release(c);
    return NULL;
  }
  const int attribs[] = {WGL_CONTEXT_MAJOR_VERSION_ARB,
                         4,
                         WGL_CONTEXT_MINOR_VERSION_ARB,
                         5,
                         WGL_CONTEXT_PROFILE_MASK_ARB,
                         WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
                         0};
  c->rc = create(c->dc, NULL, attribs);
  wglMakeCurrent(NULL, NULL);
  wglDeleteContext(temp);
  if (!c->rc || !wglMakeCurrent(c->dc, c->rc))
  {
    fail(err, err_size, "the graphics driver has no OpenGL 4.5, which the 3D drawing needs");
    release(c);
    return NULL;
  }
  char missing[64];
  if (!gl_load(missing, sizeof missing))
  {
    char msg[160];
    snprintf(msg, sizeof msg, "the graphics driver lacks an OpenGL function: %s", missing);
    fail(err, err_size, msg);
    release(c);
    return NULL;
  }
  glGetIntegerv(GLX_MAJOR_VERSION, &c->major);
  glGetIntegerv(GLX_MINOR_VERSION, &c->minor);
  if (c->major < 4 || (c->major == 4 && c->minor < 5))
  {
    char msg[96];
    snprintf(msg, sizeof msg, "OpenGL %d.%d is too old, the 3D drawing needs 4.5", c->major,
             c->minor);
    fail(err, err_size, msg);
    release(c);
    return NULL;
  }
  return c;
}

void glctx_destroy(GlCtx* c)
{
  release(c);
}

void glctx_version(const GlCtx* c, int* major, int* minor)
{
  *major = c->major;
  *minor = c->minor;
}

//---- self test ----

static GLuint compile(GLenum type, const char* src, char* err, size_t err_size)
{
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GLX_COMPILE_STATUS, &ok);
  if (!ok)
  {
    char log[400];
    glGetShaderInfoLog(s, sizeof log, NULL, log);
    snprintf(err, err_size, "shader did not compile: %s", log);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

//a full screen triangle without any vertex data. The fragment shader writes four numbers that
//depend on the pixel, and reads a second texture with texelFetch, so both an integer target and
//an integer source are tried.
static const char* VS = "#version 450 core\n"
                        "void main(){\n"
                        "  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
                        "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
                        "}\n";
static const char* FS = "#version 450 core\n"
                        "layout(binding = 0) uniform usampler2D src;\n"
                        "layout(location = 0) out uvec4 outc;\n"
                        "void main(){\n"
                        "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
                        "  uvec4 s = texelFetch(src, ivec2(p.x & 1, 0), 0);\n"
                        "  outc = uvec4(uint(p.x), uint(p.y), s.r, 200u);\n"
                        "}\n";

int glctx_selftest(char* err, size_t err_size)
{
  const int W = 8, H = 4;
  int ok = 0;
  GLuint vs = 0, fs = 0, prog = 0, vao = 0, fbo = 0, tex[2] = {0, 0};
  GLsync_ fence = NULL;
  while (glGetError() != GL_NO_ERROR)
    ;
  vs = compile(GLX_VERTEX_SHADER, VS, err, err_size);
  fs = vs ? compile(GLX_FRAGMENT_SHADER, FS, err, err_size) : 0;
  if (!vs || !fs)
    goto done;
  prog = glCreateProgram();
  glAttachShader(prog, vs);
  glAttachShader(prog, fs);
  glLinkProgram(prog);
  GLint linked = 0;
  glGetProgramiv(prog, GLX_LINK_STATUS, &linked);
  if (!linked)
  {
    char log[400];
    glGetProgramInfoLog(prog, sizeof log, NULL, log);
    snprintf(err, err_size, "shader program did not link: %s", log);
    goto done;
  }
  glGenTextures(2, tex);
  //source: two texels, red = 11 and 22
  glBindTexture(GL_TEXTURE_2D, tex[0]);
  glTexStorage2D(GL_TEXTURE_2D, 1, GLX_RGBA8UI, 2, 1);
  const uint8_t srcpx[8] = {11, 0, 0, 0, 22, 0, 0, 0};
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 2, 1, GLX_RGBA_INTEGER, GL_UNSIGNED_BYTE, srcpx);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  //target
  glBindTexture(GL_TEXTURE_2D, tex[1]);
  glTexStorage2D(GL_TEXTURE_2D, 1, GLX_RGBA8UI, W, H);
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GLX_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GLX_FRAMEBUFFER, GLX_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[1], 0);
  if (glCheckFramebufferStatus(GLX_FRAMEBUFFER) != GLX_FRAMEBUFFER_COMPLETE)
  {
    snprintf(err, err_size, "the framebuffer is not complete");
    goto done;
  }
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  glActiveTexture(GLX_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, tex[0]);
  glViewport(0, 0, W, H);
  glUseProgram(prog);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glTextureBarrier();
  glMemoryBarrier(GLX_FRAMEBUFFER_BARRIER_BIT);
  fence = glFenceSync(GLX_SYNC_GPU_COMMANDS_COMPLETE, 0);
  GLenum w = glClientWaitSync(fence, GLX_SYNC_FLUSH_COMMANDS_BIT, 2000000000ull);
  if (w != GLX_ALREADY_SIGNALED && w != GLX_CONDITION_SATISFIED)
  {
    snprintf(err, err_size, "the graphics card did not finish the test drawing");
    goto done;
  }
  uint8_t px[8 * 4 * 4];
  memset(px, 0xEE, sizeof px);
  glReadPixels(0, 0, W, H, GLX_RGBA_INTEGER, GL_UNSIGNED_BYTE, px);
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++)
    {
      const uint8_t* p = px + (y * W + x) * 4;
      uint8_t want_r = (uint8_t)(x & 1 ? 22 : 11);
      if (p[0] != x || p[1] != y || p[2] != want_r || p[3] != 200)
      {
        snprintf(err, err_size, "the test drawing gave wrong pixels (at %d,%d got %d,%d,%d,%d)", x,
                 y, p[0], p[1], p[2], p[3]);
        goto done;
      }
    }
  GLenum e = glGetError();
  if (e != GL_NO_ERROR)
  {
    snprintf(err, err_size, "OpenGL reported error 0x%X during the test", e);
    goto done;
  }
  ok = 1;
done:
  if (fence)
    glDeleteSync(fence);
  glBindFramebuffer(GLX_FRAMEBUFFER, 0);
  glUseProgram(0);
  glBindVertexArray(0);
  if (vao)
    glDeleteVertexArrays(1, &vao);
  if (fbo)
    glDeleteFramebuffers(1, &fbo);
  glDeleteTextures(2, tex);
  if (prog)
    glDeleteProgram(prog);
  if (vs)
    glDeleteShader(vs);
  if (fs)
    glDeleteShader(fs);
  return ok;
}
