// Texture decoding of the DS 3D engine: reads one texel, a whole texture, and keeps a cache.
//
// Where it fits: gpu3d.c uses texmem_texel for the pixel by pixel drawing, and the GPU drawing
// (branch graphics) uses tex_decode and the cache to turn a DS texture into a plain picture the
// graphics card can read.
// Why: the DS stores textures in 7 formats (palettes, alpha bits, 4x4 compressed blocks) in VRAM
// banks. The graphics card does not know them, so each texture is decoded once on the CPU and
// kept while its bytes do not change.
//
// Memory model: TexMem is a copy of the texture slots (4 x 128 KB) and palette slots (6 x 16 KB)
// as they were when the scene was handed over. A slot that is not mapped is NULL and reads as 0.
//
// A decoded texel is stored in one 32 bit word: red 5 bits, green 5 bits, blue 5 bits and alpha
// 5 bits, one per byte (r | g << 8 | b << 16 | a << 24), so the shader can use the exact integers
// the CPU drawing uses.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct
{
  const uint8_t* tex[4]; // texture slots, 128 KB each
  const uint8_t* pal[6]; // palette slots, 16 KB each
} TexMem;

static inline uint8_t texmem_tex8(const TexMem* m, uint32_t o)
{
  const uint8_t* p = m->tex[(o >> 17) & 3];
  if (!p)
    return 0;
  return p[o & 0x1FFFF];
}

static inline uint16_t texmem_pal16(const TexMem* m, uint32_t o)
{
  o %= 0x18000; // 96 KB (6 slots of 16 KB). Careful: 0x17FFF does NOT work as a mask (it clears bit
                // 15)
  const uint8_t* p = m->pal[o >> 14];
  if (!p)
    return 0;
  return (uint16_t)(p[o & 0x3FFF] | p[(o & 0x3FFF) + 1] << 8);
}

static inline uint16_t texmem_mix15(uint16_t c0, uint16_t c1, int p0, int p1, int div)
{
  return (uint16_t)((((c0 & 31) * p0 + (c1 & 31) * p1) / div) |
                    ((((c0 >> 5) & 31) * p0 + ((c1 >> 5) & 31) * p1) / div) << 5 |
                    ((((c0 >> 10) & 31) * p0 + ((c1 >> 10) & 31) * p1) / div) << 10);
}

// Color of a texel (RGB555) and its alpha 0..31. ti is the TEXIMAGE_PARAM word, pltt the palette
// base. s and t are texels and may be outside the texture: the wrap flags of ti decide.
static inline uint16_t texmem_texel(const TexMem* m, uint32_t ti, uint32_t pltt, int s, int t,
                                    int* alpha)
{
  int w = 8 << ((ti >> 20) & 7), h = 8 << ((ti >> 23) & 7);
  if ((ti >> 16) & 1)
  { // repeat S
    if ((ti >> 18) & 1)
    {
      int per = s & (2 * w - 1);
      s = per >= w ? 2 * w - 1 - per : per;
    }
    else
      s &= w - 1;
  }
  else
    s = s < 0 ? 0 : s >= w ? w - 1 : s;
  if ((ti >> 17) & 1)
  {
    if ((ti >> 19) & 1)
    {
      int per = t & (2 * h - 1);
      t = per >= h ? 2 * h - 1 - per : per;
    }
    else
      t &= h - 1;
  }
  else
    t = t < 0 ? 0 : t >= h ? h - 1 : t;
  uint32_t base = (ti & 0xFFFF) * 8;
  int fmt = (ti >> 26) & 7;
  int trans0 = (ti >> 29) & 1;
  uint32_t pb = pltt * 16, i;
  *alpha = 31;
  switch (fmt)
  {
  case 1:
  { // A3I5
    uint8_t b = texmem_tex8(m, base + (uint32_t)(t * w + s));
    int a = b >> 5;
    *alpha = a * 4 + a / 2;
    return texmem_pal16(m, pb + (b & 31) * 2);
  }
  case 2: // 4 colors
    i = (texmem_tex8(m, base + (uint32_t)((t * w + s) / 4)) >> ((s & 3) * 2)) & 3;
    if (trans0 && i == 0)
      *alpha = 0;
    return texmem_pal16(m, pltt * 8 + i * 2);
  case 3: // 16 colors
    i = (texmem_tex8(m, base + (uint32_t)((t * w + s) / 2)) >> ((s & 1) * 4)) & 15;
    if (trans0 && i == 0)
      *alpha = 0;
    return texmem_pal16(m, pb + i * 2);
  case 4: // 256 colors
    i = texmem_tex8(m, base + (uint32_t)(t * w + s));
    if (trans0 && i == 0)
      *alpha = 0;
    return texmem_pal16(m, pb + i * 2);
  case 5:
  { // compressed in 4x4 blocks
    uint32_t raw_data = base + (uint32_t)((t / 4) * (w / 4) + s / 4) * 4;
    int idx = (texmem_tex8(m, raw_data + (uint32_t)(t & 3)) >> ((s & 3) * 2)) & 3;
    uint32_t slot =
        (raw_data & 0x7FFFF) >= 0x40000 ? 0x10000 : 0; // data in slot 2: indices in slot 1 + 64 KB
    uint32_t info_dir = 0x20000 + slot + ((raw_data & 0x1FFFF) >> 1);
    uint16_t info = (uint16_t)(texmem_tex8(m, info_dir) | texmem_tex8(m, info_dir + 1) << 8);
    uint32_t pdir = pb + (uint32_t)(info & 0x3FFF) * 4;
    int mode = info >> 14;
    switch (idx)
    {
    case 0:
      return texmem_pal16(m, pdir);
    case 1:
      return texmem_pal16(m, pdir + 2);
    case 2:
      if (mode == 0 || mode == 2)
        return texmem_pal16(m, pdir + 4);
      if (mode == 1)
        return texmem_mix15(texmem_pal16(m, pdir), texmem_pal16(m, pdir + 2), 1, 1, 2);
      return texmem_mix15(texmem_pal16(m, pdir), texmem_pal16(m, pdir + 2), 5, 3, 8);
    default:
      if (mode == 2)
        return texmem_pal16(m, pdir + 6);
      if (mode == 3)
        return texmem_mix15(texmem_pal16(m, pdir), texmem_pal16(m, pdir + 2), 3, 5, 8);
      *alpha = 0;
      return 0;
    }
  }
  case 6:
  { // A5I3
    uint8_t b = texmem_tex8(m, base + (uint32_t)(t * w + s));
    *alpha = b >> 3;
    return texmem_pal16(m, pb + (b & 7) * 2);
  }
  case 7:
  { // direct color
    uint32_t o = base + (uint32_t)(t * w + s) * 2;
    uint16_t c = (uint16_t)(texmem_tex8(m, o) | texmem_tex8(m, o + 1) << 8);
    if (!(c & 0x8000))
      *alpha = 0;
    return c & 0x7FFF;
  }
  }
  return 0x7FFF;
}

// ---- whole textures ----

static inline int tex_width(uint32_t ti)
{
  return 8 << ((ti >> 20) & 7);
}
static inline int tex_height(uint32_t ti)
{
  return 8 << ((ti >> 23) & 7);
}

// The parts of TEXIMAGE_PARAM that change the decoded picture: address, size, format and the
// "color 0 is transparent" flag. The wrap flags (bits 16 to 19) and the texgen bits are left out,
// the drawing applies the wrap itself.
#define TEX_KEY_MASK 0x3FF0FFFFu

// Decodes every texel of the texture into out[t * w + s] (w * h words, see the file header).
// Returns 0 if the format is 0 (no texture), else 1.
int tex_decode(const TexMem* m, uint32_t ti, uint32_t pltt, uint32_t* out);

// A 64 bit hash of every byte tex_decode can read for this texture: its data, its extra data
// (format 5) and the palette part it can reach. Equal hashes mean an equal picture. A changed
// byte outside those parts does not change it.
uint64_t tex_hash(const TexMem* m, uint32_t ti, uint32_t pltt);

// ---- cache of decoded textures ----
//
// One entry per (texture, palette). texcache_get decodes on first use, and again when the hash of
// the bytes changed (the game rewrites some textures, like the emblem). Entries not used for a
// number of scenes are dropped when the cache is over its size limit.

typedef struct TexCache TexCache;

typedef struct
{
  int w, h;
  const uint32_t* px; // w * h words, owned by the cache, valid until the next texcache_get
  uint32_t id; // stable number of the entry, for the graphics side to attach its texture to
  int fresh; // 1: decoded now (new or changed), so a copy on the graphics card is out of date
} TexEntry;

TexCache* texcache_new(size_t max_bytes);
void texcache_free(TexCache* c);

// Call once per scene before the first texcache_get of that scene.
void texcache_scene(TexCache* c);

// Returns 1 and fills out, or 0 when there is no texture (format 0) or no room.
int texcache_get(TexCache* c, const TexMem* m, uint32_t ti, uint32_t pltt, TexEntry* out);

// For tests and the log: entries alive, bytes held, decodes so far.
int texcache_count(const TexCache* c);
size_t texcache_bytes(const TexCache* c);
uint64_t texcache_decodes(const TexCache* c);
