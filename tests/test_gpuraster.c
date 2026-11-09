// test_gpuraster.c: draws recorded scenes with the GPU and compares them with the CPU picture.
//
// Usage: test_gpuraster [-v] [-d dir] scenes/scene-A-001.dscn scenes/scene-A-002.dscn ...
//   -v       list the first differing pixels of each failing scene
//   -d dir   also writes a bitmap per scene into dir that shows the differences
//            (grey = equal, green = rounding, blue = edge, red = differing, yellow = transparency)
// Build from the project folder:
//   clang -O2 -Iruntime -Itests tests/test_gpuraster.c tests/scenecmp.c runtime/gpuraster.c \
//     runtime/texdecode.c runtime/scenedump.c runtime/glctx.c runtime/glload.c -lopengl32 -lgdi32 \
//     -luser32 -o tests/test_gpuraster.exe
// What it does: for each scene it makes the hidden OpenGL context once, draws the polygon list
// with gpuraster.c, and scores the result against the picture the CPU drawing stored in the file
// (rule in scenecmp.h: 0.1 percent of the pixels, no more than 3 colour steps, no transparency
// mistakes, edges not counted). Exit code 0 when every scene passes.
// Why: this is the check for the GPU drawing. The files hold game data: keep the bitmaps out of
// git.
#include "glctx.h"
#include "gpuraster.h"
#include "scenecmp.h"
#include "scenedump.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static double now_ms(void)
{
  LARGE_INTEGER f, c;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return 1000.0 * (double)c.QuadPart / (double)f.QuadPart;
}

static void put32(FILE* f, uint32_t v)
{
  fputc(v & 255, f);
  fputc(v >> 8 & 255, f);
  fputc(v >> 16 & 255, f);
  fputc(v >> 24 & 255, f);
}

static void write_diff_bmp(const char* path, const uint8_t* d, const uint16_t* ref, int w, int h)
{
  FILE* f = fopen(path, "wb");
  if (!f)
    return;
  uint32_t size = 54 + (uint32_t)w * (uint32_t)h * 3;
  fputc('B', f);
  fputc('M', f);
  put32(f, size);
  put32(f, 0);
  put32(f, 54);
  put32(f, 40);
  put32(f, (uint32_t)w);
  put32(f, (uint32_t)h);
  put32(f, 1 | 24 << 16);
  put32(f, 0);
  put32(f, size - 54);
  put32(f, 2835);
  put32(f, 2835);
  put32(f, 0);
  put32(f, 0);
  for (int y = h - 1; y >= 0; y--)
    for (int x = 0; x < w; x++)
    {
      int i = y * w + x;
      uint8_t r, g, b;
      uint16_t c = ref[i];
      uint8_t grey = (uint8_t)(((c & 31) + ((c >> 5) & 31) + ((c >> 10) & 31)) * 255 / 93 / 2 + 40);
      switch (d[i])
      {
      case 1:
        r = 0, g = 200, b = 0;
        break;
      case 2:
        r = 60, g = 60, b = 255;
        break;
      case 3:
        r = 255, g = 0, b = 0;
        break;
      case 4:
        r = 255, g = 255, b = 0;
        break;
      default:
        r = g = b = grey;
      }
      fputc(b, f);
      fputc(g, f);
      fputc(r, f);
    }
  fclose(f);
}

int main(int argc, char** argv)
{
  const char* diff_dir = NULL;
  int first = 1, verbose = 0;
  if (argc > 1 && strcmp(argv[1], "-v") == 0)
  { // list the first differing pixels of each scene
    verbose = 1;
    argv++;
    argc--;
  }
  if (argc > 2 && strcmp(argv[1], "-d") == 0)
  {
    diff_dir = argv[2];
    first = 3;
  }
  char err[400] = "";
  GlCtx* ctx = glctx_create(err, sizeof err);
  if (!ctx)
  {
    printf("no OpenGL context: %s\n", err);
    return 2;
  }
  CmpRule rule = cmp_default_rule();
  GpuRaster* g = NULL;
  int g_scale = 0, failed = 0, done = 0;
  double sum_ms = 0, worst_ms = 0, mb = 0, ms_ = 0, mr = 0, mu = 0;
  for (int a = first; a < argc; a++)
  {
    Scene s;
    if (!scenedump_read(argv[a], &s))
    {
      printf("%s: cannot read\n", argv[a]);
      failed++;
      continue;
    }
    if (s.scale != g_scale)
    {
      gpuraster_free(g);
      g = gpuraster_new(s.scale, err, sizeof err);
      g_scale = s.scale;
      if (!g)
      {
        printf("GPU drawing did not start: %s\n", err);
        scenedump_free(&s);
        glctx_destroy(ctx);
        return 2;
      }
    }
    int w = 256 * s.scale, h = 192 * s.scale;
    uint16_t* c = malloc((size_t)w * h * 2);
    uint8_t* al = malloc((size_t)w * h);
    uint8_t* diff = malloc((size_t)w * h);
    GpuScene gs;
    gs.io = s.io;
    gs.wbuf = s.wbuf;
    gs.vp_x1 = s.vp_x1;
    gs.vp_y1 = s.vp_y1;
    gs.vp_x2 = s.vp_x2;
    gs.vp_y2 = s.vp_y2;
    gs.polys = s.polys;
    gs.npolys = s.npolys;
    for (int q = 0; q < 4; q++)
      gs.mem.tex[q] = s.tex[q];
    for (int q = 0; q < 6; q++)
      gs.mem.pal[q] = s.pal[q];
    double t0 = now_ms();
    int ok = gpuraster_draw(g, &gs, c, al);
    double ms = now_ms() - t0;
    if (!ok)
    {
      printf("%s: the GPU drawing reported an error\n", argv[a]);
      failed++;
    }
    else
    {
      CmpResult r = cmp_pictures(s.color, s.alpha, c, al, w, h, &rule, diff);
      const GpuStats* st = gpuraster_stats(g);
      printf("%s  polys %4d  groups %4d  tris %4d  tex up %2d%s  %.2f ms  edge %5d  differing %5d "
             "(%.3f%%)  worst %2d  alpha wrong %d  %s\n",
             argv[a], s.npolys, st->groups, st->triangles, st->textures_uploaded,
             st->textures_missing ? " MISSING" : "", ms, r.edge, r.differing, r.percent, r.worst,
             r.alpha_wrong, r.pass ? "ok" : "FAIL");
      if (!r.pass)
        failed++;
      for (int i = 0, shown = 0; verbose && !r.pass && i < w * h && shown < 12; i++)
        if (diff[i] >= 3)
        {
          printf("    at %d,%d  cpu %02X %02X %02X a%2d   gpu %02X %02X %02X a%2d\n", i % w, i / w,
                 s.color[i] & 31, s.color[i] >> 5 & 31, s.color[i] >> 10 & 31, s.alpha[i],
                 c[i] & 31, c[i] >> 5 & 31, c[i] >> 10 & 31, al[i]);
          int nearby = 0; // does the GPU colour equal a neighbour of the CPU picture?
          for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
            {
              int x = i % w + dx, y = i / w + dy;
              if (x < 0 || y < 0 || x >= w || y >= h || (!dx && !dy))
                continue;
              uint16_t n = s.color[y * w + x];
              int d = abs((n & 31) - (c[i] & 31));
              d = d > abs((n >> 5 & 31) - (c[i] >> 5 & 31)) ? d
                                                            : abs((n >> 5 & 31) - (c[i] >> 5 & 31));
              d = d > abs((n >> 10 & 31) - (c[i] >> 10 & 31))
                      ? d
                      : abs((n >> 10 & 31) - (c[i] >> 10 & 31));
              if (d <= 1)
                nearby = 1;
            }
          printf("      %s\n",
                 nearby ? "(the GPU colour equals a neighbouring CPU pixel: shifted by one)"
                        : "(no neighbour explains it)");
          shown++;
        }
      if (diff_dir && (!r.pass || a == first))
      {
        const char* base = strrchr(argv[a], '/');
        base = base ? base + 1 : argv[a];
        char out[600];
        snprintf(out, sizeof out, "%s/%s.diff.bmp", diff_dir, base);
        write_diff_bmp(out, diff, s.color, w, h);
      }
      mb += st->ms_build, ms_ += st->ms_submit, mr += st->ms_read, mu += st->ms_unpack;
      sum_ms += ms;
      if (ms > worst_ms)
        worst_ms = ms;
      done++;
    }
    free(c);
    free(al);
    free(diff);
    scenedump_free(&s);
  }
  printf(
      "%d scenes drawn, %d failed, mean %.2f ms, worst %.2f ms (includes the picture readback)\n",
      done, failed, done ? sum_ms / done : 0, worst_ms);
  if (done)
    printf("mean split: build %.2f  submit %.2f  read (waits for the card) %.2f  unpack %.2f ms\n",
           mb / done, ms_ / done, mr / done, mu / done);
  gpuraster_free(g);
  glctx_destroy(ctx);
  return failed ? 1 : 0;
}
