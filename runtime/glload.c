//OpenGL function loader. See glload.h.
#include "glload.h"
#include <stdio.h>
#include <string.h>

#define X(ret, name, args) ret(APIENTRY* name) args;
GL_FUNCS(X)
#undef X

//wglGetProcAddress can answer 0, 1, 2, 3 or -1 for a function it does not have
static void* lookup(const char* name)
{
  void* p = (void*)wglGetProcAddress(name);
  if (!p || p == (void*)1 || p == (void*)2 || p == (void*)3 || p == (void*)-1)
    return NULL;
  return p;
}

int gl_load(char* missing, size_t missing_size)
{
  if (missing && missing_size)
    missing[0] = 0;
#define X(ret, name, args)                                                                         \
  name = (ret(APIENTRY*) args)lookup(#name);                                                       \
  if (!name)                                                                                       \
  {                                                                                                \
    if (missing && missing_size)                                                                   \
      snprintf(missing, missing_size, "%s", #name);                                                \
    return 0;                                                                                      \
  }
  GL_FUNCS(X)
#undef X
  return 1;
}
