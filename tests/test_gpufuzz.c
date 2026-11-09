// test_gpufuzz.c: draws random scenes with the GPU and with the old CPU rasterizer and compares.
//
// Usage:
//   test_gpufuzz oracle scenes/*.dscn      the oracle must reproduce every recorded CPU picture
//   test_gpufuzz [-v] FIRST_SEED COUNT     draws COUNT random scenes, seeds FIRST_SEED and up
// Build from the project folder:
//   clang -O2 -Iruntime -Itests tests/test_gpufuzz.c tests/scenecmp.c runtime/gpuraster.c \
//     runtime/texdecode.c runtime/scenedump.c runtime/glctx.c runtime/glload.c -lopengl32 -lgdi32 \
//     -luser32 -o tests/test_gpufuzz.exe
// What it does: the recorded scenes only use some of the drawing features (no Z buffer, no decal,
// no toon, no equal-depth polygons). A random scene mixes all of them: Z or W buffer, the three
// polygon modes, all texture formats with every wrap setting, translucency, alpha test, the
// equal-depth window and the polygon id rule. The oracle is raster_ref.h, a copy of the CPU
// rasterizer. The same rule as for the recorded scenes (scenecmp.h) decides pass or fail.
// A failing seed can be run again alone to see the same scene: test_gpufuzz -v SEED 1.
#include "glctx.h"
#include "raster_ref.h"
#include "scenecmp.h"
#include "scenedump.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- random numbers (xorshift, the same on every machine) ----
static uint64_t rng_state;
static uint32_t rnd(void)
{
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 7;
  rng_state ^= rng_state << 17;
  return (uint32_t)(rng_state >> 16);
}
static int rint_(int lo, int hi) // lo..hi
{
  return lo + (int)(rnd() % (uint32_t)(hi - lo + 1));
}
static float rf(float lo, float hi)
{
  return lo + (hi - lo) * (float)(rnd() & 0xFFFFFF) / 16777215.0f;
}
static int chance(int percent)
{
  return (int)(rnd() % 100) < percent;
}

// ---- a random scene ----
typedef struct
{
  uint8_t io[0x400];
  uint8_t* tex[4];
  uint8_t* pal[6];
  Poly* polys;
  int npolys, scale, wbuf, vp[4];
  int features[8]; // for the report: what the scene used
} Fuzz;

enum
{
  F_ZBUF,
  F_DECAL,
  F_TOON,
  F_SAME,
  F_TRANS,
  F_ATEST,
  F_BLEND,
  F_HIGHLIGHT
};
static const char* feature_name[8] = {"Z buffer",    "decal",      "toon",  "equal depth",
                                      "translucent", "alpha test", "blend", "highlight"};

static void fuzz_make(Fuzz* f, uint64_t seed)
{
  rng_state = seed * 0x9E3779B97F4A7C15ull + 0x1234567ull;
  for (int i = 0; i < 8; i++)
    rnd();
  memset(f, 0, sizeof *f);
  f->scale = chance(70) ? 1 : 2;
  f->wbuf = chance(50);
  f->vp[0] = 0;
  f->vp[1] = 0;
  f->vp[2] = 255;
  f->vp[3] = 191;
  if (chance(20))
  { // a smaller viewport
    f->vp[0] = rint_(0, 60);
    f->vp[1] = rint_(0, 40);
    f->vp[2] = rint_(150, 255);
    f->vp[3] = rint_(110, 191);
  }
  uint16_t disp3d = (uint16_t)((chance(90) ? 1 : 0) | (chance(30) ? 2 : 0) | (chance(40) ? 4 : 0) |
                               (chance(80) ? 8 : 0));
  f->io[0x60] = (uint8_t)disp3d;
  f->io[0x61] = (uint8_t)(disp3d >> 8);
  f->io[0x340] = (uint8_t)rint_(0, 30);
  uint32_t clear =
      (uint32_t)rint_(0, 0x7FFF) | (uint32_t)rint_(0, 31) << 16 | (uint32_t)rint_(0, 63) << 24;
  if (chance(60))
    clear &= ~(31u << 16); // usually a transparent background
  for (int i = 0; i < 4; i++)
    f->io[0x350 + i] = (uint8_t)(clear >> (8 * i));
  uint32_t cd = chance(70) ? 0x7FFF : (uint32_t)rint_(0, 0x7FFF);
  f->io[0x354] = (uint8_t)cd;
  f->io[0x355] = (uint8_t)(cd >> 8);
  for (int i = 0; i < 32; i++)
  {
    uint16_t c = (uint16_t)rint_(0, 0x7FFF);
    f->io[0x380 + i * 2] = (uint8_t)c;
    f->io[0x381 + i * 2] = (uint8_t)(c >> 8);
  }
  for (int q = 0; q < 4; q++)
  {
    f->tex[q] = malloc(0x20000);
    for (int i = 0; i < 0x20000; i++)
      f->tex[q][i] = (uint8_t)rnd();
  }
  for (int q = 0; q < 6; q++)
  {
    f->pal[q] = malloc(0x4000);
    for (int i = 0; i < 0x4000; i++)
      f->pal[q][i] = (uint8_t)rnd();
  }
  int n = rint_(20, 140);
  f->polys = calloc((size_t)n, sizeof(Poly));
  f->npolys = n;
  int ids[4] = {rint_(0, 63), rint_(0, 63), rint_(0, 63), rint_(0, 63)};
  for (int k = 0; k < n; k++)
  {
    Poly* p = &f->polys[k];
    int nv = rint_(3, 6);
    float cx = rf(-1.1f, 1.1f), cy = rf(-1.1f, 1.1f);
    float rad = chance(20) ? rf(0.5f, 1.5f) : rf(0.04f, 0.5f);
    float w0 = rf(0.6f, 25.0f), zn = rf(-0.95f, 0.95f);
    int fmt = chance(15) ? 0 : rint_(1, 7);
    int wexp = rint_(0, 4), hexp = rint_(0, 4); // 8 to 128 texels
    uint32_t ti = (uint32_t)rint_(0, 0xFFFF) | (uint32_t)rint_(0, 15) << 16 | (uint32_t)wexp << 20 |
                  (uint32_t)hexp << 23 | (uint32_t)fmt << 26 | (uint32_t)(chance(40) ? 1 : 0) << 29;
    int alpha = chance(70) ? 31 : rint_(1, 30);
    int mode = chance(60) ? 0 : (chance(50) ? 1 : 2);
    int same = chance(12), actd = chance(50);
    uint32_t attr = (uint32_t)mode << 4 | 3u << 6 | (uint32_t)actd << 11 | (uint32_t)same << 14 |
                    (uint32_t)alpha << 16 | (uint32_t)ids[rint_(0, 3)] << 24;
    p->n = nv;
    p->attr = attr;
    p->teximage = ti;
    p->pltt = (uint32_t)rint_(0, 0x17FF);
    p->translucent = (alpha > 0 && alpha < 31) || fmt == 1 || fmt == 6;
    float ang0 = rf(0, 6.28f);
    float tw = (float)(8 << wexp), th = (float)(8 << hexp);
    for (int i = 0; i < nv; i++)
    {
      float a = ang0 + (float)i * 6.2831853f / (float)nv + rf(-0.2f, 0.2f);
      float w = w0 * rf(0.8f, 1.25f);
      Vert* v = &p->v[i];
      float nx = cx + rad * cosf(a), ny = cy + rad * sinf(a);
      v->x = nx * w;
      v->y = ny * w;
      v->z = (zn + rf(-0.05f, 0.05f)) * w;
      v->w = w;
      v->r = rf(0, 31);
      v->g = rf(0, 31);
      v->b = rf(0, 31);
      // texture coordinates follow the position, so the picture is a real mapping
      v->s = (nx - cx) / rad * tw * 1.5f + tw * 0.5f;
      v->t = (ny - cy) / rad * th * 1.5f + th * 0.5f;
    }
    if (mode == 1)
      f->features[F_DECAL] = 1;
    if (mode == 2)
      f->features[F_TOON] = 1;
    if (same)
      f->features[F_SAME] = 1;
    if (p->translucent)
      f->features[F_TRANS] = 1;
  }
  if (!f->wbuf)
    f->features[F_ZBUF] = 1;
  f->features[F_ATEST] = (disp3d >> 2) & 1;
  f->features[F_BLEND] = (disp3d >> 3) & 1;
  f->features[F_HIGHLIGHT] = (disp3d >> 1) & 1;
}

// Is the centre of pixel (px, py) within `tol` pixels of an edge of any polygon, on screen? The CPU
// rasterizer draws a pixel whose centre is up to about 0.01 pixel outside a triangle, the graphics
// card follows its own fill rule, so on such a pixel either answer is right.
static int near_polygon_edge(const Fuzz* f, int scale, int px, int py, float tol)
{
  float vw = (float)(f->vp[2] - f->vp[0] + 1), vh = (float)(f->vp[3] - f->vp[1] + 1);
  float cx = (float)px + 0.5f, cy = (float)py + 0.5f;
  for (int k = 0; k < f->npolys; k++)
  {
    const Poly* p = &f->polys[k];
    float X[MAX_VERT_POLY], Y[MAX_VERT_POLY];
    for (int i = 0; i < p->n; i++)
    {
      float w = p->v[i].w == 0 ? 1e-6f : p->v[i].w, iw = 1.0f / w;
      X[i] = ((p->v[i].x * iw + 1) * vw / 2 + (float)f->vp[0]) * (float)scale;
      Y[i] = ((1 - p->v[i].y * iw) * vh / 2 + (float)(191 - f->vp[3])) * (float)scale;
    }
    for (int i = 0; i < p->n; i++)
    {
      int j = (i + 1) % p->n;
      float ex = X[j] - X[i], ey = Y[j] - Y[i];
      float len2 = ex * ex + ey * ey;
      float t = len2 > 0 ? ((cx - X[i]) * ex + (cy - Y[i]) * ey) / len2 : 0;
      t = t < 0 ? 0 : t > 1 ? 1 : t;
      float dx = X[i] + t * ex - cx, dy = Y[i] + t * ey - cy;
      if (dx * dx + dy * dy <= tol * tol)
        return 1;
    }
  }
  return 0;
}

static void fuzz_free(Fuzz* f)
{
  for (int q = 0; q < 4; q++)
    free(f->tex[q]);
  for (int q = 0; q < 6; q++)
    free(f->pal[q]);
  free(f->polys);
}

static void to_gpu_scene(const Fuzz* f, GpuScene* gs)
{
  gs->io = f->io;
  gs->wbuf = f->wbuf;
  gs->vp_x1 = f->vp[0];
  gs->vp_y1 = f->vp[1];
  gs->vp_x2 = f->vp[2];
  gs->vp_y2 = f->vp[3];
  gs->polys = f->polys;
  gs->npolys = f->npolys;
  for (int q = 0; q < 4; q++)
    gs->mem.tex[q] = f->tex[q];
  for (int q = 0; q < 6; q++)
    gs->mem.pal[q] = f->pal[q];
}

// ---- modes ----

static int mode_oracle(int argc, char** argv)
{
  int bad = 0, n = 0;
  for (int a = 2; a < argc; a++)
  {
    Scene s;
    if (!scenedump_read(argv[a], &s))
      continue;
    int w = 256 * s.scale, h = 192 * s.scale;
    uint16_t* c = malloc((size_t)w * h * 2);
    uint8_t* al = malloc((size_t)w * h);
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
    ref_draw(&gs, s.scale, c, al);
    int diff = 0;
    for (int i = 0; i < w * h; i++)
      diff += c[i] != s.color[i] || al[i] != s.alpha[i];
    if (diff)
    {
      printf("%s: the oracle differs from the recorded picture in %d pixels\n", argv[a], diff);
      bad++;
    }
    n++;
    free(c);
    free(al);
    scenedump_free(&s);
  }
  printf("oracle: %d scenes, %d different from the recording\n", n, bad);
  return bad ? 1 : 0;
}

// probe SEED X Y: finds the first polygon (in list order) after which pixel X,Y differs
static int mode_probe(int argc, char** argv)
{
  uint64_t seed = (uint64_t)strtoull(argv[2], NULL, 10);
  int px = atoi(argv[3]), py = atoi(argv[4]);
  char err[400];
  GlCtx* ctx = glctx_create(err, sizeof err);
  if (!ctx)
    return 2;
  Fuzz f;
  fuzz_make(&f, seed);
  GpuRaster* g = gpuraster_new(f.scale, err, sizeof err);
  int w = 256 * f.scale, h = 192 * f.scale;
  uint16_t *rc = malloc((size_t)w * h * 2), *gc = malloc((size_t)w * h * 2);
  uint8_t *ra = malloc((size_t)w * h), *ga = malloc((size_t)w * h);
  GpuScene gs;
  to_gpu_scene(&f, &gs);
  int i = py * w + px;
  printf("scene: scale %d, %s buffer, disp3d %02X%02X, alpha ref %d, polygons %d\n", f.scale,
         f.wbuf ? "W" : "Z", f.io[0x61], f.io[0x60], f.io[0x340] & 31, f.npolys);
  for (int n = 1; n <= f.npolys; n++)
  {
    gs.npolys = n;
    ref_draw(&gs, f.scale, rc, ra);
    gpuraster_draw(g, &gs, gc, ga);
    int d = rc[i] != gc[i] || ra[i] != ga[i];
    if (d)
    {
      const Poly* q = &f.polys[n - 1];
      printf("first differs after polygon %d (of %d): attr %08X tex %08X pltt %X n %d translucent "
             "%d\n",
             n - 1, f.npolys, q->attr, q->teximage, q->pltt, q->n, q->translucent);
      printf("  cpu %02X %02X %02X a%d   gpu %02X %02X %02X a%d\n", rc[i] & 31, rc[i] >> 5 & 31,
             rc[i] >> 10 & 31, ra[i], gc[i] & 31, gc[i] >> 5 & 31, gc[i] >> 10 & 31, ga[i]);
      break;
    }
  }
  return 0;
}

int main(int argc, char** argv)
{
  if (argc >= 2 && strcmp(argv[1], "oracle") == 0)
    return mode_oracle(argc, argv);
  if (argc >= 5 && strcmp(argv[1], "probe") == 0)
    return mode_probe(argc, argv);
  int verbose = 0, a = 1;
  if (argc > 1 && strcmp(argv[1], "-v") == 0)
  {
    verbose = 1;
    a = 2;
  }
  if (argc < a + 2)
  {
    printf("usage: test_gpufuzz oracle scenes/*.dscn | test_gpufuzz [-v] FIRST_SEED COUNT\n");
    return 2;
  }
  uint64_t first = (uint64_t)strtoull(argv[a], NULL, 10);
  int count = atoi(argv[a + 1]);
  char err[400] = "";
  GlCtx* ctx = glctx_create(err, sizeof err);
  if (!ctx)
  {
    printf("no OpenGL context: %s\n", err);
    return 2;
  }
  GpuRaster* g[3] = {NULL, NULL, NULL};
  CmpRule rule = cmp_default_rule();
  int failed = 0, used[8] = {0};
  long long edge = 0, shifted = 0, differing = 0, total = 0, knife_total = 0;
  int missing_total = 0;
  for (int k = 0; k < count; k++)
  {
    uint64_t seed = first + (uint64_t)k;
    Fuzz f;
    fuzz_make(&f, seed);
    int sc = f.scale;
    if (!g[sc])
    {
      g[sc] = gpuraster_new(sc, err, sizeof err);
      if (!g[sc])
      {
        printf("GPU drawing did not start: %s\n", err);
        return 2;
      }
    }
    int w = 256 * sc, h = 192 * sc;
    uint16_t *rc = malloc((size_t)w * h * 2), *gc = malloc((size_t)w * h * 2);
    uint8_t *ra = malloc((size_t)w * h), *ga = malloc((size_t)w * h), *diff = malloc((size_t)w * h);
    GpuScene gs;
    to_gpu_scene(&f, &gs);
    ref_draw(&gs, sc, rc, ra);
    // the same scene with texture coordinates, depth and colours nudged by 0.001 percent: a pixel
    // that changes between the two oracle pictures is on a precision knife edge (a texel boundary
    // or a depth tie), so the GPU may legitimately land on the other side
    uint8_t* unstable = calloc((size_t)w * h, 1);
    for (int sign = -1; sign <= 1; sign += 2) // nudged one way, then the other
    {
      Poly* np = malloc((size_t)f.npolys * sizeof(Poly));
      memcpy(np, f.polys, (size_t)f.npolys * sizeof(Poly));
      for (int k2 = 0; k2 < f.npolys; k2++)
        for (int i = 0; i < np[k2].n; i++)
        {
          Vert* v = &np[k2].v[i];
          // the graphics card snaps screen positions to a fine grid (1/256 pixel or so): a jitter
          // of about 1/40 pixel covers that
          float jx = (float)(((k2 * 7 + i * 13) % 5) - 2) * 0.5f;
          float jy = (float)(((k2 * 11 + i * 5) % 5) - 2) * 0.5f;
          float up = 1.0f + (float)sign * 1e-4f;
          v->x += (float)sign * v->w * jx * 2e-4f / (float)sc;
          v->y += (float)sign * v->w * jy * 2e-4f / (float)sc;
          v->s *= up;
          v->t *= up;
          v->z *= up;
          v->r *= up;
          v->g *= up;
          v->b *= up;
        }
      GpuScene gs2 = gs;
      gs2.polys = np;
      uint16_t* rc2 = malloc((size_t)w * h * 2);
      uint8_t* ra2 = malloc((size_t)w * h);
      ref_draw(&gs2, sc, rc2, ra2);
      for (int i = 0; i < w * h; i++)
        unstable[i] |= (uint8_t)((ra[i] == 0) != (ra2[i] == 0) || (ra[i] && rc[i] != rc2[i]) ||
                                 abs((int)ra[i] - (int)ra2[i]) > 1);
      free(rc2);
      free(ra2);
      free(np);
    }
    if (!gpuraster_draw(g[sc], &gs, gc, ga))
    {
      printf("seed %llu: the GPU drawing reported an error\n", (unsigned long long)seed);
      failed++;
    }
    else
    {
      CmpResult r = cmp_pictures(rc, ra, gc, ga, w, h, &rule, diff);
      edge += r.edge;
      shifted += r.shifted;
      differing += r.differing;
      total += r.total;
      for (int i = 0; i < 8; i++)
        used[i] += f.features[i];
      // a pixel that is off and not explained by a knife edge is a real mistake
      int wrong = 0, knife = 0;
      for (int i = 0; i < w * h; i++)
        if (diff[i] == 3 || diff[i] == 4)
        {
          if (unstable[i] || near_polygon_edge(&f, sc, i % w, i / w, 0.05f))
          {
            knife++;
            diff[i] = 5; // shown as excused
          }
          else
            wrong++;
        }
      knife_total += knife;
      missing_total += gpuraster_stats(g[sc])->textures_missing;
      int ok =
          wrong == 0 && (double)(r.differing - knife) * 100.0 / (double)r.total <= rule.max_percent;
      if (!ok || verbose)
      {
        printf(
            "seed %llu  scale %d  %s  polys %d  edge %d  shifted %d  differing %d (%.3f%%)  worst "
            "%d  alpha wrong %d  %s\n",
            (unsigned long long)seed, sc, f.wbuf ? "W" : "Z", f.npolys, r.edge, r.shifted,
            r.differing, r.percent, r.worst, r.alpha_wrong, ok ? "ok" : "FAIL");
        for (int i = 0, shown = 0; verbose && i < w * h && shown < 8; i++)
          if (diff[i] == 3 || diff[i] == 4)
          {
            printf("    at %d,%d  cpu %02X %02X %02X a%2d   gpu %02X %02X %02X a%2d%s\n", i % w,
                   i / w, rc[i] & 31, rc[i] >> 5 & 31, rc[i] >> 10 & 31, ra[i], gc[i] & 31,
                   gc[i] >> 5 & 31, gc[i] >> 10 & 31, ga[i],
                   diff[i] == 4 ? "  (transparency)" : "");
            float d_tol[5] = {0.1f, 0.25f, 0.5f, 1.0f, 2.0f};
            for (int q = 0; q < 5; q++)
              if (near_polygon_edge(&f, sc, i % w, i / w, d_tol[q]))
              {
                printf("      the pixel centre is within %.2f pixel of a polygon edge\n", d_tol[q]);
                break;
              }
            shown++;
          }
      }
      if (!ok)
        failed++;
    }
    free(unstable);
    free(rc);
    free(gc);
    free(ra);
    free(ga);
    free(diff);
    fuzz_free(&f);
  }
  printf("%d random scenes, %d failed\n", count, failed);
  printf("pixels: %lld compared, %lld edge, %lld shifted, %lld differing (%lld of those on a knife "
         "edge)\n",
         total, edge, shifted, differing, knife_total);
  int resets = 0;
  for (int i = 1; i < 3; i++)
    resets += g[i] ? gpuraster_stats(g[i])->atlas_resets : 0;
  printf("polygons that lost their texture: %d, texture atlas emptied %d times\n", missing_total,
         resets);
  printf("scenes that used:");
  for (int i = 0; i < 8; i++)
    printf("  %s %d", feature_name[i], used[i]);
  printf("\n");
  for (int i = 1; i < 3; i++)
    gpuraster_free(g[i]);
  glctx_destroy(ctx);
  return failed ? 1 : 0;
}
