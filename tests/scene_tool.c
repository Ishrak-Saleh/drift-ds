// scene_tool.c: looks at the scene dumps in scenes/ (see runtime/scenedump.h).
//
// Usage:
//   scene_tool info scenes/scene-X.dscn          what is in the file
//   scene_tool bmp scenes/scene-X.dscn out.bmp   the stored picture as a bitmap (transparent =
//   checker) scene_tool list scenes                       one line per scene: scale, polygons,
//   textures
// Build from the project folder:
//   clang -Iruntime -Itests tests/scene_tool.c runtime/scenedump.c -o tests/scene_tool.exe
// Why: the dumps are binary and hold the picture the CPU drawing made. This shows them without the
// game. The files hold game data: keep the output pictures out of git too.
#include "scenedump.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

static int count_set(const void* const* a, int n)
{
  int c = 0;
  for (int i = 0; i < n; i++)
    c += a[i] != NULL;
  return c;
}

static void print_info(const char* path, const Scene* s, int one_line)
{
  int w = 256 * s->scale, h = 192 * s->scale;
  int transparent = 0, translucent = 0, textured = 0;
  for (int i = 0; i < w * h; i++)
    transparent += s->alpha[i] == 0, translucent += s->alpha[i] > 0 && s->alpha[i] < 31;
  for (int i = 0; i < s->npolys; i++)
    textured += ((s->polys[i].teximage >> 26) & 7) != 0;
  int ntex = count_set((const void* const*)s->tex, SCENE_NTEX);
  int npal = count_set((const void* const*)s->pal, SCENE_NPAL);
  if (one_line)
  {
    printf("%s  scale %d  %dx%d  polygons %d  textured %d  tex slots %d  pal slots %d\n", path,
           s->scale, w, h, s->npolys, textured, ntex, npal);
    return;
  }
  printf("file: %s\n", path);
  printf("picture: %dx%d (scale %d), %s buffer, viewport %d,%d to %d,%d\n", w, h, s->scale,
         s->wbuf ? "W" : "Z", s->vp_x1, s->vp_y1, s->vp_x2, s->vp_y2);
  printf("polygons: %d (%d textured)\n", s->npolys, textured);
  printf("texture slots mapped: %d of %d, palette slots mapped: %d of %d\n", ntex, SCENE_NTEX, npal,
         SCENE_NPAL);
  printf("pixels: %d transparent, %d translucent, %d total\n", transparent, translucent, w * h);
}

static void put32(FILE* f, uint32_t v)
{
  fputc(v & 255, f);
  fputc(v >> 8 & 255, f);
  fputc(v >> 16 & 255, f);
  fputc(v >> 24 & 255, f);
}

static int write_bmp(const char* path, const Scene* s)
{
  int w = 256 * s->scale, h = 192 * s->scale;
  FILE* f = fopen(path, "wb");
  if (!f)
    return 0;
  uint32_t row = (uint32_t)w * 3, pad = (4 - row % 4) % 4, size = 54 + (row + pad) * (uint32_t)h;
  fputc('B', f);
  fputc('M', f);
  put32(f, size);
  put32(f, 0);
  put32(f, 54);
  put32(f, 40);
  put32(f, (uint32_t)w);
  put32(f, (uint32_t)h);
  fputc(1, f);
  fputc(0, f);
  fputc(24, f);
  fputc(0, f);
  for (int i = 0; i < 6; i++)
    put32(f, 0);
  for (int y = h - 1; y >= 0; y--)
  {
    for (int x = 0; x < w; x++)
    {
      int i = y * w + x;
      uint16_t c = s->color[i];
      int r = (c & 31) * 255 / 31, g = (c >> 5 & 31) * 255 / 31, b = (c >> 10 & 31) * 255 / 31;
      if (s->alpha[i] == 0)
        r = g = b = ((x / 8 + y / 8) & 1) ? 90 : 60; // checker for transparent
      fputc(b, f);
      fputc(g, f);
      fputc(r, f);
    }
    for (uint32_t p = 0; p < pad; p++)
      fputc(0, f);
  }
  return fclose(f) == 0;
}

static int list_dir(const char* dir)
{
#ifdef _WIN32
  char pat[300];
  snprintf(pat, sizeof pat, "%s\\*.dscn", dir);
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pat, &fd);
  if (h == INVALID_HANDLE_VALUE)
  {
    printf("no scenes in %s\n", dir);
    return 1;
  }
  do
  {
    char p[600];
    snprintf(p, sizeof p, "%s\\%s", dir, fd.cFileName);
    Scene s;
    if (scenedump_read(p, &s))
    {
      print_info(fd.cFileName, &s, 1);
      scenedump_free(&s);
    }
    else
      printf("%s  not a valid scene\n", fd.cFileName);
  } while (FindNextFileA(h, &fd));
  FindClose(h);
  return 0;
#else
  (void)dir;
  printf("list needs Windows\n");
  return 1;
#endif
}

int main(int argc, char** argv)
{
  if (argc >= 3 && !strcmp(argv[1], "list"))
    return list_dir(argv[2]);
  if (argc >= 3 && (!strcmp(argv[1], "info") || !strcmp(argv[1], "bmp")))
  {
    Scene s;
    if (!scenedump_read(argv[2], &s))
    {
      printf("cannot read %s (missing, damaged or from another version)\n", argv[2]);
      return 1;
    }
    int rc = 0;
    if (!strcmp(argv[1], "info"))
      print_info(argv[2], &s, 0);
    else if (argc < 4 || !write_bmp(argv[3], &s))
    {
      printf("usage: scene_tool bmp <scene> <out.bmp>\n");
      rc = 1;
    }
    else
      printf("wrote %s\n", argv[3]);
    scenedump_free(&s);
    return rc;
  }
  printf("usage: scene_tool info <scene> | bmp <scene> <out.bmp> | list <folder>\n");
  return 1;
}
