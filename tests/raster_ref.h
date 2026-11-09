// raster_ref.h: the CPU rasterizer of gpu3d.c as it was when the GPU work started, kept as a test
// oracle (the same idea as texel_ref.h).
//
// Where it fits: tests/test_gpufuzz.c draws random scenes with this code and with gpuraster.c and
// compares the pictures. The body below is a verbatim copy of VP .. paint_band in runtime/gpu3d.c
// (commit 73a07ea and the graphics branch before the GPU drawing went live), with three changes:
// the texture cache is left out (every texel goes through texel()), the band threads are replaced
// by one call that paints the whole picture, and the globals it needs are defined here.
// Why: the recorded scenes only use some features (no Z buffer, no decal, no toon, no equal-depth
// polygons). Random scenes reach all of them, and need an oracle that can run any scene.
// Do not edit the copied body: when the real rasterizer changes on purpose, copy it again.
#pragma once
#include "gpu3d_types.h"
#include "gpuraster.h"
#include "texdecode.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
  uint32_t ti, pltt;
  int w, h;
  uint32_t* px;
} TexC;
static volatile uint32_t epoch = 1;
static TexMem snap;

static inline uint16_t texel(uint32_t ti, uint32_t pltt, int s, int t, int* alpha)
{
  return texmem_texel(&snap, ti, pltt, s, t, alpha);
}

// ---- copied from runtime/gpu3d.c ----
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
    q->tc = NULL; // the oracle decodes every texel with texel()
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

// ---- end of the copy ----

// Draws a scene with the copied code. color gets w*h RGB555 words, alpha w*h bytes.
static void ref_draw(const GpuScene* s, int scale, uint16_t* color, uint8_t* alpha)
{
  esc = scale;
  AN = 256 * scale;
  AT = 192 * scale;
  memset(&job, 0, sizeof job);
  memcpy(job.io, s->io, 0x400);
  job.wbuf = s->wbuf;
  job.vp_x1 = s->vp_x1;
  job.vp_y1 = s->vp_y1;
  job.vp_x2 = s->vp_x2;
  job.vp_y2 = s->vp_y2;
  snap = s->mem;
  prepare(&job, s->polys, s->npolys);
  build_tris();
  paint_band(0, AT - 1);
  memcpy(color, color_buf, (size_t)AN * AT * 2);
  memcpy(alpha, alpha_buf, (size_t)AN * AT);
}
