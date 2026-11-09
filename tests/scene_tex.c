// scene_tex.c: runs the texture cache over recorded scenes, in file order, like the game would.
//
// Usage: scene_tex scenes/scene-A-001.dscn scenes/scene-A-002.dscn ...
// Build from the project folder:
//   clang -O2 -Iruntime -Itests tests/scene_tex.c runtime/scenedump.c runtime/texdecode.c \
//     -o tests/scene_tex.exe
// Why: shows how many different textures a scene uses, how much of that the cache already has,
// and what decoding costs. These numbers decide how the texture upload for the GPU is built.
#include "scenedump.h"
#include "texdecode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
static double now_ms(void)
{
  LARGE_INTEGER f, c;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return 1000.0 * (double)c.QuadPart / (double)f.QuadPart;
}
#else
#include <time.h>
static double now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return 1000.0 * (double)ts.tv_sec + (double)ts.tv_nsec / 1e6;
}
#endif

int main(int argc, char** argv)
{
  TexCache* c = texcache_new(96u * 1024 * 1024);
  double total_ms = 0;
  for (int a = 1; a < argc; a++)
  {
    Scene s;
    if (!scenedump_read(argv[a], &s))
    {
      printf("%s: cannot read\n", argv[a]);
      continue;
    }
    TexMem m;
    for (int q = 0; q < 4; q++)
      m.tex[q] = s.tex[q];
    for (int q = 0; q < 6; q++)
      m.pal[q] = s.pal[q];
    uint64_t dec0 = texcache_decodes(c);
    double t0 = now_ms();
    texcache_scene(c);
    int used = 0, distinct = 0, fresh = 0, nofit = 0;
    for (int i = 0; i < s.npolys; i++)
    {
      const Poly* p = &s.polys[i];
      if (p->n < 3 || ((p->teximage >> 26) & 7) == 0)
        continue;
      TexEntry e;
      used++;
      if (!texcache_get(c, &m, p->teximage, p->pltt, &e))
        nofit++;
      else if (e.fresh)
        fresh++;
    }
    double ms = now_ms() - t0;
    total_ms += ms;
    distinct = (int)(texcache_decodes(c) - dec0);
    printf("%s  polygons %4d  textured %4d  decoded now %3d  (%d changed or new)  %d without room  "
           "%.2f ms  cache %d textures %.1f MB\n",
           argv[a], s.npolys, used, distinct, fresh, nofit, ms, texcache_count(c),
           (double)texcache_bytes(c) / (1024.0 * 1024.0));
    scenedump_free(&s);
  }
  printf("total %.1f ms over %d scenes, %llu decodes\n", total_ms, argc - 1,
         (unsigned long long)texcache_decodes(c));
  texcache_free(c);
  return 0;
}
