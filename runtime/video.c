//Video of the DS: the 9 VRAM banks and the two 2D engines (A and B).
//Each bank (A..I) is assigned with VRAMCNT to a use: backgrounds or sprites of an
//engine, extended palettes, 3D textures or direct access (LCDC). Here page tables
//of 16 KB are kept for each use.
//The engines draw line by line: 4 backgrounds + sprites, sorted by
//priority, with windows, transparencies and the master brightness applied.
//For now the whole frame is drawn on reaching VBlank.
#include "compat.h"
#include "video.h"
#include "gpu3d.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

uint8_t g_palette[0x800], g_oam[0x800];

static uint8_t bA[0x20000], bB[0x20000], bC[0x20000], bD[0x20000], bE[0x10000];
static uint8_t bF[0x4000], bG[0x4000], bH[0x8000], bI[0x4000];
static uint8_t* const bank[9] = {bA, bB, bC, bD, bE, bF, bG, bH, bI};
static const uint32_t size_bank[9] = {0x20000, 0x20000, 0x20000, 0x20000, 0x10000,
                                      0x4000,  0x4000,  0x8000,  0x4000};
//what the image build reads: the real VRAM and 3D, or the snapshot (build on another thread)
static uint8_t* const* bank_act = bank;
static const uint16_t *c3_act, *c3hd_act;
static const uint8_t *a3_act, *a3hd_act;
static uint8_t vramcnt[9];

//Pages of 16 KB of each zone
static uint8_t *bg_a[32], *obj_a[16], *bg_b[8], *obj_b[8], *lcdc[41];
static uint8_t *ext_bg_a[4], *ext_obj_a, *ext_bg_b[4], *ext_obj_b; //paletas extendidas (8 KB)
uint8_t *g_tex[4], *g_texpal[6]; //for the 3D (later)

static void map(uint8_t** table_data, int n, uint32_t off, uint8_t* mem, uint32_t nbytes)
{
  for (uint32_t k = 0; k < nbytes; k += 0x4000)
  {
    int p = (int)((off + k) / 0x4000);
    if (p < n)
      table_data[p] = mem + k;
  }
}

static void recalculate(void)
{
  memset(bg_a, 0, sizeof bg_a);
  memset(obj_a, 0, sizeof obj_a);
  memset(bg_b, 0, sizeof bg_b);
  memset(obj_b, 0, sizeof obj_b);
  memset(lcdc, 0, sizeof lcdc);
  memset(ext_bg_a, 0, sizeof ext_bg_a);
  memset(ext_bg_b, 0, sizeof ext_bg_b);
  ext_obj_a = ext_obj_b = NULL;
  memset(g_tex, 0, sizeof g_tex);
  memset(g_texpal, 0, sizeof g_texpal);
  static const uint32_t lcdc_off[9] = {0,       0x20000, 0x40000, 0x60000, 0x80000,
                                       0x90000, 0x94000, 0x98000, 0xA0000};
  for (int b = 0; b < 9; b++)
  {
    uint8_t v = vramcnt[b];
    if (!(v & 0x80))
      continue;
    int mst = v & 7, ofs = (v >> 3) & 3;
    uint8_t* m = bank[b];
    uint32_t t = size_bank[b];
    if (mst == 0)
    {
      map(lcdc, 41, lcdc_off[b], m, t);
      continue;
    }
    switch (b)
    {
    case 0:
    case 1: //a, B
      switch (mst & 3)
      {
      case 1:
        map(bg_a, 32, ofs * 0x20000, m, t);
        break;
      case 2:
        map(obj_a, 16, (ofs & 1) * 0x20000, m, t);
        break;
      case 3:
        g_tex[ofs] = m;
        break;
      }
      break;
    case 2:
    case 3: //C, D
      switch (mst)
      {
      case 1:
        map(bg_a, 32, ofs * 0x20000, m, t);
        break;
      case 3:
        g_tex[ofs] = m;
        break;
      case 4:
        if (b == 2)
          map(bg_b, 8, 0, m, t);
        else
          map(obj_b, 8, 0, m, t);
        break;
      } //mst 2: ARM7 VRAM (separate)
      break;
    case 4: //E
      switch (mst)
      {
      case 1:
        map(bg_a, 32, 0, m, t);
        break;
      case 2:
        map(obj_a, 16, 0, m, t);
        break;
      case 3:
        for (int k = 0; k < 4; k++)
          g_texpal[k] = m + k * 0x4000;
        break;
      case 4:
        for (int k = 0; k < 4; k++)
          ext_bg_a[k] = m + k * 0x2000;
        break;
      }
      break;
    case 5:
    case 6:
    { //F, G
      uint32_t off = 0x4000 * (ofs & 1) + 0x10000 * (ofs >> 1);
      switch (mst)
      {
      case 1:
        map(bg_a, 32, off, m, t);
        break;
      case 2:
        map(obj_a, 16, off, m, t);
        break;
      case 3:
        g_texpal[(ofs & 1) + (ofs >> 1) * 4] = m;
        break;
      case 4:
        ext_bg_a[(ofs & 1) * 2] = m;
        ext_bg_a[(ofs & 1) * 2 + 1] = m + 0x2000;
        break;
      case 5:
        ext_obj_a = m;
        break;
      }
      break;
    }
    case 7: //H
      switch (mst & 3)
      {
      case 1:
        map(bg_b, 8, 0, m, t);
        break;
      case 2:
        for (int k = 0; k < 4; k++)
          ext_bg_b[k] = m + k * 0x2000;
        break;
      }
      break;
    case 8: //I
      switch (mst & 3)
      {
      case 1:
        map(bg_b, 8, 0x8000, m, t);
        break;
      case 2:
        map(obj_b, 8, 0, m, t);
        break;
      case 3:
        ext_obj_b = m;
        break;
      }
      break;
    }
  }
}

void video_vramcnt(int b, uint8_t v)
{
  if (b < 0 || b > 8 || vramcnt[b] == v)
    return;
  vramcnt[b] = v;
  recalculate();
}

uint8_t* vram_ptr9(uint32_t a)
{
  uint8_t* p;
  switch ((a >> 21) & 7)
  {
  case 0:
    p = bg_a[(a & 0x7FFFF) >> 14];
    break;
  case 1:
    p = bg_b[(a & 0x1FFFF) >> 14];
    break;
  case 2:
    p = obj_a[(a & 0x3FFFF) >> 14];
    break;
  case 3:
    p = obj_b[(a & 0x1FFFF) >> 14];
    break;
  default:
  {
    uint32_t o = a & 0xFFFFF;
    p = o < 0xA4000 ? lcdc[o >> 14] : NULL;
    break;
  }
  }
  return p ? p + (a & 0x3FFF) : NULL;
}

//---- motores 2D ----------------------------------------------------------------
typedef struct
{
  const uint8_t* r; //registers of the engine
  const uint16_t *pal_bg, *pal_obj;
  const uint8_t* oam;
  uint8_t **bg, **obj;
  uint32_t bg_mask, obj_mask;
  uint8_t **ext_bg, *ext_obj;
  int is_a;
} Motor;

static inline uint16_t r16(const Motor* m, int o)
{
  return (uint16_t)(m->r[o] | m->r[o + 1] << 8);
}
static inline uint32_t r32(const Motor* m, int o)
{
  return r16(m, o) | (uint32_t)r16(m, o + 2) << 16;
}

static inline uint8_t v8(uint8_t** t, uint32_t mask, uint32_t o)
{
  o &= mask;
  uint8_t* p = t[o >> 14];
  return p ? p[o & 0x3FFF] : 0;
}
static inline uint16_t v16(uint8_t** t, uint32_t mask, uint32_t o)
{
  return (uint16_t)(v8(t, mask, o) | v8(t, mask, o + 1) << 8);
}
static inline uint16_t ext16(uint8_t* slot, uint32_t idx)
{
  return slot ? (uint16_t)(slot[idx * 2] | slot[idx * 2 + 1] << 8) : 0;
}

#define PX_SET 0x8000 //in the line buffers: there is a pixel

static uint16_t lin_bg[4][256];
static uint16_t lin_obj[256];
static uint8_t prio_obj[256], semi_obj[256], window_obj[256];
static uint8_t alpha_3d[256]; //alpha of background 0 when it is 3D
static uint16_t
    lin_a15[256]; //the line of engine A in 15 bits, before the master brightness (for the capture)
static uint8_t
    mark3d[256]; //rescale: at this point the 3D is seen as is (it can be taken from the big 3D)
static uint16_t brightness_a; //MASTER_BRIGHT of engine A on the current line
static int hofs_3d; //horizontal shift of the 3D layer on the current line (the menu uses it)

static void background_text(const Motor* m, int n, int y)
{
  uint16_t cnt = r16(m, 0x08 + 2 * n), dc_lo = r16(m, 0);
  uint32_t dc = r32(m, 0);
  int hofs = r16(m, 0x10 + 4 * n) & 0x1FF, vofs = r16(m, 0x12 + 4 * n) & 0x1FF;
  int w = (cnt >> 14) & 1 ? 512 : 256, h = (cnt >> 14) & 2 ? 512 : 256;
  uint32_t chr = ((cnt >> 2) & 15) * 0x4000 + (m->is_a ? ((dc >> 24) & 7) * 0x10000 : 0);
  uint32_t scr = ((cnt >> 8) & 31) * 0x800 + (m->is_a ? ((dc >> 27) & 7) * 0x10000 : 0);
  int bpp8 = (cnt >> 7) & 1;
  uint8_t* ext = NULL;
  if (bpp8 && (dc >> 30) & 1)
    ext = m->ext_bg[(n < 2 && (cnt & 0x2000)) ? n + 2 : n];
  int yy = (y + vofs) & (h - 1);
  (void)dc_lo;
  for (int x = 0; x < 256; x++)
  {
    int xx = (x + hofs) & (w - 1);
    uint32_t block = scr + (uint32_t)(((yy >> 8) * (w >> 8) + (xx >> 8)) * 0x800);
    uint16_t e = v16(m->bg, m->bg_mask,
                     block + (uint32_t)((((yy & 255) >> 3) * 32 + ((xx & 255) >> 3)) * 2));
    int px = xx & 7, py = yy & 7;
    if (e & 0x400)
      px = 7 - px;
    if (e & 0x800)
      py = 7 - py;
    uint32_t tile = e & 0x3FF;
    uint16_t col;
    if (bpp8)
    {
      uint8_t i = v8(m->bg, m->bg_mask, chr + tile * 64 + (uint32_t)(py * 8 + px));
      if (!i)
      {
        lin_bg[n][x] = 0;
        continue;
      }
      col = ext ? ext16(ext, (uint32_t)((e >> 12) * 256 + i)) : m->pal_bg[i];
    }
    else
    {
      uint8_t b = v8(m->bg, m->bg_mask, chr + tile * 32 + (uint32_t)(py * 4 + px / 2));
      uint8_t i = (px & 1) ? b >> 4 : b & 15;
      if (!i)
      {
        lin_bg[n][x] = 0;
        continue;
      }
      col = m->pal_bg[(e >> 12) * 16 + i];
    }
    lin_bg[n][x] = col | PX_SET;
  }
}

//Backgrounds that rotate/scale (affine) and their extended variants (bitmap)
static int32_t ref_x[2][2], ref_y[2][2]; //internal position per engine and background (2 and 3)

static void background_affine(const Motor* m, int n, int y, int extended)
{
  uint16_t cnt = r16(m, 0x08 + 2 * n);
  uint32_t dc = r32(m, 0);
  int o = n == 2 ? 0x20 : 0x30;
  int16_t pa = (int16_t)r16(m, o), pc = (int16_t)r16(m, o + 4);
  int32_t *rx = &ref_x[!m->is_a][n - 2], *ry = &ref_y[!m->is_a][n - 2];
  int sz = (cnt >> 14) & 3;
  int wraps = (cnt >> 13) & 1;
  int32_t x0 = *rx, y0 = *ry;
  (void)y;
  uint32_t chr = ((cnt >> 2) & 15) * 0x4000 + (m->is_a ? ((dc >> 24) & 7) * 0x10000 : 0);
  uint32_t scr = ((cnt >> 8) & 31) * 0x800 + (m->is_a ? ((dc >> 27) & 7) * 0x10000 : 0);
  int type = 0, w,
      h; //0 normal affine, 1 affine with 16 bit map, 2 bitmap 256 colors, 3 direct bitmap
  if (!extended)
  {
    w = h = 128 << sz;
  }
  else if (!(cnt & 0x80))
  {
    type = 1;
    w = h = 128 << sz;
  }
  else
  {
    type = (cnt & 4) ? 3 : 2;
    static const int bw[4] = {128, 256, 512, 512}, bh[4] = {128, 256, 256, 512};
    w = bw[sz];
    h = bh[sz];
    scr = ((cnt >> 8) & 31) * 0x4000;
  }
  uint8_t* ext = (dc >> 30) & 1 ? m->ext_bg[n] : NULL;
  for (int x = 0; x < 256; x++)
  {
    int tx = (x0 + pa * x) >> 8, ty = (y0 + pc * x) >> 8;
    if (wraps)
    {
      tx &= w - 1;
      ty &= h - 1;
    }
    else if (tx < 0 || ty < 0 || tx >= w || ty >= h)
    {
      lin_bg[n][x] = 0;
      continue;
    }
    uint16_t col = 0;
    int opaque = 0;
    if (type == 0)
    {
      uint8_t t = v8(m->bg, m->bg_mask, scr + (uint32_t)((ty >> 3) * (w >> 3) + (tx >> 3)));
      uint8_t i = v8(m->bg, m->bg_mask, chr + t * 64u + (uint32_t)((ty & 7) * 8 + (tx & 7)));
      if (i)
      {
        col = m->pal_bg[i];
        opaque = 1;
      }
    }
    else if (type == 1)
    {
      uint16_t e = v16(m->bg, m->bg_mask, scr + (uint32_t)(((ty >> 3) * (w >> 3) + (tx >> 3)) * 2));
      int px = tx & 7, py = ty & 7;
      if (e & 0x400)
        px = 7 - px;
      if (e & 0x800)
        py = 7 - py;
      uint8_t i = v8(m->bg, m->bg_mask, chr + (e & 0x3FFu) * 64 + (uint32_t)(py * 8 + px));
      if (i)
      {
        col = ext ? ext16(ext, (uint32_t)((e >> 12) * 256 + i)) : m->pal_bg[i];
        opaque = 1;
      }
    }
    else if (type == 2)
    {
      uint8_t i = v8(m->bg, m->bg_mask, scr + (uint32_t)(ty * w + tx));
      if (i)
      {
        col = m->pal_bg[i];
        opaque = 1;
      }
    }
    else
    {
      uint16_t c = v16(m->bg, m->bg_mask, scr + (uint32_t)(ty * w + tx) * 2);
      if (c & 0x8000)
      {
        col = c;
        opaque = 1;
      }
    }
    lin_bg[n][x] = opaque ? (col & 0x7FFF) | PX_SET : 0;
  }
  *rx += (int16_t)r16(m, o + 2); //PB
  *ry += (int16_t)r16(m, o + 6); //PD
}

static void reset_affine(const Motor* m)
{
  int k = !m->is_a;
  for (int n = 0; n < 2; n++)
  {
    int o = n == 0 ? 0x28 : 0x38;
    ref_x[k][n] = (int32_t)(r32(m, o) << 4) >> 4;
    ref_y[k][n] = (int32_t)(r32(m, o + 4) << 4) >> 4;
  }
}

static const uint8_t obj_w[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
static const uint8_t obj_h[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};

static void sprites(const Motor* m, int y)
{
  uint32_t dc = r32(m, 0);
  memset(lin_obj, 0, sizeof lin_obj);
  memset(prio_obj, 4, sizeof prio_obj);
  memset(semi_obj, 0, sizeof semi_obj);
  memset(window_obj, 0, sizeof window_obj);
  if (!((dc >> 12) & 1))
    return;
  for (int i = 127; i >= 0; i--)
  { //from the last to the first: the lowest index wins
    const uint8_t* e = m->oam + i * 8;
    uint16_t a0 = (uint16_t)(e[0] | e[1] << 8), a1 = (uint16_t)(e[2] | e[3] << 8),
             a2 = (uint16_t)(e[4] | e[5] << 8);
    int affine = (a0 >> 8) & 1;
    if (!affine && ((a0 >> 9) & 1))
      continue; //hidden
    int shape = (a0 >> 14) & 3, nbytes = (a1 >> 14) & 3;
    if (shape == 3)
      continue;
    int w = obj_w[shape][nbytes], h = obj_h[shape][nbytes];
    int twice = affine && ((a0 >> 9) & 1);
    int bw = twice ? w * 2 : w, bh = twice ? h * 2 : h;
    int oy = a0 & 0xFF;
    if (oy + bh > 256)
      oy -= 256;
    int row = y - oy;
    if (row < 0 || row >= bh)
      continue;
    int ox = a1 & 0x1FF;
    if (ox >= 256)
      ox -= 512;
    int mode = (a0 >> 10) & 3, bpp8 = (a0 >> 13) & 1;
    int prio = (a2 >> 10) & 3;
    uint32_t tile = a2 & 0x3FF;
    int16_t pa = 256, pb = 0, pc = 0, pd = 256;
    if (affine)
    {
      const uint8_t* p = m->oam + ((a1 >> 9) & 31) * 32;
      pa = (int16_t)(p[6] | p[7] << 8);
      pb = (int16_t)(p[14] | p[15] << 8);
      pc = (int16_t)(p[22] | p[23] << 8);
      pd = (int16_t)(p[30] | p[31] << 8);
    }
    for (int bx = 0; bx < bw; bx++)
    {
      int sx = ox + bx;
      if (sx < 0 || sx >= 256)
        continue;
      int tx, ty;
      if (affine)
      {
        int cx = bx - bw / 2, cy = row - bh / 2;
        tx = ((pa * cx + pb * cy) >> 8) + w / 2;
        ty = ((pc * cx + pd * cy) >> 8) + h / 2;
        if (tx < 0 || ty < 0 || tx >= w || ty >= h)
          continue;
      }
      else
      {
        tx = (a1 & 0x1000) ? w - 1 - bx : bx;
        ty = (a1 & 0x2000) ? h - 1 - row : row;
      }
      uint16_t col = 0;
      int opaque = 0;
      if (mode == 3)
      { //bitmap sprite (direct color)
        uint32_t dir;
        if ((dc >> 6) & 1)
          dir = tile * (128u << ((dc >> 22) & 1)) + (uint32_t)(ty * w + tx) * 2;
        else
        {
          int width = (dc >> 5) & 1 ? 256 : 128;
          uint32_t mask = width == 256 ? 0x1F : 0x0F;
          dir = (tile & mask) * 0x10 + (tile & ~mask) * 0x80 + (uint32_t)(ty * width + tx) * 2;
        }
        uint16_t c = v16(m->obj, m->obj_mask, dir);
        if (c & 0x8000)
        {
          col = c;
          opaque = 1;
        }
      }
      else
      {
        uint32_t base;
        int t_x = tx >> 3, t_y = ty >> 3;
        if ((dc >> 4) & 1)
        { //mapeo 1D
          uint32_t limit = 32u << ((dc >> 20) & 3);
          base = tile * limit + (uint32_t)((t_y * (w >> 3) + t_x) * (bpp8 ? 64 : 32));
        }
        else
        { //2D mapping: grid of 32x32 tiles of 32 bytes
          base = (tile + (uint32_t)(t_y * 32 + t_x * (bpp8 ? 2 : 1))) * 32;
        }
        int px = tx & 7, py = ty & 7;
        if (bpp8)
        {
          uint8_t ix = v8(m->obj, m->obj_mask, base + (uint32_t)(py * 8 + px));
          if (ix)
          {
            opaque = 1;
            col = ((dc >> 31) & 1) ? ext16(m->ext_obj, (uint32_t)((a2 >> 12) * 256 + ix))
                                   : m->pal_obj[ix];
          }
        }
        else
        {
          uint8_t b = v8(m->obj, m->obj_mask, base + (uint32_t)(py * 4 + px / 2));
          uint8_t ix = (px & 1) ? b >> 4 : b & 15;
          if (ix)
          {
            opaque = 1;
            col = m->pal_obj[(a2 >> 12) * 16 + ix];
          }
        }
      }
      if (!opaque)
        continue;
      if (mode == 2)
      {
        window_obj[sx] = 1;
        continue;
      }
      if (mode == 3 && !(a2 >> 12))
        continue; //bitmap with alpha 0: not seen
      if (prio <= prio_obj[sx])
      {
        lin_obj[sx] = (col & 0x7FFF) | PX_SET;
        prio_obj[sx] = (uint8_t)prio;
        //1 = semi-transparent (uses BLDALPHA). Bitmap: 2 + its own alpha (0-15, in attr2)
        semi_obj[sx] = mode == 1 ? 1 : mode == 3 ? (uint8_t)(2 + (a2 >> 12)) : 0;
      }
    }
  }
}

static inline uint32_t a_rgb(uint16_t c)
{
  uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
  return (r * 255 / 31) << 16 | (g * 255 / 31) << 8 | (b * 255 / 31);
}

static uint16_t mix(uint16_t a, uint16_t b, int eva, int evb)
{
  int r = ((a & 31) * eva + (b & 31) * evb) >> 4;
  int g = (((a >> 5) & 31) * eva + ((b >> 5) & 31) * evb) >> 4;
  int bl = (((a >> 10) & 31) * eva + ((b >> 10) & 31) * evb) >> 4;
  if (r > 31)
    r = 31;
  if (g > 31)
    g = 31;
  if (bl > 31)
    bl = 31;
  return (uint16_t)(r | g << 5 | bl << 10);
}

static uint16_t brightness(uint16_t c, int mode, int f)
{
  if (f > 16)
    f = 16;
  int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
  if (mode == 1)
  {
    r += (31 - r) * f / 16;
    g += (31 - g) * f / 16;
    b += (31 - b) * f / 16;
  }
  else
  {
    r -= r * f / 16;
    g -= g * f / 16;
    b -= b * f / 16;
  }
  return (uint16_t)(r | g << 5 | b << 10);
}

static void draw_line(const Motor* m, int y, uint32_t* out)
{
  uint32_t dc = r32(m, 0);
  if (m->is_a)
    memset(mark3d, 0, sizeof mark3d);
  int mode_screen = (dc >> 16) & 3;
  if (!m->is_a)
    mode_screen &= 1;
  if (mode_screen == 0 || (dc & 0x80))
  { //off or forced white
    for (int x = 0; x < 256; x++)
      out[x] = 0xFFFFFF;
    return;
  }
  if (mode_screen == 2)
  { //shows a VRAM bank as is
    const uint8_t* b = bank_act[(dc >> 18) & 3] + y * 512;
    for (int x = 0; x < 256; x++)
      out[x] = a_rgb((uint16_t)(b[x * 2] | b[x * 2 + 1] << 8));
    return;
  }
  if (mode_screen == 3)
  {
    for (int x = 0; x < 256; x++)
      out[x] = 0;
    return;
  }
  int mode_bg = dc & 7;
  int active[4];
  for (int n = 0; n < 4; n++)
  {
    active[n] = (dc >> (8 + n)) & 1;
    if (!active[n])
      continue;
    int type = 0; //0 text, 1 affine, 2 extended
    if (n == 0 && m->is_a && (dc & 8))
    { //background 0 = the image of the 3D engine
      const uint16_t* c3 = c3_act + y * 256;
      const uint8_t* a3 = a3_act + y * 256;
      int hofs = r16(m, 0x10) & 0x1FF;
      if (hofs & 0x100)
        hofs -= 0x200;
      hofs_3d = hofs;
      for (int x = 0; x < 256; x++)
      {
        int sx = x + hofs;
        uint8_t a = (sx >= 0 && sx < 256) ? a3[sx] : 0;
        alpha_3d[x] = a;
        lin_bg[0][x] = a ? (c3[sx] & 0x7FFF) | PX_SET : 0;
      }
      continue;
    }
    if (n == 2)
      type = (mode_bg == 2 || mode_bg == 4) ? 1 : mode_bg == 5 ? 2 : 0;
    if (n == 3)
      type = (mode_bg == 1 || mode_bg == 2) ? 1 : mode_bg >= 3 ? 2 : 0;
    if (mode_bg == 6 && n != 0)
    {
      active[n] = 0;
      continue;
    }
    if (type == 0)
      background_text(m, n, y);
    else
      background_affine(m, n, y, type == 2);
  }
  sprites(m, y);
  //windows: which layers are seen at each pixel (bits 0-3 backgrounds, 4 sprites, 5 effects)
  uint8_t vis[256];
  int has_ventanas = (dc >> 13) & 7;
  for (int x = 0; x < 256; x++)
    vis[x] = 0x3F;
  if (has_ventanas)
  {
    uint16_t winin = r16(m, 0x48), winout = r16(m, 0x4A);
    for (int x = 0; x < 256; x++)
    {
      uint8_t v = (uint8_t)(winout & 0x3F);
      if (((dc >> 15) & 1) && window_obj[x])
        v = (uint8_t)((winout >> 8) & 0x3F);
      for (int w = 1; w >= 0; w--)
      {
        if (!((dc >> (13 + w)) & 1))
          continue;
        int x1 = m->r[0x41 + 2 * w], x2 = m->r[0x40 + 2 * w];
        int y1 = m->r[0x45 + 2 * w], y2 = m->r[0x44 + 2 * w];
        int inside_x = x1 <= x2 ? (x >= x1 && x < x2) : (x >= x1 || x < x2);
        int inside_y = y1 <= y2 ? (y >= y1 && y < y2) : (y >= y1 || y < y2);
        if (inside_x && inside_y)
        {
          v = (uint8_t)((winin >> (8 * w)) & 0x3F);
        }
      }
      vis[x] = v;
    }
    //window 0 has preference: the last of the loop is applied (w = 0)
  }
  uint16_t bldcnt = r16(m, 0x50), bldalpha = r16(m, 0x52);
  int effect = (bldcnt >> 6) & 3;
  int eva = bldalpha & 31, evb = (bldalpha >> 8) & 31, evy = r16(m, 0x54) & 31;
  if (eva > 16)
    eva = 16;
  if (evb > 16)
    evb = 16;
  uint16_t background = m->pal_bg[0] & 0x7FFF;
  uint16_t mb = r16(m, 0x6C);
  for (int x = 0; x < 256; x++)
  {
    //the two top layers: (color, layer id) with id 0-3 backgrounds, 4 sprites, 5 backdrop
    uint16_t c1 = background, c2 = background;
    int id1 = 5, id2 = 5, p1 = 5, p2 = 5;
    for (int pr = 3; pr >= 0; pr--)
    {
      for (int n = 3; n >= 0; n--)
      {
        if (!active[n] || !(vis[x] & (1 << n)) || !(lin_bg[n][x] & PX_SET))
          continue;
        if ((r16(m, 0x08 + 2 * n) & 3) != pr)
          continue;
        c2 = c1;
        id2 = id1;
        p2 = p1;
        c1 = lin_bg[n][x] & 0x7FFF;
        id1 = n;
        p1 = pr;
      }
      if ((lin_obj[x] & PX_SET) && prio_obj[x] == pr && (vis[x] & 0x10))
      {
        c2 = c1;
        id2 = id1;
        p2 = p1;
        c1 = lin_obj[x] & 0x7FFF;
        id1 = 4;
        p1 = pr;
      }
    }
    (void)p1;
    (void)p2;
    uint16_t c = c1;
    int effects = (vis[x] >> 5) & 1;
    int is_3d = id1 == 0 && m->is_a && (dc & 8);
    if (is_3d && alpha_3d[x] < 31 && ((bldcnt >> (8 + id2)) & 1))
    { //3D semitransparente
      int a = alpha_3d[x] + 1;
      c = mix(c1, c2, a / 2, 16 - a / 2);
    }
    else if (id1 == 4 && semi_obj[x] && ((bldcnt >> (8 + id2)) & 1))
    {
      //bitmap sprites blend with their own alpha (alpha + 1)/16, not with BLDALPHA
      //(the copy of the capture shown with sprites came out dark on alternate frames)
      if (semi_obj[x] >= 2)
        c = mix(c1, c2, semi_obj[x] - 1, 17 - semi_obj[x]);
      else
        c = mix(c1, c2, eva, evb);
    }
    else if (effects && ((bldcnt >> id1) & 1))
    {
      if (effect == 1 && ((bldcnt >> (8 + id2)) & 1))
        c = mix(c1, c2, eva, evb);
      else if (effect == 2)
        c = brightness(c1, 1, evy);
      else if (effect == 3)
        c = brightness(c1, 2, evy);
    }
    if (m->is_a)
    {
      lin_a15[x] = c; //for the capture (goes before the master brightness)
      mark3d[x] = is_3d && c == c1; //the 3D is seen without mixes
      brightness_a = mb;
    }
    int bm = (mb >> 14) & 3;
    if (bm == 1 || bm == 2)
      c = brightness(c, bm, mb & 31);
    out[x] = a_rgb(c);
  }
}

//Screen capture (DISPCAPCNT): copies a line of the image of engine A, of the 3D alone or of the
//VRAM (or a mix) to a bank A-D. The game uses it to have 3D on both screens (track presentation,
//ceremony): it paints one screen each frame and the other shows the copy of the previous one.
static void capture_line(const uint8_t* io9, uint32_t cap, int y)
{
  static const int width[4] = {128, 256, 256, 256}, height[4] = {128, 64, 128, 192};
  int sz = (cap >> 20) & 3, w = width[sz];
  if (y >= height[sz])
    return;
  uint8_t* dst = bank[(cap >> 16) & 3];
  uint32_t woff = ((cap >> 18) & 3) * 0x8000;
  uint32_t dispcnt = (uint32_t)(io9[0] | io9[1] << 8 | io9[2] << 16 | io9[3] << 24);
  const uint8_t* vram = bank[(dispcnt >> 18) & 3];
  uint32_t roff = ((cap >> 26) & 3) * 0x8000;
  int source = (cap >> 29) & 3, only3d = (cap >> 24) & 1, from_fifo = (cap >> 25) & 1;
  int eva = cap & 31, evb = (cap >> 8) & 31;
  if (eva > 16)
    eva = 16;
  if (evb > 16)
    evb = 16;
  const uint16_t* c3 = gx_color() + y * 256;
  const uint8_t* a3 = gx_alpha() + y * 256;
  for (int x = 0; x < w; x++)
  {
    uint16_t ca = only3d ? (uint16_t)(c3[x] & 0x7FFF) : lin_a15[x];
    int aa = only3d ? a3[x] != 0 : 1;
    uint16_t cb = 0;
    int ab = 0;
    if (!from_fifo)
    {
      uint32_t o = (roff + (uint32_t)(y * w + x) * 2) & 0x1FFFF;
      cb = (uint16_t)(vram[o] | vram[o + 1] << 8);
      ab = cb >> 15;
      cb &= 0x7FFF;
    }
    uint16_t col;
    int alpha;
    if (source == 0)
    {
      col = ca;
      alpha = aa;
    }
    else if (source == 1)
    {
      col = cb;
      alpha = ab;
    }
    else
    {
      int r = ((ca & 31) * aa * eva + (cb & 31) * ab * evb) / 16;
      int g = (((ca >> 5) & 31) * aa * eva + ((cb >> 5) & 31) * ab * evb) / 16;
      int b = (((ca >> 10) & 31) * aa * eva + ((cb >> 10) & 31) * ab * evb) / 16;
      col = (uint16_t)((r > 31 ? 31 : r) | (g > 31 ? 31 : g) << 5 | (b > 31 ? 31 : b) << 10);
      alpha = (aa && eva) || (ab && evb);
    }
    uint32_t o = (woff + (uint32_t)(y * w + x) * 2) & 0x1FFFF;
    uint16_t v = (uint16_t)(col | (alpha ? 0x8000 : 0));
    dst[o] = (uint8_t)v;
    dst[o + 1] = (uint8_t)(v >> 8);
  }
}

//---- reescalado -----------------------------------------------------------------
int video_scale(void)
{
  static int e;
  if (!e)
  {
    const char* v = getenv("DRIFT_SCALE");
    e = v ? atoi(v) : 1;
    if (e < 1)
      e = 1;
    if (e > 4)
      e = 4;
  }
  return e;
}

static uint32_t hd_up[1024 * 768], hd_down[1024 * 768];
//of the last build: at which points of each line of engine A the 3D is seen as is, with what shift
//and brightness, and whether engine A goes on top (for the in-between images: only those points
//change)
static uint16_t brightness_f[192];
static int a_up;
const uint32_t* video_hd(int bottom)
{
  return bottom ? hd_down : hd_up;
}

//one line of the DS at big size: what is 3D is taken from the 3D painted at more resolution. The
//rest (menus, scoreboard, map) is repeated as is, without blurring
static void line_hd(uint32_t* dst, const uint32_t* low, int y, int is_a)
{
  int e = video_scale(), an = 256 * e;
  const uint16_t* c3 = c3hd_act;
  const uint8_t* a3 = a3hd_act;
  int bm = (brightness_a >> 14) & 3, f = brightness_a & 31;
  for (int sy = 0; sy < e; sy++)
  {
    uint32_t* d = dst + (size_t)(y * e + sy) * an;
    int row3d = (y * e + sy) * an;
    for (int x = 0; x < 256; x++)
    {
      if (is_a && mark3d[x])
      {
        for (int sx = 0; sx < e; sx++)
        {
          int i = row3d + (x + hofs_3d) * e + sx; //mark3d is only where x + hofs falls inside
          if (!a3[i])
          {
            d[x * e + sx] = low[x];
            continue;
          }
          uint16_t c = (uint16_t)(c3[i] & 0x7FFF);
          if (bm == 1 || bm == 2)
            c = brightness(c, bm, f);
          d[x * e + sx] = a_rgb(c);
        }
      }
      else
      {
        for (int sx = 0; sx < e; sx++)
          d[x * e + sx] = low[x];
      }
    }
  }
}

//Page tables of the two engines (the real ones or those of the snapshot)
typedef struct
{
  uint8_t *bg_a[32], *obj_a[16], *bg_b[8], *obj_b[8], *ext_bg_a[4], *ext_obj_a, *ext_bg_b[4],
      *ext_obj_b;
} Tables;

static void assemble(const uint8_t* io9, const uint8_t* pal, const uint8_t* oam, Tables* t,
                     uint32_t* up, uint32_t* bottom);

void video_draw(const uint8_t* io9, uint32_t* up, uint32_t* bottom)
{
  static Tables t;
  memcpy(t.bg_a, bg_a, sizeof bg_a);
  memcpy(t.obj_a, obj_a, sizeof obj_a);
  memcpy(t.bg_b, bg_b, sizeof bg_b);
  memcpy(t.obj_b, obj_b, sizeof obj_b);
  memcpy(t.ext_bg_a, ext_bg_a, sizeof ext_bg_a);
  memcpy(t.ext_bg_b, ext_bg_b, sizeof ext_bg_b);
  t.ext_obj_a = ext_obj_a;
  t.ext_obj_b = ext_obj_b;
  bank_act = bank;
  c3_act = gx_color();
  a3_act = gx_alpha();
  c3hd_act = gx_hd_color();
  a3hd_act = gx_hd_alpha();
  assemble(io9, g_palette, g_oam, &t, up, bottom);
}

//---- building on another thread ----
static struct
{
  uint8_t io[0x2000], pal[0x800], oam[0x800];
  uint8_t mA[0x20000], mB[0x20000], mC[0x20000], mD[0x20000], mE[0x10000];
  uint8_t mF[0x4000], mG[0x4000], mH[0x8000], mI[0x4000];
  uint16_t c3[256 * 192], c3hd[1024 * 768];
  uint8_t a3[256 * 192], a3hd[1024 * 768];
  Tables t;
  int ind; //set of 3D outputs of this snapshot (for its in-between images)
  int64_t ts; //when it was made (time of the frame)
} snapshot;
static uint8_t* const bank_snapshot[9] = {snapshot.mA, snapshot.mB, snapshot.mC,
                                          snapshot.mD, snapshot.mE, snapshot.mF,
                                          snapshot.mG, snapshot.mH, snapshot.mI};
static HANDLE ev_snapshot;
static volatile LONG assembling;

static uint8_t* a_snapshot(uint8_t* p)
{ //a pointer to the real VRAM -> the same place in the snapshot
  if (!p)
    return NULL;
  for (int b = 0; b < 9; b++)
    if (p >= bank[b] && p < bank[b] + size_bank[b])
      return bank_snapshot[b] + (p - bank[b]);
  return NULL;
}

static DWORD WINAPI assemble_thread(LPVOID arg)
{
  (void)arg;
  static uint32_t up[256 * 192], bottom[256 * 192];
  for (;;)
  {
    WaitForSingleObject(ev_snapshot, INFINITE);
    bank_act = bank_snapshot;
    c3_act = snapshot.c3;
    a3_act = snapshot.a3;
    c3hd_act = snapshot.c3hd;
    a3hd_act = snapshot.a3hd;
    assemble(snapshot.io, snapshot.pal, snapshot.oam, &snapshot.t, up, bottom);
    window_show_en(up, bottom, snapshot.ts);
    InterlockedExchange(&assembling, 0);
  }
}

void video_wait_assembly(void)
{
  while (assembling)
    SwitchToThread();
}

void video_draw_separate(const uint8_t* io9)
{
  if (!ev_snapshot)
  {
    ev_snapshot = CreateEventW(NULL, FALSE, FALSE, NULL);
    HANDLE h = CreateThread(NULL, 0, assemble_thread, NULL, 0, NULL);
    SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL); //the game first
    CloseHandle(h);
  }
  if (assembling)
    return; //still on the previous one: this one is skipped (the game does not wait)
  memcpy(snapshot.io, io9, sizeof snapshot.io);
  memcpy(snapshot.pal, g_palette, sizeof snapshot.pal);
  memcpy(snapshot.oam, g_oam, sizeof snapshot.oam);
  for (int b = 0; b < 9; b++)
    memcpy(bank_snapshot[b], bank[b], size_bank[b]);
  for (int i = 0; i < 32; i++)
    snapshot.t.bg_a[i] = a_snapshot(bg_a[i]);
  for (int i = 0; i < 16; i++)
    snapshot.t.obj_a[i] = a_snapshot(obj_a[i]);
  for (int i = 0; i < 8; i++)
  {
    snapshot.t.bg_b[i] = a_snapshot(bg_b[i]);
    snapshot.t.obj_b[i] = a_snapshot(obj_b[i]);
  }
  for (int i = 0; i < 4; i++)
  {
    snapshot.t.ext_bg_a[i] = a_snapshot(ext_bg_a[i]);
    snapshot.t.ext_bg_b[i] = a_snapshot(ext_bg_b[i]);
  }
  snapshot.t.ext_obj_a = a_snapshot(ext_obj_a);
  snapshot.t.ext_obj_b = a_snapshot(ext_obj_b);
  //read which 3D scene is current ONCE: a scene that takes 1 to 3 ms (menus) can finish while the
  //copies below run, and the old code (gx_indice() on every line) then mixed the colour of one
  //scene with the alpha or the big copy of the next one, which showed up as crawling edges on the
  //kart in menus
  const int ind = gx_indice();
  snapshot.ind = ind;
  snapshot.ts = window_clock();
  memcpy(snapshot.c3, gx_color_of(ind), sizeof snapshot.c3);
  memcpy(snapshot.a3, gx_alpha_of(ind), sizeof snapshot.a3);
  int e = video_scale();
  if (e > 1)
  {
    memcpy(snapshot.c3hd, gx_hd_color_of(ind), (size_t)256 * e * 192 * e * 2);
    memcpy(snapshot.a3hd, gx_hd_alpha_of(ind), (size_t)256 * e * 192 * e);
  }
  InterlockedExchange(&assembling, 1);
  SetEvent(ev_snapshot);
}

static void assemble(const uint8_t* io9, const uint8_t* pal, const uint8_t* oam, Tables* t,
                     uint32_t* up, uint32_t* bottom)
{
  Motor a = {io9,
             (const uint16_t*)pal,
             (const uint16_t*)(pal + 0x200),
             oam,
             t->bg_a,
             t->obj_a,
             0x7FFFF,
             0x3FFFF,
             t->ext_bg_a,
             t->ext_obj_a,
             1};
  Motor b = {io9 + 0x1000,
             (const uint16_t*)(pal + 0x400),
             (const uint16_t*)(pal + 0x600),
             oam + 0x400,
             t->bg_b,
             t->obj_b,
             0x1FFFF,
             0x1FFFF,
             t->ext_bg_b,
             t->ext_obj_b,
             0};
  uint16_t powcnt = (uint16_t)(io9[0x304] | io9[0x305] << 8);
  uint32_t* out_a = (powcnt >> 15) & 1 ? up : bottom;
  uint32_t* out_b = (powcnt >> 15) & 1 ? bottom : up;
  reset_affine(&a);
  reset_affine(&b);
  uint32_t cap = (uint32_t)(io9[0x64] | io9[0x65] << 8 | io9[0x66] << 16 | io9[0x67] << 24);
  a_up = out_a == up;
  for (int y = 0; y < 192; y++)
  {
    memset(lin_a15, 0, sizeof lin_a15);
    if (powcnt & 2)
      draw_line(&a, y, out_a + y * 256);
    else
      memset(out_a + y * 256, 0, 256 * 4);
    brightness_f[y] = brightness_a;
    if (cap >> 31)
      capture_line(io9, cap, y);
    if (powcnt & 0x200)
      draw_line(&b, y, out_b + y * 256);
    else
      memset(out_b + y * 256, 0, 256 * 4);
    if (video_scale() > 1)
    {
      //with the capture on, without big 3D: the other screen shows the copy (at DS size) and each
      //screen would tremble between sharp and not sharp on alternate frames
      line_hd(out_a == up ? hd_up : hd_down, out_a + y * 256, y, !(cap >> 31));
      line_hd(out_b == up ? hd_up : hd_down, out_b + y * 256, y, 0);
    }
  }
}
