//test_gl.c: checks the OpenGL 4.5 context of runtime/glctx.c on a hidden window.
//
//Usage: test_gl.exe
//build from the project folder:
//  clang -Iruntime tests/test_gl.c runtime/glctx.c runtime/glload.c -lopengl32 -lgdi32 -luser32 \
//    -o tests/test_gl.exe
//what it does: opens a window that is never shown, makes the context, runs the self test (shader,
//framebuffer, integer texture, fence, barrier) and closes everything. It takes well under a
//second and shows nothing on the screen. It only needs a graphics driver with OpenGL 4.5.
#include "glctx.h"
#include <stdio.h>

int main(void)
{
  char err[300] = "";
  GlCtx* c = glctx_create(err, sizeof err);
  if (!c)
  {
    printf("no context: %s\n", err);
    return 1;
  }
  int major, minor;
  glctx_version(c, &major, &minor);
  printf("context OpenGL %d.%d\n", major, minor);
  int ok = glctx_selftest(err, sizeof err);
  printf(ok ? "self test passed\n" : "self test FAILED: %s\n", err);
  for (int i = 0; ok && i < 3; i++) //a second and third run catch leftovers of the first
    ok = glctx_selftest(err, sizeof err);
  if (!ok)
    printf("repeated self test FAILED: %s\n", err);
  glctx_destroy(c);
  return ok ? 0 : 1;
}
