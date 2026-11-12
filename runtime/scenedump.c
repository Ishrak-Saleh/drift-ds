//Scene dump: saves one 3D scene to a file, and reads it back. See scenedump.h.
#include "scenedump.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int put(FILE* f, const void* p, size_t n)
{
  return fwrite(p, 1, n, f) == n;
}

int scenedump_write(const char* path, const Scene* s)
{
  if (!path || !s || !s->io || !s->polys || !s->color || !s->alpha)
    return 0;
  if (s->scale < 1 || s->scale > 4 || s->npolys < 0 || s->npolys > MAX_POLYS)
    return 0;
  SceneHeader h;
  memset(&h, 0, sizeof h);
  h.magic = SCENE_MAGIC;
  h.version = SCENE_VERSION;
  h.poly_size = (uint32_t)sizeof(Poly);
  h.scale = (uint32_t)s->scale;
  h.wbuf = s->wbuf;
  h.vp_x1 = s->vp_x1;
  h.vp_y1 = s->vp_y1;
  h.vp_x2 = s->vp_x2;
  h.vp_y2 = s->vp_y2;
  h.npolys = (uint32_t)s->npolys;
  for (int i = 0; i < SCENE_NTEX; i++)
    if (s->tex[i])
      h.tex_mask |= 1u << i;
  for (int i = 0; i < SCENE_NPAL; i++)
    if (s->pal[i])
      h.pal_mask |= 1u << i;

  char tmp[300];
  if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp)
    return 0;
  FILE* f = fopen(tmp, "wb");
  if (!f)
    return 0;
  size_t pixels = (size_t)256 * h.scale * 192 * h.scale;
  int ok = put(f, &h, sizeof h) && put(f, s->io, 0x400);
  for (int i = 0; ok && i < SCENE_NTEX; i++)
    if (s->tex[i])
      ok = put(f, s->tex[i], SCENE_TEX_SIZE);
  for (int i = 0; ok && i < SCENE_NPAL; i++)
    if (s->pal[i])
      ok = put(f, s->pal[i], SCENE_PAL_SIZE);
  ok = ok && put(f, s->polys, (size_t)s->npolys * sizeof(Poly)) && put(f, s->color, pixels * 2) &&
       put(f, s->alpha, pixels);
  ok = (fclose(f) == 0) && ok;
  if (ok)
  {
    remove(path); //Windows will not rename over an existing file
    ok = rename(tmp, path) == 0;
  }
  if (!ok)
    remove(tmp);
  return ok;
}

static int count_bits(uint32_t m)
{
  int n = 0;
  for (; m; m &= m - 1)
    n++;
  return n;
}

int scenedump_read(const char* path, Scene* out)
{
  if (!path || !out)
    return 0;
  memset(out, 0, sizeof *out);
  FILE* f = fopen(path, "rb");
  if (!f)
    return 0;
  SceneHeader h;
  if (fread(&h, sizeof h, 1, f) != 1 || h.magic != SCENE_MAGIC || h.version != SCENE_VERSION ||
      h.poly_size != sizeof(Poly) || h.scale < 1 || h.scale > 4 || h.npolys > MAX_POLYS ||
      (h.tex_mask >> SCENE_NTEX) || (h.pal_mask >> SCENE_NPAL))
  {
    fclose(f);
    return 0;
  }
  size_t pixels = (size_t)256 * h.scale * 192 * h.scale;
  size_t ntex = (size_t)count_bits(h.tex_mask), npal = (size_t)count_bits(h.pal_mask);
  size_t sz_io = 0x400, sz_tex = ntex * SCENE_TEX_SIZE, sz_pal = npal * SCENE_PAL_SIZE;
  size_t sz_poly = (size_t)h.npolys * sizeof(Poly), sz_col = pixels * 2, sz_alpha = pixels;
  size_t total = sz_io + sz_tex + sz_pal + sz_poly + sz_col + sz_alpha;
  //the file must be exactly as long as the header says
  if (fseek(f, 0, SEEK_END) != 0)
  {
    fclose(f);
    return 0;
  }
  long end = ftell(f);
  if (end < 0 || (size_t)end != sizeof h + total || fseek(f, (long)sizeof h, SEEK_SET) != 0)
  {
    fclose(f);
    return 0;
  }
  uint8_t* block = malloc(total);
  if (!block || fread(block, 1, total, f) != total)
  {
    free(block);
    fclose(f);
    return 0;
  }
  fclose(f);
  //all pieces start at a multiple of 4 inside the block (sizes above are), so the casts are safe
  uint8_t* p = block;
  out->io = p;
  p += sz_io;
  for (int i = 0; i < SCENE_NTEX; i++)
    if (h.tex_mask >> i & 1)
    {
      out->tex[i] = p;
      p += SCENE_TEX_SIZE;
    }
  for (int i = 0; i < SCENE_NPAL; i++)
    if (h.pal_mask >> i & 1)
    {
      out->pal[i] = p;
      p += SCENE_PAL_SIZE;
    }
  out->polys = (const Poly*)(const void*)p;
  p += sz_poly;
  out->color = (const uint16_t*)(const void*)p;
  p += sz_col;
  out->alpha = p;
  out->npolys = (int)h.npolys;
  out->scale = (int)h.scale;
  out->wbuf = h.wbuf;
  out->vp_x1 = h.vp_x1;
  out->vp_y1 = h.vp_y1;
  out->vp_x2 = h.vp_x2;
  out->vp_y2 = h.vp_y2;
  return 1;
}

void scenedump_free(Scene* s)
{
  if (!s)
    return;
  free((void*)s->io); //io is the start of the block read by scenedump_read
  memset(s, 0, sizeof *s);
}
