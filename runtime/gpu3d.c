// 3D engine of the DS: geometry (commands, matrices, lights, clipping) and rasterizer.
// The game sends commands through the FIFO (0x04000400, packed 4 by 4) or
// through direct ports (0x04000440 + 4 * (command - 0x10)). Vertices are
// transformed on arrival and polygons are kept in a list. When the
// game asks for SWAP_BUFFERS, on the next VBlank the whole list is painted into
// an image of 256x192 that the 2D engine A uses as background 0.
// Floating point is used (the DS uses fixed point): small differences of
// rounding, but much simpler.
#include "gpu3d.h"
#include "glctx.h"
#include "glload.h"
#include "gpu3d_types.h"
#include "gpuraster.h"
#include "log.h"
#include "scenedump.h"
#include "texdecode.h"
#include <math.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern uint8_t *g_tex[4], *g_texpal[6];
// copy of the texture and palette memory maps, taken when a scene is handed to the drawing thread.
// video.c clears and refills g_tex and g_texpal every time the game remaps a VRAM bank, and the
// drawing thread reading them in that gap got NULL, which came out as black pixels coming and going
// on models in menus
static TexMem snap;
static GpuRaster* gpu; // the GPU drawing, made and used by the drawing thread only
static int npp; // polygons drawn in the last scene
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

//---- polygon list (types in gpu3d_types.h) ----
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

static void start_threads(void);

void gx_init(void)
{
  identity(proj);
  identity(pos);
  identity(vec);
  identity(tex);
  // the graphics card check happens at the launch: a PC without OpenGL 4.5 gets its message now,
  // not at the first 3D scene
  start_threads();
}

// Drawing is done on the graphics card (gpuraster.c, OpenGL 4.5):
//- The geometry above builds the polygon list; the drawing thread hands it to gpuraster_draw, which
// returns the colour and alpha pictures (the CPU rasterizer that was here is in git history, in
// commit 73a07ea).
//- Everything happens in a separate thread while the game prepares the next
// scene. If the game finishes another scene before the previous one
// ends, that one is skipped (better to lose a frame than slow the game).
//- The thread owns an OpenGL context on a hidden window. If the PC cannot give one, the game
// says so in a message box and a log line, and closes (there is no CPU fallback).
#include "compat.h"

typedef struct
{ // what the painting thread needs (copied at the start of the scene)
  uint8_t io[0x400];
  int wbuf, vp_x1, vp_y1, vp_x2, vp_y2, lst;
} Job;

static Job job;

// Rescale: the 3D is painted at esc times the DS resolution (DRIFT_SCALE, 1-4). output_* is at DS
// size (for the 2D, the capture and the replays). output_hd_* is at big size (for the window).
#define MAXP (1024 * 768)
static int esc = 1, AN = 256, AT = 192;
static uint16_t color_buf[MAXP]; // RGB555
static uint8_t alpha_buf[MAXP]; // 0..31 (0 = nada)
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

// Scene dump (SCENE_DUMP=n in settings.ini, or DRIFT_SCENE_DUMP): saves n scenes, one about every
// second, to scenes/. Each file has the registers, polygons, textures and palettes of the scene and
// the picture this drawing made from them (scenedump.h). It is a developer tool for the GPU work:
// the files hold game data and stay out of git. Writing slows the drawing thread, so scenes may be
// skipped while it runs.
#define DUMP_EVERY 60
#define DUMP_EVERY_CAPTURE 10
static int dump_left, dump_count;
static char dump_stamp[24];

static void dump_init(void)
{
  const char* v = getenv("DRIFT_SCENE_DUMP");
  dump_left = v ? atoi(v) : 0;
  if (dump_left < 0)
    dump_left = 0;
  if (dump_left > 500)
    dump_left = 500;
  if (!dump_left)
    return;
  time_t now = time(NULL);
  struct tm* lt = localtime(&now);
  if (lt)
    strftime(dump_stamp, sizeof dump_stamp, "%Y%m%d-%H%M%S", lt);
  else
    snprintf(dump_stamp, sizeof dump_stamp, "scene");
  CreateDirectoryA("scenes", NULL);
  log_msg("3D",
          "scene dump on: %d scenes, one every %d (every %d with the capture on), into scenes/ "
          "(game data, never commit)",
          dump_left, DUMP_EVERY, DUMP_EVERY_CAPTURE);
}

// called by the drawing thread when a scene is painted and before its buffers are reused
static void dump_scene(const Poly* lst, int n)
{
  if (dump_left <= 0)
    return;
  // while the screen capture is on (track presentation, ceremony) scenes are saved more often,
  // because that is the case the GPU drawing has to get right and it is over in a few seconds
  int capture = (job.io[0x67] & 0x80) != 0;
  int every = capture ? DUMP_EVERY_CAPTURE : DUMP_EVERY;
  if (cont_frames % every != every / 2)
    return;
  Scene s;
  memset(&s, 0, sizeof s);
  s.scale = esc;
  s.wbuf = job.wbuf;
  s.vp_x1 = job.vp_x1;
  s.vp_y1 = job.vp_y1;
  s.vp_x2 = job.vp_x2;
  s.vp_y2 = job.vp_y2;
  s.io = job.io;
  for (int q = 0; q < SCENE_NTEX; q++)
    s.tex[q] = snap.tex[q];
  for (int q = 0; q < SCENE_NPAL; q++)
    s.pal[q] = snap.pal[q];
  s.polys = lst;
  s.npolys = n;
  s.color = color_buf;
  s.alpha = alpha_buf;
  char path[64];
  snprintf(path, sizeof path, "scenes/scene-%s-%03d.dscn", dump_stamp, ++dump_count);
  int ok = scenedump_write(path, &s);
  dump_left--;
  if (!ok)
    log_msg("3D", "scene dump failed: %s", path);
  else if (!dump_left)
    log_msg("3D", "scene dump done: %d scenes in scenes/", dump_count);
}

static HANDLE ev_job, ev_free, ev_ready;
static volatile int gpu_ok; // set by the drawing thread: 1 = the graphics card is ready
static char gpu_err[300]; // why it was not, in plain words

// Makes the OpenGL context and the drawing on this thread, and tells start_threads how it went.
static int gpu_start(void)
{
  GlCtx* ctx = glctx_create(gpu_err, sizeof gpu_err);
  if (!ctx)
    return 0;
  if (!glctx_selftest(gpu_err, sizeof gpu_err))
  {
    glctx_destroy(ctx);
    return 0;
  }
  gpu = gpuraster_new(esc, gpu_err, sizeof gpu_err);
  if (!gpu)
  {
    glctx_destroy(ctx);
    return 0;
  }
  int major = 0, minor = 0;
  glctx_version(ctx, &major, &minor);
  log_msg("3D", "GPU drawing ready: OpenGL %d.%d", major, minor);
  return 1;
}

static DWORD WINAPI render_thread(LPVOID arg)
{
  (void)arg;
  gpu_ok = gpu_start();
  SetEvent(ev_ready);
  if (!gpu_ok)
    return 0;
  int draw_errors = 0;
  for (;;)
  {
    WaitForSingleObject(ev_job, INFINITE);
    LARGE_INTEGER scene_start;
    QueryPerformanceCounter(&scene_start);
    const Poly* lst = polys[job.lst];
    int n = npolis[job.lst];
    int other = (output_current + 1) % 3;
    Output* S = &outputs[other];
    GpuScene gs;
    gs.io = job.io;
    gs.wbuf = job.wbuf;
    gs.vp_x1 = job.vp_x1;
    gs.vp_y1 = job.vp_y1;
    gs.vp_x2 = job.vp_x2;
    gs.vp_y2 = job.vp_y2;
    gs.polys = lst;
    gs.npolys = n;
    gs.mem = snap;
    if (!gpuraster_draw(gpu, &gs, color_buf, alpha_buf) && draw_errors++ < 5)
      log_msg("3D", "the GPU drawing reported an error (the picture of this scene may be wrong)");
    npp = gpuraster_stats(gpu)->polygons;
    dump_scene(lst, n);
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
  ev_ready = CreateEventW(NULL, FALSE, FALSE, NULL);
  HANDLE h = CreateThread(NULL, 0, render_thread, NULL, 0, NULL);
  SetThreadPriority(h, THREAD_PRIORITY_ABOVE_NORMAL);
  CloseHandle(h);
  // wait for the drawing thread to say whether the graphics card can do it
  if (WaitForSingleObject(ev_ready, 20000) != WAIT_OBJECT_0)
  {
    snprintf(gpu_err, sizeof gpu_err, "the graphics driver did not answer");
    gpu_ok = 0;
  }
  if (!gpu_ok)
  {
    char msg[600];
    log_msg("3D", "GPU drawing unavailable: %s", gpu_err);
    snprintf(msg, sizeof msg,
             "DriftDS needs a graphics card with OpenGL 4.5 to draw the 3D.\n\n"
             "Problem: %s\n\n"
             "Updating the graphics driver may help. The game will close now.",
             gpu_err);
    MessageBoxA(NULL, msg, "DriftDS", MB_OK | MB_ICONERROR);
    ExitProcess(1);
  }
  log_msg("3D", "scale %dx (%dx%d pixels)", esc, AN, AT);
  dump_init();
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
    snap.tex[q] = g_tex[q] ? copy_tex[q] : NULL;
    if (g_tex[q])
      memcpy(copy_tex[q], g_tex[q], 0x20000);
  }
  for (int q = 0; q < 6; q++)
  {
    snap.pal[q] = g_texpal[q] ? copy_pal[q] : NULL;
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
