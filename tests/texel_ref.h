//texel_ref.h: the texel decoder as it was in runtime/gpu3d.c at commit 73a07ea, copied word for
//word except for the names (ref_ prefix) and the memory arrays (ref_tex, ref_pal).
//why: texdecode.h moved this code. The tests compare the new code with this copy, so a mistake
//in the move shows up. It is a test oracle only and is never built into the game.
#pragma once
#include <stdint.h>

static uint8_t *ref_tex[4], *ref_pal[6];

static inline uint8_t ref_tex8(uint32_t o)
{
  uint8_t* p = ref_tex[(o >> 17) & 3];
  if (!p)
    return 0;
  return p[o & 0x1FFFF];
}
static inline uint16_t ref_pal16(uint32_t o)
{
  o %= 0x18000; //96 KB (6 slots of 16 KB). Careful: 0x17FFF does NOT work as a mask (it clears bit
                //15)
  uint8_t* p = ref_pal[o >> 14];
  if (!p)
    return 0;
  return (uint16_t)(p[o & 0x3FFF] | p[(o & 0x3FFF) + 1] << 8);
}

static inline uint16_t ref_media15(uint16_t c0, uint16_t c1, int p0, int p1, int div)
{
  return (uint16_t)((((c0 & 31) * p0 + (c1 & 31) * p1) / div) |
                    ((((c0 >> 5) & 31) * p0 + ((c1 >> 5) & 31) * p1) / div) << 5 |
                    ((((c0 >> 10) & 31) * p0 + ((c1 >> 10) & 31) * p1) / div) << 10);
}

//Color of a texel (RGB555) and its alpha 0..31
static uint16_t ref_texel(uint32_t ti, uint32_t pltt, int s, int t, int* alpha)
{
  int w = 8 << ((ti >> 20) & 7), h = 8 << ((ti >> 23) & 7);
  if ((ti >> 16) & 1)
  { //repetir S
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
  { //A3I5
    uint8_t b = ref_tex8(base + (uint32_t)(t * w + s));
    int a = b >> 5;
    *alpha = a * 4 + a / 2;
    return ref_pal16(pb + (b & 31) * 2);
  }
  case 2: //4 colores
    i = (ref_tex8(base + (uint32_t)((t * w + s) / 4)) >> ((s & 3) * 2)) & 3;
    if (trans0 && i == 0)
      *alpha = 0;
    return ref_pal16(pltt * 8 + i * 2);
  case 3: //16 colores
    i = (ref_tex8(base + (uint32_t)((t * w + s) / 2)) >> ((s & 1) * 4)) & 15;
    if (trans0 && i == 0)
      *alpha = 0;
    return ref_pal16(pb + i * 2);
  case 4: //256 colores
    i = ref_tex8(base + (uint32_t)(t * w + s));
    if (trans0 && i == 0)
      *alpha = 0;
    return ref_pal16(pb + i * 2);
  case 5:
  { //compressed in 4x4 blocks
    uint32_t raw_data = base + (uint32_t)((t / 4) * (w / 4) + s / 4) * 4;
    int idx = (ref_tex8(raw_data + (uint32_t)(t & 3)) >> ((s & 3) * 2)) & 3;
    uint32_t slot =
        (raw_data & 0x7FFFF) >= 0x40000 ? 0x10000 : 0; //data in slot 2: indices in slot 1 + 64 KB
    uint32_t info_dir = 0x20000 + slot + ((raw_data & 0x1FFFF) >> 1);
    uint16_t info = (uint16_t)(ref_tex8(info_dir) | ref_tex8(info_dir + 1) << 8);
    uint32_t pdir = pb + (uint32_t)(info & 0x3FFF) * 4;
    int mode = info >> 14;
    switch (idx)
    {
    case 0:
      return ref_pal16(pdir);
    case 1:
      return ref_pal16(pdir + 2);
    case 2:
      if (mode == 0 || mode == 2)
        return ref_pal16(pdir + 4);
      if (mode == 1)
        return ref_media15(ref_pal16(pdir), ref_pal16(pdir + 2), 1, 1, 2);
      return ref_media15(ref_pal16(pdir), ref_pal16(pdir + 2), 5, 3, 8);
    default:
      if (mode == 2)
        return ref_pal16(pdir + 6);
      if (mode == 3)
        return ref_media15(ref_pal16(pdir), ref_pal16(pdir + 2), 3, 5, 8);
      *alpha = 0;
      return 0;
    }
  }
  case 6:
  { //A5I3
    uint8_t b = ref_tex8(base + (uint32_t)(t * w + s));
    *alpha = b >> 3;
    return ref_pal16(pb + (b & 7) * 2);
  }
  case 7:
  { //color directo
    uint32_t o = base + (uint32_t)(t * w + s) * 2;
    uint16_t c = (uint16_t)(ref_tex8(o) | ref_tex8(o + 1) << 8);
    if (!(c & 0x8000))
      *alpha = 0;
    return c & 0x7FFF;
  }
  }
  return 0x7FFF;
}

