// 3D engine of the DS: geometry (commands, matrices, lights, clipping) and rasterizer.
// The game sends commands through the FIFO (0x04000400, packed 4 by 4) or
// through direct ports (0x04000440 + 4 * (command - 0x10)). Vertices are
// transformed on arrival and polygons are kept in a list. When the
// game asks for SWAP_BUFFERS, on the next VBlank the whole list is painted into
// an image of 256x192 that the 2D engine A uses as background 0.
// Floating point is used (the DS uses fixed point): small differences of
// rounding, but much simpler.
#include "gpu3d.h"
#include "log.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern uint8_t *g_tex[4], *g_texpal[6];
// copy of the texture and palette memory maps, taken when a scene is handed to the drawing thread.
// video.c clears and refills g_tex and g_texpal every time the game remaps a VRAM bank, and the
// drawing thread reading them in that gap got NULL, which came out as black pixels coming and going
// on models in menus
static uint8_t *snap_tex[4], *snap_pal[6];
// and a private copy of the CONTENT of the mapped texture and palette memory (about 600 KB, a few
// tenths of a ms): the game keeps rewriting textures and palettes (the emblem, animated models)
// while the drawing thread reads them, which gave torn or black pixels. The real DS draws a scene
// from the memory as it is at the swap
static uint8_t copy_tex[4][0x20000], copy_pal[6][0x4000];

typedef float Mat[16]; // row by row. Row vector: v' = v * M

//---- state of the geometry engine ----
static Mat proj, pos, vec, tex;
static Mat stack_proj[1], stack_pos[32], stack_vec[32], stack_tex[1];
static int sp_proj, sp_pos, sp_tex, mode_mtx;
static int error_stack;

static uint32_t d_pl_min = 0xFFFF, d_pl_max; // PLTT_BASE received (debug)
static uint32_t attr_pending, attr; // POLYGON_ATTR (applied at BEGIN_VTXS)
static uint32_t teximage, pltt_base;
static int type_prim;
static float vx, vy, vz; // last vertex (for VTX_XY, VTX_DIFF...)
static float col_r = 31, col_g = 31, col_b = 31;
static float ts, tt; // current texture coordinates (in texels)
static float ts0, tt0; // those given with TEXCOORD (for modes 2 and 3)
static uint32_t dif_amb, spe_emi;
static float light_dir[4][3], light_col[4][3];
static uint8_t brightness_table[128];
static int vp_x1, vp_y1 = 0, vp_x2 = 255, vp_y2 = 191;

//---- polygon list ----
typedef struct
{
  float x, y, z, w; // clip coordinates
  float r, g, b; // color 0..31
  float s, t; // texels
} Vert;

#define MAX_VERT_POLY 10
typedef struct
{
  Vert v[MAX_VERT_POLY];
  int n; // vertices after clipping (0 if outside the view)
  uint32_t attr, teximage, pltt;
  int translucent;
} Poly;

#define MAX_POLYS 4096
static Poly polys[2][MAX_POLYS];
static int npolis[2], list; // list = the one being filled (with those that fall outside the view)
static int nvisible[2]; // the ones seen (what the console counts)
static int swap_pending, swap_param;

static Vert strip[4]; // vertices waiting to form a polygon
static int ntira, nvert_prim, odd;

// ---- matrices ----------------------------------------------------------------
static void identity(float* m)
{
  memset(m, 0, sizeof(Mat));
  m[0] = m[5] = m[10] = m[15] = 1;
}

static void mult(float* dst, const float* a, const float* b)
{ // dst = a * b
  Mat r;
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++)
      r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] +
                     a[i * 4 + 3] * b[12 + j];
  memcpy(dst, r, sizeof(Mat));
}

static void clip(float* m)
{
  mult(m, pos, proj);
}

// Applies a matrix (cur = m * cur) according to the mode
static void apply(const float* m, int also_vec)
{
  switch (mode_mtx)
  {
  case 0:
    mult(proj, m, proj);
    break;
  case 1:
    mult(pos, m, pos);
    break;
  case 2:
    mult(pos, m, pos);
    if (also_vec)
      mult(vec, m, vec);
    break;
  case 3:
    mult(tex, m, tex);
    break;
  }
}

static void load(const float* m)
{
  switch (mode_mtx)
  {
  case 0:
    memcpy(proj, m, sizeof(Mat));
    break;
  case 1:
    memcpy(pos, m, sizeof(Mat));
    break;
  case 2:
    memcpy(pos, m, sizeof(Mat));
    memcpy(vec, m, sizeof(Mat));
    break;
  case 3:
    memcpy(tex, m, sizeof(Mat));
    break;
  }
}

static float f12(uint32_t v)
{
  return (float)(int32_t)v / 4096.0f;
}
static float s16_12(uint32_t v)
{
  return (float)(int16_t)(v & 0xFFFF) / 4096.0f;
}
static float s10(uint32_t v, int bit, float div)
{
  int32_t x = (int32_t)((v >> bit) & 0x3FF);
  if (x & 0x200)
    x -= 0x400;
  return (float)x / div;
}

// ---- luces -------------------------------------------------------------------
static void color15(uint32_t c, float* r, float* g, float* b)
{
  *r = (float)(c & 31);
  *g = (float)((c >> 5) & 31);
  *b = (float)((c >> 10) & 31);
}

static void normal(uint32_t p)
{
  float n[3] = {s10(p, 0, 512), s10(p, 10, 512), s10(p, 20, 512)};
  // texture coordinates from the normal (mode 2)
  if (((teximage >> 30) & 3) == 2)
  {
    ts = (n[0] * tex[0] + n[1] * tex[4] + n[2] * tex[8]) / 16 +
         ts0; // the result is in 1/16 of a texel
    tt = (n[0] * tex[1] + n[1] * tex[5] + n[2] * tex[9]) / 16 + tt0;
  }
  float tn[3];
  for (int j = 0; j < 3; j++)
    tn[j] = n[0] * vec[j] + n[1] * vec[4 + j] + n[2] * vec[8 + j];
  float dr, dg, db, ar, ag, ab, sr, sg, sb, er, eg, eb;
  color15(dif_amb, &dr, &dg, &db);
  color15(dif_amb >> 16, &ar, &ag, &ab);
  color15(spe_emi, &sr, &sg, &sb);
  color15(spe_emi >> 16, &er, &eg, &eb);
  float r = er, g = eg, b = eb;
  for (int l = 0; l < 4; l++)
  {
    if (!((attr >> l) & 1))
      continue;
    float* L = light_dir[l];
    float dif = -(L[0] * tn[0] + L[1] * tn[1] + L[2] * tn[2]);
    if (dif < 0)
      dif = 0;
    float h[3] = {(L[0]) / 2, (L[1]) / 2, (L[2] - 1) / 2};
    float spec = -(h[0] * tn[0] + h[1] * tn[1] + h[2] * tn[2]);
    if (spec < 0)
      spec = 0;
    spec = spec * spec;
    if (spec > 1)
      spec = 1;
    if ((spe_emi >> 15) & 1)
      spec = brightness_table[(int)(spec * 127)] / 255.0f;
    float* C = light_col[l];
    r += C[0] * (dr * dif + sr * spec + ar) / 31;
    g += C[1] * (dg * dif + sg * spec + ag) / 31;
    b += C[2] * (db * dif + sb * spec + ab) / 31;
  }
  col_r = r > 31 ? 31 : r;
  col_g = g > 31 ? 31 : g;
  col_b = b > 31 ? 31 : b;
}

//---- clipping against the view pyramid ----
static Vert interp(const Vert* a, const Vert* b, float t)
{
  Vert v;
  v.x = a->x + (b->x - a->x) * t;
  v.y = a->y + (b->y - a->y) * t;
  v.z = a->z + (b->z - a->z) * t;
  v.w = a->w + (b->w - a->w) * t;
  v.r = a->r + (b->r - a->r) * t;
  v.g = a->g + (b->g - a->g) * t;
  v.b = a->b + (b->b - a->b) * t;
  v.s = a->s + (b->s - a->s) * t;
  v.t = a->t + (b->t - a->t) * t;
  return v;
}

static float distance(const Vert* v, int plane)
{
  switch (plane)
  {
  case 0:
    return v->w + v->x;
  case 1:
    return v->w - v->x;
  case 2:
    return v->w + v->y;
  case 3:
    return v->w - v->y;
  case 4:
    return v->w + v->z;
  default:
    return v->w - v->z;
  }
}

static int clip_poly(Vert* v, int n)
{
  Vert tmp[MAX_VERT_POLY + 6];
  Vert ent[MAX_VERT_POLY + 6];
  memcpy(ent, v, sizeof(Vert) * n);
  for (int p = 0; p < 6; p++)
  {
    int m = 0;
    for (int i = 0; i < n; i++)
    {
      Vert *a = &ent[i], *b = &ent[(i + 1) % n];
      float da = distance(a, p), db = distance(b, p);
      if (da >= 0)
        tmp[m++] = *a;
      if ((da >= 0) != (db >= 0))
        tmp[m++] = interp(a, b, da / (da - db));
      if (m >= MAX_VERT_POLY + 5)
        break;
    }
    n = m;
    memcpy(ent, tmp, sizeof(Vert) * n);
    if (n < 3)
      return 0;
  }
  if (n > MAX_VERT_POLY)
    n = MAX_VERT_POLY;
  memcpy(v, ent, sizeof(Vert) * n);
  return n;
}

static void emit(Vert* v, int n)
{
  if (npolis[list] >= MAX_POLYS)
    return;
  Poly* p = &polys[list][npolis[list]];
  // also those outside the view (with n = 0): so the list of a scene and the one of the next
  // have the same shape and can be paired for the in-between images
  memcpy(p->v, v, sizeof(Vert) * n);
  n = clip_poly(p->v, n);
  if (n < 3)
    n = 0;
  else
    nvisible[list]++;
  p->n = n;
  p->attr = attr;
  p->teximage = teximage;
  p->pltt = pltt_base;
  int alpha = (attr >> 16) & 31;
  int fmt = (teximage >> 26) & 7;
  p->translucent = (alpha > 0 && alpha < 31) || fmt == 1 || fmt == 6;
  npolis[list]++;
}

static void vertex(float x, float y, float z)
{
  vx = x;
  vy = y;
  vz = z;
  Mat c;
  clip(c);
  Vert v;
  v.x = x * c[0] + y * c[4] + z * c[8] + c[12];
  v.y = x * c[1] + y * c[5] + z * c[9] + c[13];
  v.z = x * c[2] + y * c[6] + z * c[10] + c[14];
  v.w = x * c[3] + y * c[7] + z * c[11] + c[15];
  v.r = col_r;
  v.g = col_g;
  v.b = col_b;
  if (((teximage >> 30) & 3) == 3)
  { // texture coordinates from the vertex
    v.s = (x * tex[0] + y * tex[4] + z * tex[8]) / 16 + ts0;
    v.t = (x * tex[1] + y * tex[5] + z * tex[9]) / 16 + tt0;
  }
  else
  {
    v.s = ts;
    v.t = tt;
  }
  strip[ntira++] = v;
  switch (type_prim)
  {
  case 0: // triangles
    if (ntira == 3)
    {
      emit(strip, 3);
      ntira = 0;
    }
    break;
  case 1: // quads
    if (ntira == 4)
    {
      emit(strip, 4);
      ntira = 0;
    }
    break;
  case 2: // triangle strip
    if (ntira == 3)
    {
      Vert t[3] = {strip[0], strip[1], strip[2]};
      if (odd)
      {
        t[0] = strip[1];
        t[1] = strip[0];
      }
      emit(t, 3);
      odd ^= 1;
      strip[0] = strip[1];
      strip[1] = strip[2];
      ntira = 2;
    }
    break;
  case 3: // quad strip
    if (ntira == 4)
    {
      Vert q[4] = {strip[0], strip[1], strip[3], strip[2]};
      emit(q, 4);
      strip[0] = strip[2];
      strip[1] = strip[3];
      ntira = 2;
    }
    break;
  }
  nvert_prim++;
}

//---- commands ----
static const int n_param[256] = {
    [0x10] = 1,  [0x11] = 0,  [0x12] = 1,  [0x13] = 1, [0x14] = 1, [0x15] = 0, [0x16] = 16,
    [0x17] = 12, [0x18] = 16, [0x19] = 12, [0x1A] = 9, [0x1B] = 3, [0x1C] = 3, [0x20] = 1,
    [0x21] = 1,  [0x22] = 1,  [0x23] = 2,  [0x24] = 1, [0x25] = 1, [0x26] = 1, [0x27] = 1,
    [0x28] = 1,  [0x29] = 1,  [0x2A] = 1,  [0x2B] = 1, [0x30] = 1, [0x31] = 1, [0x32] = 1,
    [0x33] = 1,  [0x34] = 32, [0x40] = 1,  [0x41] = 0, [0x50] = 1, [0x60] = 1, [0x70] = 3,
    [0x71] = 2,  [0x72] = 1,
};
static const uint8_t command_valid[256] = {
    [0x10] = 1, [0x11] = 1, [0x12] = 1, [0x13] = 1, [0x14] = 1, [0x15] = 1, [0x16] = 1, [0x17] = 1,
    [0x18] = 1, [0x19] = 1, [0x1A] = 1, [0x1B] = 1, [0x1C] = 1, [0x20] = 1, [0x21] = 1, [0x22] = 1,
    [0x23] = 1, [0x24] = 1, [0x25] = 1, [0x26] = 1, [0x27] = 1, [0x28] = 1, [0x29] = 1, [0x2A] = 1,
    [0x2B] = 1, [0x30] = 1, [0x31] = 1, [0x32] = 1, [0x33] = 1, [0x34] = 1, [0x40] = 1, [0x41] = 1,
    [0x50] = 1, [0x60] = 1, [0x70] = 1, [0x71] = 1, [0x72] = 1,
};

static float pos_res[4], vec_res[3];
static int box_inside = 1;

static void run(int cmd, const uint32_t* p)
{
  Mat m;
  switch (cmd)
  {
  case 0x10:
    mode_mtx = p[0] & 3;
    break;
  case 0x11: // PUSH
    if (mode_mtx == 0)
    {
      memcpy(stack_proj[0], proj, sizeof(Mat));
      sp_proj = 1;
    }
    else if (mode_mtx == 3)
    {
      memcpy(stack_tex[0], tex, sizeof(Mat));
      sp_tex = 1;
    }
    else
    {
      if (sp_pos >= 31)
      {
        error_stack = 1;
        break;
      }
      memcpy(stack_pos[sp_pos], pos, sizeof(Mat));
      memcpy(stack_vec[sp_pos], vec, sizeof(Mat));
      sp_pos++;
    }
    break;
  case 0x12:
  { // POP
    if (mode_mtx == 0)
    {
      sp_proj = 0;
      memcpy(proj, stack_proj[0], sizeof(Mat));
    }
    else if (mode_mtx == 3)
    {
      sp_tex = 0;
      memcpy(tex, stack_tex[0], sizeof(Mat));
    }
    else
    {
      int n = (int)(p[0] & 63);
      if (n & 32)
        n -= 64;
      sp_pos -= n;
      if (sp_pos < 0)
      {
        sp_pos = 0;
        error_stack = 1;
      }
      if (sp_pos > 31)
      {
        sp_pos = 31;
        error_stack = 1;
      }
      memcpy(pos, stack_pos[sp_pos], sizeof(Mat));
      memcpy(vec, stack_vec[sp_pos], sizeof(Mat));
    }
    break;
  }
  case 0x13: // STORE
    if (mode_mtx == 0)
      memcpy(stack_proj[0], proj, sizeof(Mat));
    else if (mode_mtx == 3)
      memcpy(stack_tex[0], tex, sizeof(Mat));
    else
    {
      int n = p[0] & 31;
      if (n == 31)
        error_stack = 1;
      memcpy(stack_pos[n], pos, sizeof(Mat));
      memcpy(stack_vec[n], vec, sizeof(Mat));
    }
    break;
  case 0x14: // RESTORE
    if (mode_mtx == 0)
      memcpy(proj, stack_proj[0], sizeof(Mat));
    else if (mode_mtx == 3)
      memcpy(tex, stack_tex[0], sizeof(Mat));
    else
    {
      int n = p[0] & 31;
      memcpy(pos, stack_pos[n], sizeof(Mat));
      memcpy(vec, stack_vec[n], sizeof(Mat));
    }
    break;
  case 0x15:
    identity(m);
    load(m);
    break;
  case 0x16:
    for (int i = 0; i < 16; i++)
      m[i] = f12(p[i]);
    load(m);
    break;
  case 0x17:
    identity(m);
    for (int i = 0; i < 4; i++)
      for (int j = 0; j < 3; j++)
        m[i * 4 + j] = f12(p[i * 3 + j]);
    load(m);
    break;
  case 0x18:
    for (int i = 0; i < 16; i++)
      m[i] = f12(p[i]);
    apply(m, 1);
    break;
  case 0x19:
    identity(m);
    for (int i = 0; i < 4; i++)
      for (int j = 0; j < 3; j++)
        m[i * 4 + j] = f12(p[i * 3 + j]);
    apply(m, 1);
    break;
  case 0x1A:
    identity(m);
    for (int i = 0; i < 3; i++)
      for (int j = 0; j < 3; j++)
        m[i * 4 + j] = f12(p[i * 3 + j]);
    apply(m, 1);
    break;
  case 0x1B: // SCALE (does not affect the vector matrix)
    identity(m);
    m[0] = f12(p[0]);
    m[5] = f12(p[1]);
    m[10] = f12(p[2]);
    apply(m, 0);
    break;
  case 0x1C:
    identity(m);
    m[12] = f12(p[0]);
    m[13] = f12(p[1]);
    m[14] = f12(p[2]);
    apply(m, 1);
    break;
  case 0x20:
    color15(p[0], &col_r, &col_g, &col_b);
    break;
  case 0x21:
    normal(p[0]);
    break;
  case 0x22:
  {
    float s = (float)(int16_t)(p[0] & 0xFFFF) / 16.0f, t = (float)(int16_t)(p[0] >> 16) / 16.0f;
    ts0 = s;
    tt0 = t;
    if (((teximage >> 30) & 3) == 1)
    { // transformed by the texture matrix
      ts = s * tex[0] + t * tex[4] + (tex[8] + tex[12]) / 16;
      tt = s * tex[1] + t * tex[5] + (tex[9] + tex[13]) / 16;
    }
    else
    {
      ts = s;
      tt = t;
    }
    break;
  }
  case 0x23:
    vertex(s16_12(p[0]), s16_12(p[0] >> 16), s16_12(p[1]));
    break;
  case 0x24:
    vertex(s10(p[0], 0, 64), s10(p[0], 10, 64), s10(p[0], 20, 64));
    break;
  case 0x25:
    vertex(s16_12(p[0]), s16_12(p[0] >> 16), vz);
    break;
  case 0x26:
    vertex(s16_12(p[0]), vy, s16_12(p[0] >> 16));
    break;
  case 0x27:
    vertex(vx, s16_12(p[0]), s16_12(p[0] >> 16));
    break;
  case 0x28: // VTX_DIFF: 10 bit signed values added as they are in units of 4.12
    vertex(vx + s10(p[0], 0, 4096), vy + s10(p[0], 10, 4096), vz + s10(p[0], 20, 4096));
    break;
  case 0x29:
    attr_pending = p[0];
    break;
  case 0x2A:
    teximage = p[0];
    break;
  case 0x2B:
    pltt_base = p[0] & 0x1FFF;
    if (pltt_base < d_pl_min)
      d_pl_min = pltt_base;
    if (pltt_base > d_pl_max)
      d_pl_max = pltt_base;
    break;
  case 0x30:
    dif_amb = p[0];
    if (p[0] & 0x8000)
      color15(p[0], &col_r, &col_g, &col_b);
    break;
  case 0x31:
    spe_emi = p[0];
    break;
  case 0x32:
  {
    int l = (p[0] >> 30) & 3;
    float L[3] = {s10(p[0], 0, 512), s10(p[0], 10, 512), s10(p[0], 20, 512)};
    for (int j = 0; j < 3; j++)
      light_dir[l][j] = L[0] * vec[j] + L[1] * vec[4 + j] + L[2] * vec[8 + j];
    break;
  }
  case 0x33:
  {
    int l = (p[0] >> 30) & 3;
    color15(p[0], &light_col[l][0], &light_col[l][1], &light_col[l][2]);
    break;
  }
  case 0x34:
    for (int i = 0; i < 32; i++)
      memcpy(brightness_table + i * 4, &p[i], 4);
    break;
  case 0x40:
    type_prim = p[0] & 3;
    attr = attr_pending;
    ntira = 0;
    nvert_prim = 0;
    odd = 0;
    break;
  case 0x41:
    break;
  case 0x50:
    swap_pending = 1;
    swap_param = p[0] & 3;
    break;
  case 0x60:
    vp_x1 = p[0] & 0xFF;
    vp_y1 = (p[0] >> 8) & 0xFF;
    vp_x2 = (p[0] >> 16) & 0xFF;
    vp_y2 = (p[0] >> 24) & 0xFF;
    break;
  case 0x70:
    box_inside = 1;
    break; // BOX_TEST: always "seen"
  case 0x71:
  {
    Mat c;
    clip(c);
    float x = s16_12(p[0]), y = s16_12(p[0] >> 16), z = s16_12(p[1]);
    vx = x;
    vy = y;
    vz = z;
    for (int j = 0; j < 4; j++)
      pos_res[j] = x * c[j] + y * c[4 + j] + z * c[8 + j] + c[12 + j];
    break;
  }
  case 0x72:
  {
    float n[3] = {s10(p[0], 0, 512), s10(p[0], 10, 512), s10(p[0], 20, 512)};
    for (int j = 0; j < 3; j++)
      vec_res[j] = n[0] * vec[j] + n[1] * vec[4 + j] + n[2] * vec[8 + j];
    break;
  }
  }
}

// FIFO: packed commands (4 commands per word, then their parameters)
static uint32_t fifo_cmds; // commands pending of the packed word
static int fifo_ncmds;
static uint32_t params[32];
static int nparams, cmd_current = -1;

static void next_command(void);

static void start_command(int cmd)
{
  cmd_current = cmd;
  nparams = 0;
  if (!command_valid[cmd] || n_param[cmd] == 0)
  {
    if (command_valid[cmd])
      run(cmd, params);
    cmd_current = -1;
    next_command();
  }
}

static void next_command(void)
{
  while (cmd_current < 0 && fifo_ncmds > 0)
  {
    int c = fifo_cmds & 0xFF;
    fifo_cmds >>= 8;
    fifo_ncmds--;
    if (c == 0)
      continue; // NOP
    cmd_current = c;
    nparams = 0;
    if (!command_valid[c] || n_param[c] == 0)
    {
      if (command_valid[c])
        run(c, params);
      cmd_current = -1;
    }
  }
}

static void fifo_write(uint32_t v)
{
  if (cmd_current < 0)
  {
    if (v == 0)
      return;
    fifo_cmds = v;
    fifo_ncmds = 4;
    // removes the trailing NOPs
    while (fifo_ncmds > 0 && ((fifo_cmds >> (8 * (fifo_ncmds - 1))) & 0xFF) == 0)
      fifo_ncmds--;
    next_command();
    return;
  }
  params[nparams++] = v;
  if (nparams >= n_param[cmd_current])
  {
    run(cmd_current, params);
    cmd_current = -1;
    next_command();
  }
}

static uint32_t port_params[32];
static int port_n, port_cmd = -1;

void gx_write(uint32_t a, uint32_t v)
{
  if (a < 0x04000440)
  {
    fifo_write(v);
    return;
  }
  if (a >= 0x04000600)
    return;
  int cmd = (int)((a - 0x04000400) >> 2);
  if (!command_valid[cmd])
    return;
  if (n_param[cmd] == 0)
  {
    run(cmd, port_params);
    return;
  }
  if (port_cmd != cmd)
  {
    port_cmd = cmd;
    port_n = 0;
  }
  port_params[port_n++] = v;
  if (port_n >= n_param[cmd])
  {
    run(cmd, port_params);
    port_n = 0;
    port_cmd = -1;
  }
}

uint32_t gx_read(uint32_t a)
{
  Mat c;
  switch (a & ~3u)
  {
  case 0x04000600: // GXSTAT: FIFO empty, nothing busy
    return (uint32_t)box_inside << 1 | (uint32_t)(sp_pos & 31) << 8 | (uint32_t)sp_proj << 13 |
           (uint32_t)error_stack << 15 | 0x06000000;
  case 0x04000604:
    return (uint32_t)nvisible[list] | (uint32_t)(nvisible[list] * 3) << 16;
  case 0x04000620:
  case 0x04000624:
  case 0x04000628:
  case 0x0400062C:
    return (uint32_t)(int32_t)(pos_res[(a - 0x04000620) / 4] * 4096);
  case 0x04000630:
  case 0x04000634:
    return (uint32_t)((int32_t)(vec_res[(a - 0x04000630) / 2] * 4096) & 0xFFFF) |
           (uint32_t)((int32_t)(vec_res[(a - 0x04000630) / 2 + 1] * 4096) << 16);
  }
  if (a >= 0x04000640 && a < 0x04000680)
  {
    clip(c);
    return (uint32_t)(int32_t)(c[(a - 0x04000640) / 4] * 4096);
  }
  if (a >= 0x04000680 && a < 0x040006A4)
  {
    int i = (int)(a - 0x04000680) / 4;
    return (uint32_t)(int32_t)(vec[(i / 3) * 4 + i % 3] * 4096);
  }
  return 0;
}

void gx_init(void)
{
  identity(proj);
  identity(pos);
  identity(vec);
  identity(tex);
}

// Fast and in parallel:
//- Each triangle is prepared once (equations of its edges and of its
// attributes) and walked adding increments, row by row, only inside.
//- The screen is split into NBANDAS bands of rows, each in its thread:
// each band paints all the polygons in order, so the result is
// the same as painting one by one.
//- Everything happens in a separate thread while the game prepares the next
// scene. If the game finishes another scene before the previous one
// ends, that one is skipped (better to lose a frame than slow the game).
#include "compat.h"

static inline uint8_t tex8(uint32_t o)
{
  uint8_t* p = snap_tex[(o >> 17) & 3];
  if (!p)
    return 0;
  return p[o & 0x1FFFF];
}
static inline uint16_t pal16(uint32_t o)
{
  o %= 0x18000; // 96 KB (6 slots of 16 KB). Careful: 0x17FFF does NOT work as a mask (it clears bit
                // 15)
  uint8_t* p = snap_pal[o >> 14];
  if (!p)
    return 0;
  return (uint16_t)(p[o & 0x3FFF] | p[(o & 0x3FFF) + 1] << 8);
}

static inline uint16_t media15(uint16_t c0, uint16_t c1, int p0, int p1, int div)
{
  return (uint16_t)((((c0 & 31) * p0 + (c1 & 31) * p1) / div) |
                    ((((c0 >> 5) & 31) * p0 + ((c1 >> 5) & 31) * p1) / div) << 5 |
                    ((((c0 >> 10) & 31) * p0 + ((c1 >> 10) & 31) * p1) / div) << 10);
}

// Color of a texel (RGB555) and its alpha 0..31
static uint16_t texel(uint32_t ti, uint32_t pltt, int s, int t, int* alpha)
{
  int w = 8 << ((ti >> 20) & 7), h = 8 << ((ti >> 23) & 7);
  if ((ti >> 16) & 1)
  { // repetir S
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
    uint8_t b = tex8(base + (uint32_t)(t * w + s));
    int a = b >> 5;
    *alpha = a * 4 + a / 2;
    return pal16(pb + (b & 31) * 2);
  }
  case 2: // 4 colores
    i = (tex8(base + (uint32_t)((t * w + s) / 4)) >> ((s & 3) * 2)) & 3;
    if (trans0 && i == 0)
      *alpha = 0;
    return pal16(pltt * 8 + i * 2);
  case 3: // 16 colores
    i = (tex8(base + (uint32_t)((t * w + s) / 2)) >> ((s & 1) * 4)) & 15;
    if (trans0 && i == 0)
      *alpha = 0;
    return pal16(pb + i * 2);
  case 4: // 256 colores
    i = tex8(base + (uint32_t)(t * w + s));
    if (trans0 && i == 0)
      *alpha = 0;
    return pal16(pb + i * 2);
  case 5:
  { // compressed in 4x4 blocks
    uint32_t raw_data = base + (uint32_t)((t / 4) * (w / 4) + s / 4) * 4;
    int idx = (tex8(raw_data + (uint32_t)(t & 3)) >> ((s & 3) * 2)) & 3;
    uint32_t slot =
        (raw_data & 0x7FFFF) >= 0x40000 ? 0x10000 : 0; // data in slot 2: indices in slot 1 + 64 KB
    uint32_t info_dir = 0x20000 + slot + ((raw_data & 0x1FFFF) >> 1);
    uint16_t info = (uint16_t)(tex8(info_dir) | tex8(info_dir + 1) << 8);
    uint32_t pdir = pb + (uint32_t)(info & 0x3FFF) * 4;
    int mode = info >> 14;
    switch (idx)
    {
    case 0:
      return pal16(pdir);
    case 1:
      return pal16(pdir + 2);
    case 2:
      if (mode == 0 || mode == 2)
        return pal16(pdir + 4);
      if (mode == 1)
        return media15(pal16(pdir), pal16(pdir + 2), 1, 1, 2);
      return media15(pal16(pdir), pal16(pdir + 2), 5, 3, 8);
    default:
      if (mode == 2)
        return pal16(pdir + 6);
      if (mode == 3)
        return media15(pal16(pdir), pal16(pdir + 2), 3, 5, 8);
      *alpha = 0;
      return 0;
    }
  }
  case 6:
  { // A5I3
    uint8_t b = tex8(base + (uint32_t)(t * w + s));
    *alpha = b >> 3;
    return pal16(pb + (b & 7) * 2);
  }
  case 7:
  { // color directo
    uint32_t o = base + (uint32_t)(t * w + s) * 2;
    uint16_t c = (uint16_t)(tex8(o) | tex8(o + 1) << 8);
    if (!(c & 0x8000))
      *alpha = 0;
    return c & 0x7FFF;
  }
  }
  return 0x7FFF;
}

// texture cache: every texel is decoded by texel() at most once per scene, then read back by every
// pixel, band and in-between image of that scene. one 32 bit word per texel: epoch(12) alpha(5)
// color(15), so a band thread writes it in one store and a stale entry (old epoch) just gets
// decoded again. nothing is kept between scenes, so a texture the game rewrites is picked up
// exactly as before
typedef struct
{
  uint32_t ti, pltt;
  int w, h;
  uint32_t* px;
} TexC;
#define MAX_TEXC 512
#define MAX_TEXC_BYTES (96u * 1024 * 1024)
static TexC texc[MAX_TEXC];
static int ntexc;
static size_t texc_bytes;
static volatile uint32_t epoch = 1; // 1..4095, 0 is the empty (never decoded) value

static void texc_empty(void)
{
  for (int i = 0; i < ntexc; i++)
    free(texc[i].px);
  ntexc = 0;
  texc_bytes = 0;
}

// start of a real scene (sub-frames share it): new epoch, and a flush when the counter wraps or
// memory is high
static void texc_scene(void)
{
  epoch = epoch % 4095 + 1;
  if (epoch == 1 || texc_bytes > MAX_TEXC_BYTES / 2)
    texc_empty();
}

// cache slot of a texture, NULL when there is no room (the pixel then uses texel() directly)
static TexC* texc_search(uint32_t ti, uint32_t pltt)
{
  static int last;
  if (last < ntexc && texc[last].ti == ti && texc[last].pltt == pltt)
    return &texc[last];
  for (int i = 0; i < ntexc; i++)
    if (texc[i].ti == ti && texc[i].pltt == pltt)
    {
      last = i;
      return &texc[i];
    }
  int w = 8 << ((ti >> 20) & 7), h = 8 << ((ti >> 23) & 7);
  size_t bytes = (size_t)w * h * 4;
  if (ntexc >= MAX_TEXC || texc_bytes + bytes > MAX_TEXC_BYTES)
    return NULL;
  uint32_t* px = calloc((size_t)w * h, 4);
  if (!px)
    return NULL;
  texc[ntexc] = (TexC){ti, pltt, w, h, px};
  texc_bytes += bytes;
  last = ntexc;
  return &texc[ntexc++];
}

typedef struct
{
  float x, y, z, iw, r, g, b, s, t;
} VP; // vertex on screen (attributes / w)

typedef struct
{ // polygon ready to paint
  VP v[MAX_VERT_POLY];
  int n;
  uint32_t teximage, pltt;
  uint8_t mode, alpha, id, textures, depth_same, act_depth_trans, translucent;
  TexC* tc; // texture cache slot, NULL = decode with texel() per pixel
} PP;

typedef struct
{ // what the painting thread needs (copied at the start of the scene)
  uint8_t io[0x400];
  int wbuf, vp_x1, vp_y1, vp_x2, vp_y2, lst;
} Job;

static PP pp[MAX_POLYS];
static int npp;
static Job job;

// Rescale: the 3D is painted at esc times the DS resolution (DRIFT_SCALE, 1-4). output_* is at DS
// size (for the 2D, the capture and the replays). output_hd_* is at big size (for the window).
#define MAXP (1024 * 768)
static int esc = 1, AN = 256, AT = 192;
static uint16_t color_buf[MAXP]; // RGB555
static uint8_t alpha_buf[MAXP]; // 0..31 (0 = nada)
static uint32_t depth_buf[MAXP];
static uint8_t id_buf[MAXP];
// What was painted: a ring of 3 sets. The painting thread writes the one after the current. The
// image build (another thread) uses the one it gets for a frame, without it being overwritten.
typedef struct
{
  uint16_t color[256 * 192]; // at DS size
  uint8_t alpha[256 * 192];
  uint16_t hd_color[MAXP]; // at big size (rescale)
  uint8_t hd_alpha[MAXP];
} Output;
static Output outputs[3];
static volatile int output_current, polys_last;
static volatile uint64_t cont_frames, cont_skipped;

// Turns the polygons of the list into screen coordinates and drops those not seen

static void prepare(const Job* tj, const Poly* lst, int n)
{
  uint16_t disp3d = (uint16_t)(tj->io[0x60] | tj->io[0x61] << 8);
  float vw = (float)(tj->vp_x2 - tj->vp_x1 + 1), vh = (float)(tj->vp_y2 - tj->vp_y1 + 1);
  npp = 0;
  for (int k = 0; k < n; k++)
  {
    const Poly* p = &lst[k];
    if (p->n < 3)
      continue; // outside the view
    int alpha = (p->attr >> 16) & 31, mode = (p->attr >> 4) & 3;
    if (alpha == 0 || mode == 3)
      continue; // wireframe and shadows: not painted for now
    PP* q = &pp[npp];
    float area = 0;
    for (int i = 0; i < p->n; i++)
    {
      const Vert* a = &p->v[i];
      float w = a->w == 0 ? 1e-6f : a->w, iw = 1.0f / w;
      VP* v = &q->v[i];
      v->x = ((a->x * iw + 1) * vw / 2 + (float)tj->vp_x1) * (float)esc;
      v->y = ((1 - a->y * iw) * vh / 2 + (float)(191 - tj->vp_y2)) * (float)esc;
      // DRIFT_SNAP=1: round screen positions to whole pixels of the drawing grid, as the DS does
      // (its rasterizer has no sub-pixel positions), so textures do not slide by tiny amounts while
      // a model moves
      v->z = tj->wbuf ? w : (a->z * iw + 1) / 2;
      v->iw = iw;
      v->r = a->r * iw;
      v->g = a->g * iw;
      v->b = a->b * iw;
      v->s = a->s * iw;
      v->t = a->t * iw;
    }
    for (int i = 0; i < p->n; i++)
    {
      const VP *a = &q->v[i], *b = &q->v[(i + 1) % p->n];
      area += a->x * b->y - b->x * a->y;
    }
    int front = area < 0;
    if (front && !((p->attr >> 7) & 1))
      continue;
    if (!front && !((p->attr >> 6) & 1))
      continue;
    int fmt = (p->teximage >> 26) & 7;
    q->n = p->n;
    q->teximage = p->teximage;
    q->pltt = p->pltt;
    q->mode = (uint8_t)mode;
    q->alpha = (uint8_t)alpha;
    q->id = (uint8_t)((p->attr >> 24) & 63);
    q->textures = (disp3d & 1) && fmt != 0;
    q->tc = q->textures ? texc_search(p->teximage, p->pltt) : NULL;
    q->depth_same = (p->attr >> 14) & 1;
    q->act_depth_trans = (p->attr >> 11) & 1;
    q->translucent = (uint8_t)p->translucent;
    npp++;
  }
}

// triangle setup, computed once per image (not once per band): bands only clip the row range and
// draw
typedef struct
{
  const PP* p;
  float id, k0, k0x, k0y, k1, k1x, k1y, l0x, l1x;
  float zA, zB, zC, iwA, iwB, iwC, rA, rB, rC, gA, gB, gC, bA, bB, bC, sA, sB, sC, tA, tB, tC;
  int y0, y1, xa, xb; // bounding box on screen, x already clipped to the image
} Tri;
#define MAX_TRIS (MAX_POLYS * 8)
static Tri tris[MAX_TRIS];
static int ntris, ntris_op; // tris[0..ntris_op) are opaque, the rest translucent

static void tri_prepare(const PP* p, const VP* A, const VP* B, const VP* C)
{
  if (ntris >= MAX_TRIS)
    return;
  float d = (B->x - A->x) * (C->y - A->y) - (C->x - A->x) * (B->y - A->y);
  if (fabsf(d) < 1e-6f)
    return;
  Tri* T = &tris[ntris];
  float id = 1.0f / d;
  int y0 = (int)floorf(fminf(A->y, fminf(B->y, C->y))),
      y1 = (int)ceilf(fmaxf(A->y, fmaxf(B->y, C->y)));
  int xa = (int)floorf(fminf(A->x, fminf(B->x, C->x))),
      xb = (int)ceilf(fmaxf(A->x, fmaxf(B->x, C->x)));
  if (xa < 0)
    xa = 0;
  if (xb > AN - 1)
    xb = AN - 1;
  if (y1 < 0 || y0 > AT - 1 || xa > xb)
    return; // nothing of it lands in the image
  T->p = p;
  T->id = id;
  T->y0 = y0;
  T->y1 = y1;
  T->xa = xa;
  T->xb = xb;
  // barycentric weights of A and B as linear functions: l = (k + kx*px + ky*py) / d
  T->k0 = B->x * C->y - C->x * B->y;
  T->k0x = B->y - C->y;
  T->k0y = C->x - B->x;
  T->k1 = C->x * A->y - A->x * C->y;
  T->k1x = C->y - A->y;
  T->k1y = A->x - C->x;
  T->l0x = T->k0x * id;
  T->l1x = T->k1x * id;
  // atributo = aC + l0*(aA - aC) + l1*(aB - aC)
#define DIF(c)                                                                                     \
  T->c##A = A->c - C->c;                                                                           \
  T->c##B = B->c - C->c;                                                                           \
  T->c##C = C->c
  DIF(z);
  DIF(iw);
  DIF(r);
  DIF(g);
  DIF(b);
  DIF(s);
  DIF(t);
#undef DIF
  ntris++;
}

// triangles of the image, called after prepare(): opaque polygons first, then translucent ones,
// each in list order
static void build_tris(void)
{
  ntris = 0;
  for (int pass_num = 0; pass_num < 2; pass_num++)
  {
    for (int k = 0; k < npp; k++)
    {
      const PP* q = &pp[k];
      if (q->translucent != pass_num)
        continue;
      for (int j = 1; j + 1 < q->n; j++)
        tri_prepare(q, &q->v[0], &q->v[j], &q->v[j + 1]);
    }
    if (!pass_num)
      ntris_op = ntris;
  }
}

static void triangle(const Tri* T, int yb0, int yb1, const Job* tj, int layer_trans)
{
  const PP* p = T->p;
  int y0 = T->y0, y1 = T->y1, xa = T->xa, xb = T->xb;
  if (y0 < yb0)
    y0 = yb0;
  if (y1 > yb1)
    y1 = yb1;
  if (y0 > y1)
    return;
  const float id = T->id, k0 = T->k0, k0x = T->k0x, k0y = T->k0y, k1 = T->k1, k1x = T->k1x,
              k1y = T->k1y;
  const float l0x = T->l0x, l1x = T->l1x;
  const float zA = T->zA, zB = T->zB, zC = T->zC, iwA = T->iwA, iwB = T->iwB, iwC = T->iwC;
  const float rA = T->rA, rB = T->rB, rC = T->rC, gA = T->gA, gB = T->gB, gC = T->gC;
  const float bA = T->bA, bB = T->bB, bC = T->bC, sA = T->sA, sB = T->sB, sC = T->sC;
  const float tA = T->tA, tB = T->tB, tC = T->tC;
  const uint8_t* io = tj->io;
  uint16_t disp3d = (uint16_t)(io[0x60] | io[0x61] << 8);
  int test_alpha = (disp3d >> 2) & 1, ref_alpha = io[0x340] & 31, mix_alpha = (disp3d >> 3) & 1;
  int mode = p->mode, alpha_poly = p->alpha, idp = p->id, va = alpha_poly * 2 + 1;
  const int wbuf = tj->wbuf;
  TexC* const tc = p->tc;
  const uint32_t ep = epoch;
  int tw = 0, th = 0, rep_s = 0, flip_s = 0, rep_t = 0, flip_t = 0;
  if (tc)
  { // wrap flags of the texture, once per triangle
    uint32_t ti = p->teximage;
    tw = tc->w;
    th = tc->h;
    rep_s = (ti >> 16) & 1;
    rep_t = (ti >> 17) & 1;
    flip_s = (ti >> 18) & 1;
    flip_t = (ti >> 19) & 1;
  }
  const float eps = -1e-4f;
  for (int y = y0; y <= y1; y++)
  {
    float py = (float)y + 0.5f;
    float px0 = (float)xa + 0.5f;
    float l0 = (k0 + k0x * px0 + k0y * py) * id, l1 = (k1 + k1x * px0 + k1y * py) * id;
    // stretch of the row inside the triangle: l0, l1 and l2 = 1 - l0 - l1 >= 0
    float lo = 0, hi = (float)(xb - xa);
    float base[3] = {l0, l1, 1 - l0 - l1}, step[3] = {l0x, l1x, -l0x - l1x};
    int outside = 0;
    for (int e = 0; e < 3; e++)
    {
      if (step[e] > 0)
      {
        float c = (eps - base[e]) / step[e];
        if (c > lo)
          lo = c;
      }
      else if (step[e] < 0)
      {
        float c = (eps - base[e]) / step[e];
        if (c < hi)
          hi = c;
      }
      else if (base[e] < eps)
        outside = 1;
    }
    if (outside || lo > hi)
      continue;
    int xi = xa + (int)ceilf(lo), xf = xa + (int)floorf(hi);
    if (xi < xa)
      xi = xa;
    if (xf > xb)
      xf = xb;
    l0 += l0x * (float)(xi - xa);
    l1 += l1x * (float)(xi - xa);
    for (int x = xi; x <= xf; x++, l0 += l0x, l1 += l1x)
    {
      int i = y * AN + x;
      // With a vertex stuck to the camera (huge 1/w) and the others far, at the edge (eps) this can
      // give ~0 or negative: w and the colors blow up. That is already outside the triangle: not
      // painted (the game closed when going through King Boo in his mission: with the camera
      // inside, the index of the toon table came out negative and was read outside memory)
      float iwp = iwC + l0 * iwA + l1 * iwB;
      if (!(iwp > 0))
        continue;
      // w-buffer: w is not linear on screen (1/w is), so the already corrected w is used
      // z-buffer mode: depth does not need w, so the division waits until the depth test has passed
      float w;
      uint32_t zi;
      if (wbuf)
      {
        w = 1.0f / iwp;
        zi = (uint32_t)(w * 0x1000);
      }
      else
      {
        zi = (uint32_t)((zC + l0 * zA + l1 * zB) * 0xFFFFFF);
      }
      if (p->depth_same ? (zi > depth_buf[i] + 0x200 || zi + 0x200 < depth_buf[i])
                        : zi >= depth_buf[i])
        continue;
      if (!wbuf)
        w = 1.0f / iwp;
      int vr = (int)((rC + l0 * rA + l1 * rB) * w * 2 + 0.5f);
      int vg = (int)((gC + l0 * gA + l1 * gB) * w * 2 + 0.5f);
      int vb = (int)((bC + l0 * bA + l1 * bB) * w * 2 + 0.5f);
      vr = vr < 0    ? 0
           : vr > 63 ? 63
                     : vr; // like the console: 0..63 (and the toon table only has 32)
      vg = vg < 0 ? 0 : vg > 63 ? 63 : vg;
      vb = vb < 0 ? 0 : vb > 63 ? 63 : vb;
      int cr = vr, cg = vg, cb = vb, ca = va;
      if (mode == 2)
      { // toon / highlight: table according to red
        uint32_t o = 0x380 + (uint32_t)(vr >> 1) * 2;
        uint16_t tc = (uint16_t)(io[o] | io[o + 1] << 8);
        if ((disp3d >> 1) & 1)
        {
          cr = vr + (tc & 31) * 2;
          cg = vr + ((tc >> 5) & 31) * 2;
          cb = vr + ((tc >> 10) & 31) * 2;
          if (cr > 63)
            cr = 63;
          if (cg > 63)
            cg = 63;
          if (cb > 63)
            cb = 63;
        }
        else
        {
          cr = (tc & 31) * 2 + 1;
          cg = ((tc >> 5) & 31) * 2 + 1;
          cb = ((tc >> 10) & 31) * 2 + 1;
        }
        vr = cr;
        vg = cg;
        vb = cb;
      }
      if (p->textures)
      {
        float s = (sC + l0 * sA + l1 * sB) * w, t = (tC + l0 * tA + l1 * tB) * w;
        int ta;
        uint16_t tcol;
        if (tc)
        {
          int si = (int)floorf(s), ti_ = (int)floorf(t);
          // same wrap as texel(), then one cache read
          if (rep_s)
          {
            if (flip_s)
            {
              int per = si & (2 * tw - 1);
              si = per >= tw ? 2 * tw - 1 - per : per;
            }
            else
              si &= tw - 1;
          }
          else
            si = si < 0 ? 0 : si >= tw ? tw - 1 : si;
          if (rep_t)
          {
            if (flip_t)
            {
              int per = ti_ & (2 * th - 1);
              ti_ = per >= th ? 2 * th - 1 - per : per;
            }
            else
              ti_ &= th - 1;
          }
          else
            ti_ = ti_ < 0 ? 0 : ti_ >= th ? th - 1 : ti_;
          uint32_t* slot = &tc->px[(size_t)ti_ * (size_t)tw + (size_t)si];
          uint32_t e = *slot;
          if ((e >> 20) != ep)
          {
            int a;
            uint16_t c = texel(p->teximage, p->pltt, si, ti_, &a);
            e = ep << 20 | (uint32_t)a << 15 | (c & 0x7FFF);
            *slot = e;
          }
          tcol = (uint16_t)(e & 0x7FFF);
          ta = (int)((e >> 15) & 31);
        }
        else
        {
          tcol = texel(p->teximage, p->pltt, (int)floorf(s), (int)floorf(t), &ta);
        }
        int tr = (tcol & 31) * 2 + ((tcol & 31) ? 1 : 0);
        int tg = ((tcol >> 5) & 31) * 2 + (((tcol >> 5) & 31) ? 1 : 0);
        int tb = ((tcol >> 10) & 31) * 2 + (((tcol >> 10) & 31) ? 1 : 0);
        int ta6 = ta * 2 + (ta ? 1 : 0);
        if (mode == 1)
        { // decal
          cr = (tr * ta6 + vr * (63 - ta6)) / 64;
          cg = (tg * ta6 + vg * (63 - ta6)) / 64;
          cb = (tb * ta6 + vb * (63 - ta6)) / 64;
        }
        else
        { // modulate (and toon with texture)
          cr = ((tr + 1) * (vr + 1) - 1) / 64;
          cg = ((tg + 1) * (vg + 1) - 1) / 64;
          cb = ((tb + 1) * (vb + 1) - 1) / 64;
          ca = ((ta6 + 1) * (va + 1) - 1) / 64;
        }
      }
      int a5 = ca >> 1;
      if (a5 == 0)
        continue;
      if (test_alpha && a5 <= ref_alpha)
        continue;
      uint16_t col = (uint16_t)((cr >> 1) | (cg >> 1) << 5 | (cb >> 1) << 10);
      if (a5 < 31 && mix_alpha)
      {
        if (layer_trans && id_buf[i] == idp && alpha_buf[i] < 31 && alpha_buf[i] > 0)
          continue;
        if (alpha_buf[i])
        {
          uint16_t d0 = color_buf[i];
          int dr = d0 & 31, dg = (d0 >> 5) & 31, db = (d0 >> 10) & 31;
          int nr = ((cr >> 1) * (a5 + 1) + dr * (31 - a5)) / 32;
          int ng = ((cg >> 1) * (a5 + 1) + dg * (31 - a5)) / 32;
          int nb = ((cb >> 1) * (a5 + 1) + db * (31 - a5)) / 32;
          col = (uint16_t)(nr | ng << 5 | nb << 10);
          a5 = a5 > alpha_buf[i] ? a5 : alpha_buf[i];
        }
        if (p->act_depth_trans)
          depth_buf[i] = zi;
      }
      else
      {
        depth_buf[i] = zi;
      }
      color_buf[i] = col;
      alpha_buf[i] = (uint8_t)a5;
      id_buf[i] = (uint8_t)idp;
    }
  }
}

static void paint_band(int yb0, int yb1)
{
  const Job* tj = &job;
  const uint8_t* io = tj->io;
  uint32_t clear = (uint32_t)(io[0x350] | io[0x351] << 8 | io[0x352] << 16 | io[0x353] << 24);
  uint16_t clear_depth = (uint16_t)((io[0x354] | io[0x355] << 8) & 0x7FFF);
  uint16_t cc = clear & 0x7FFF;
  uint8_t ca = (clear >> 16) & 31, cid = (clear >> 24) & 63;
  uint32_t cz = (uint32_t)clear_depth * 0x200 + 0x1FF;
  for (int i = yb0 * AN; i < (yb1 + 1) * AN; i++)
  {
    color_buf[i] = cc;
    alpha_buf[i] = ca;
    depth_buf[i] = cz;
    id_buf[i] = cid;
  }
  for (int pass_num = 0; pass_num < 2; pass_num++)
  { // first the opaque, then the translucent
    int k0 = pass_num ? ntris_op : 0, k1 = pass_num ? ntris : ntris_op;
    for (int k = k0; k < k1; k++)
    {
      const Tri* T = &tris[k];
      if (T->y1 < yb0 || T->y0 > yb1)
        continue; // outside this band
      triangle(T, yb0, yb1, tj, pass_num);
    }
  }
}

#define NBANDAS 24
static HANDLE ev_band_ir[NBANDAS], ev_band_done[NBANDAS], ev_job, ev_free;
static int nbandas = NBANDAS; // as many as cores minus one (the game's), at most NBANDAS

// balanced drawing: the image is cut into small row chunks and every thread takes the next free
// chunk from a shared counter, so a thread that got cheap rows (sky) keeps helping with the
// expensive ones (road, kart). Before, each thread had one fixed slice and the scene waited for the
// slowest slice (2 to 3 times the average in slow scenes)
static volatile LONG chunk_sig; // next chunk to draw
static int chunk_rows = 8, n_chunks = 1; // rows per chunk and chunks in the image, set by paint_all

static DWORD WINAPI band_thread(LPVOID arg)
{
  int b = (int)(intptr_t)arg;
  for (;;)
  {
    WaitForSingleObject(ev_band_ir[b], INFINITE);
    for (;;)
    {
      int i = (int)InterlockedIncrement(&chunk_sig) - 1;
      if (i >= n_chunks)
        break;
      int y0 = i * chunk_rows, y1 = y0 + chunk_rows - 1;
      if (y1 > AT - 1)
        y1 = AT - 1;
      paint_band(y0, y1);
    }
    SetEvent(ev_band_done[b]);
  }
}

static void paint_all(void)
{
  // about 6 chunks per thread, never fewer than 4 rows each
  chunk_rows = AT / (nbandas * 6);
  if (chunk_rows < 4)
    chunk_rows = 4;
  n_chunks = (AT + chunk_rows - 1) / chunk_rows;
  chunk_sig = 0;
  for (int b = 0; b < nbandas; b++)
    SetEvent(ev_band_ir[b]);
  WaitForMultipleObjects(nbandas, ev_band_done, TRUE, INFINITE);
}

static DWORD WINAPI render_thread(LPVOID arg)
{
  (void)arg;
  for (;;)
  {
    WaitForSingleObject(ev_job, INFINITE);
    LARGE_INTEGER scene_start;
    QueryPerformanceCounter(&scene_start);
    const Poly* lst = polys[job.lst];
    int n = npolis[job.lst];
    int other = (output_current + 1) % 3;
    Output* S = &outputs[other];
    texc_scene(); // texture cache: new epoch for this scene
    prepare(&job, lst, n);
    build_tris();
    paint_all();
    if (esc == 1)
    {
      memcpy(S->color, color_buf, sizeof S->color);
      memcpy(S->alpha, alpha_buf, sizeof S->alpha);
    }
    else
    {
      memcpy(S->hd_color, color_buf, (size_t)AN * AT * 2);
      memcpy(S->hd_alpha, alpha_buf, (size_t)AN * AT);
      for (int y = 0; y < 192; y++) // at DS size: the central point of each block
        for (int x = 0; x < 256; x++)
        {
          int i = (y * esc + esc / 2) * AN + x * esc + esc / 2;
          S->color[y * 256 + x] = color_buf[i];
          S->alpha[y * 256 + x] = alpha_buf[i];
        }
    }
    output_current = other;
    polys_last = npp;
    cont_frames++;
    SetEvent(ev_free);
    LARGE_INTEGER scene_end;
    QueryPerformanceCounter(&scene_end);
    log_note_scene(scene_end.QuadPart - scene_start.QuadPart, npp);
  }
}

int video_scale(void);

static void start_threads(void)
{
  esc = video_scale();
  AN = 256 * esc;
  AT = 192 * esc;
  ev_job = CreateEventW(NULL, FALSE, FALSE, NULL);
  ev_free = CreateEventW(NULL, FALSE, TRUE, NULL);
  DWORD_PTR del_process = 0, del_system = 0; // cores the game can use
  GetProcessAffinityMask(GetCurrentProcess(), &del_process, &del_system);
  // With more bands than cores the game and the music lost their turn and more scenes were skipped
  //(tested limiting the game to 2 and 4 cores).
  nbandas = -1;
  for (; del_process; del_process &= del_process - 1)
    nbandas++;
  if (nbandas < 1)
    nbandas = 1;
  if (nbandas > NBANDAS)
    nbandas = NBANDAS;
  for (int b = 0; b < nbandas; b++)
  {
    ev_band_ir[b] = CreateEventW(NULL, FALSE, FALSE, NULL);
    ev_band_done[b] = CreateEventW(NULL, FALSE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0, band_thread, (LPVOID)(intptr_t)b, 0, NULL));
  }
  HANDLE h = CreateThread(NULL, 0, render_thread, NULL, 0, NULL);
  SetThreadPriority(h, THREAD_PRIORITY_ABOVE_NORMAL);
  CloseHandle(h);
  log_msg("3D", "scale %dx (%dx%d pixels), %d drawing threads", esc, AN, AT, nbandas);
}

// With the capture on, the scene before the one just requested is shown: on the console the 3D
// image of a frame is the one the game prepared in the previous one, and so each view lands on its
// screen (otherwise, in the track presentation the top view came out below and the other way
// round). Which one (indice_fixed) is fixed instead of waiting for the new one to be painted: that
// wait slowed the ARM7 and the music in the presentation.
static volatile int fixed, indice_fixed;

void gx_vblank(const uint8_t* io9)
{
  int capture = (io9[0x67] & 0x80) != 0;
  if (!capture)
    fixed = 0;
  if (!swap_pending)
    return;
  swap_pending = 0;
  if (!ev_job)
    start_threads();
  // With screen capture on (3D on both screens: track presentation, ceremony)
  // the game alternates the top and bottom view every frame. There the scene is painted right away:
  // if it came out a frame late (the normal case, to not slow the game) the two views would be
  // mixed.
  int immediate = capture;
  if (WaitForSingleObject(ev_free, immediate ? INFINITE : 0) != WAIT_OBJECT_0)
  {
    // the previous scene is still being painted: this one is skipped and the game does not wait
    npolis[list] = nvisible[list] = 0;
    cont_skipped++;
    return;
  }
  memcpy(job.io, io9, sizeof job.io);
  job.wbuf = (swap_param >> 1) & 1;
  job.vp_x1 = vp_x1;
  job.vp_y1 = vp_y1;
  job.vp_x2 = vp_x2;
  job.vp_y2 = vp_y2;
  job.lst = list;
  list ^= 1; // the other list starts to be filled now
  npolis[list] = nvisible[list] = 0;
  if (immediate)
  { // the previous one is already painted (waited above): that is the one shown
    indice_fixed = output_current;
    fixed = 1;
  }
  for (int q = 0; q < 4; q++)
  { // maps and contents as they are at this swap (the game thread is the only writer of the maps)
    snap_tex[q] = g_tex[q] ? copy_tex[q] : NULL;
    if (g_tex[q])
      memcpy(copy_tex[q], g_tex[q], 0x20000);
  }
  for (int q = 0; q < 6; q++)
  {
    snap_pal[q] = g_texpal[q] ? copy_pal[q] : NULL;
    if (g_texpal[q])
      memcpy(copy_pal[q], g_texpal[q], 0x4000);
  }
  SetEvent(ev_job);
}

int gx_indice(void)
{
  return fixed ? indice_fixed : output_current;
}
// the same buffers for a fixed output set i (read gx_indice() once and use these, so colour, alpha
// and the big copies all come from the same scene even if a newer scene finishes while the picture
// is being copied)
const uint16_t* gx_color_of(int i)
{
  return outputs[i].color;
}
const uint8_t* gx_alpha_of(int i)
{
  return outputs[i].alpha;
}
const uint16_t* gx_hd_color_of(int i)
{
  return outputs[i].hd_color;
}
const uint8_t* gx_hd_alpha_of(int i)
{
  return outputs[i].hd_alpha;
}
const uint16_t* gx_color(void)
{
  return outputs[gx_indice()].color;
}
const uint8_t* gx_alpha(void)
{
  return outputs[gx_indice()].alpha;
}
int gx_polys(void)
{
  return polys_last;
}
const uint16_t* gx_hd_color(void)
{
  return outputs[gx_indice()].hd_color;
}
const uint8_t* gx_hd_alpha(void)
{
  return outputs[gx_indice()].hd_alpha;
}
// in-between images of output set i (from the previous scene to that one): how many and each one
uint64_t gx_frames(void)
{
  return cont_frames;
}
// scenes that were dropped because the previous one was still being painted
uint64_t gx_skipped(void)
{
  return cont_skipped;
}
