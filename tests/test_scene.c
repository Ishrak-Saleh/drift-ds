// test_scene.c: checks for runtime/scenedump.c and tests/scenecmp.c. No framework, prints one line
// per failure. Run it from an empty scratch folder, it writes a few small files there.
// Build and run from the project folder:
//   clang -Iruntime -Itests tests/test_scene.c runtime/scenedump.c tests/scenecmp.c \
//     -o tests/test_scene.exe
//   (cd scratch && ../tests/test_scene.exe)
#include "scenecmp.h"
#include "scenedump.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond)                                                                                \
  do                                                                                               \
  {                                                                                                \
    if (!(cond))                                                                                   \
    {                                                                                              \
      printf("FAIL line %d: %s\n", __LINE__, #cond);                                               \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

static long file_size(const char* path)
{
  FILE* f = fopen(path, "rb");
  if (!f)
    return -1;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fclose(f);
  return n;
}

// copies the first n bytes of a file (n < 0: the whole file) and appends `extra` zero bytes
static void copy_file(const char* from, const char* to, long n, int extra)
{
  FILE* a = fopen(from, "rb");
  FILE* b = fopen(to, "wb");
  int c;
  while (a && b && n != 0 && (c = fgetc(a)) != EOF)
  {
    fputc(c, b);
    if (n > 0)
      n--;
  }
  for (int i = 0; i < extra; i++)
    fputc(0, b);
  if (a)
    fclose(a);
  if (b)
    fclose(b);
}

static void test_roundtrip(void)
{
  static uint8_t io[0x400], tex0[SCENE_TEX_SIZE], tex2[SCENE_TEX_SIZE], pal1[SCENE_PAL_SIZE];
  static Poly polys[3];
  static uint16_t color[256 * 192];
  static uint8_t alpha[256 * 192];
  for (int i = 0; i < 0x400; i++)
    io[i] = (uint8_t)(i * 7);
  for (int i = 0; i < SCENE_TEX_SIZE; i++)
  {
    tex0[i] = (uint8_t)i;
    tex2[i] = (uint8_t)(i >> 3);
  }
  for (int i = 0; i < SCENE_PAL_SIZE; i++)
    pal1[i] = (uint8_t)(255 - i);
  for (int k = 0; k < 3; k++)
  {
    polys[k].n = 3 + k;
    polys[k].attr = 0x1F0000u + (uint32_t)k;
    polys[k].teximage = 0xAB000000u + (uint32_t)k;
    polys[k].pltt = (uint32_t)k * 5;
    polys[k].translucent = k & 1;
    for (int v = 0; v < MAX_VERT_POLY; v++)
      polys[k].v[v] = (Vert){(float)v, (float)k, 0.5f, 1.25f, 1, 2, 3, 4.5f, 6.5f};
  }
  for (int i = 0; i < 256 * 192; i++)
  {
    color[i] = (uint16_t)(i * 3) & 0x7FFF;
    alpha[i] = (uint8_t)(i % 32);
  }
  Scene s;
  memset(&s, 0, sizeof s);
  s.scale = 1;
  s.wbuf = 1;
  s.vp_x1 = 3;
  s.vp_y1 = 4;
  s.vp_x2 = 250;
  s.vp_y2 = 180;
  s.io = io;
  s.tex[0] = tex0;
  s.tex[2] = tex2;
  s.pal[1] = pal1;
  s.polys = polys;
  s.npolys = 3;
  s.color = color;
  s.alpha = alpha;
  CHECK(scenedump_write("a.dscn", &s));
  CHECK(file_size("a.tmp") < 0 && file_size("a.dscn.tmp") < 0); // no temporary file left behind

  Scene r;
  CHECK(scenedump_read("a.dscn", &r));
  CHECK(r.scale == 1 && r.wbuf == 1 && r.vp_x1 == 3 && r.vp_y1 == 4 && r.vp_x2 == 250 &&
        r.vp_y2 == 180 && r.npolys == 3);
  CHECK(r.tex[0] && !r.tex[1] && r.tex[2] && !r.tex[3]);
  CHECK(!r.pal[0] && r.pal[1] && !r.pal[2] && !r.pal[5]);
  CHECK(!memcmp(r.io, io, sizeof io));
  CHECK(r.tex[0] && !memcmp(r.tex[0], tex0, sizeof tex0));
  CHECK(r.tex[2] && !memcmp(r.tex[2], tex2, sizeof tex2));
  CHECK(r.pal[1] && !memcmp(r.pal[1], pal1, sizeof pal1));
  CHECK(!memcmp(r.polys, polys, sizeof polys));
  CHECK(!memcmp(r.color, color, sizeof color));
  CHECK(!memcmp(r.alpha, alpha, sizeof alpha));
  scenedump_free(&r);
  CHECK(r.io == NULL && r.polys == NULL);

  // writing again over an existing file works
  CHECK(scenedump_write("a.dscn", &s));

  // bad input is rejected and nothing is returned
  long n = file_size("a.dscn");
  CHECK(n > 0);
  copy_file("a.dscn", "short.dscn", n - 1, 0);
  CHECK(!scenedump_read("short.dscn", &r) && r.io == NULL);
  copy_file("a.dscn", "long.dscn", -1, 1);
  CHECK(!scenedump_read("long.dscn", &r));
  copy_file("a.dscn", "head.dscn", 20, 0);
  CHECK(!scenedump_read("head.dscn", &r));
  CHECK(!scenedump_read("missing.dscn", &r));
  CHECK(!scenedump_read(NULL, &r));
  CHECK(!scenedump_read("a.dscn", NULL));

  // damaged header fields: magic, version and Poly size are checked
  FILE* f = fopen("magic.dscn", "wb");
  copy_file("a.dscn", "magic.dscn", -1, 0);
  if (f)
    fclose(f);
  f = fopen("magic.dscn", "r+b");
  if (f)
  {
    fputc('X', f);
    fclose(f);
  }
  CHECK(!scenedump_read("magic.dscn", &r));
  copy_file("a.dscn", "ver.dscn", -1, 0);
  f = fopen("ver.dscn", "r+b");
  if (f)
  {
    fseek(f, 4, SEEK_SET);
    fputc(9, f);
    fclose(f);
  }
  CHECK(!scenedump_read("ver.dscn", &r));
  copy_file("a.dscn", "poly.dscn", -1, 0);
  f = fopen("poly.dscn", "r+b");
  if (f)
  {
    fseek(f, 8, SEEK_SET);
    fputc(1, f);
    fclose(f);
  }
  CHECK(!scenedump_read("poly.dscn", &r));

  // write refuses bad input
  Scene bad = s;
  bad.scale = 5;
  CHECK(!scenedump_write("bad.dscn", &bad));
  bad = s;
  bad.npolys = MAX_POLYS + 1;
  CHECK(!scenedump_write("bad.dscn", &bad));
  bad = s;
  bad.io = NULL;
  CHECK(!scenedump_write("bad.dscn", &bad));
  CHECK(file_size("bad.dscn") < 0);
}

#define W 64
#define H 48
static uint16_t rc[W * H], gc[W * H];
static uint8_t ra[W * H], ga[W * H], dif[W * H];

static void flat(uint16_t c, uint8_t a)
{
  for (int i = 0; i < W * H; i++)
  {
    rc[i] = gc[i] = c;
    ra[i] = ga[i] = a;
  }
}

static void test_compare(void)
{
  CmpRule rule = cmp_default_rule();
  CHECK(rule.small_step == 1 && rule.max_step == 3 && rule.max_percent == 0.1);

  flat(0x2A55, 31);
  CmpResult r = cmp_pictures(rc, ra, gc, ga, W, H, NULL, dif);
  CHECK(r.pass && r.differing == 0 && r.worst == 0 && r.total == W * H && r.percent == 0.0);

  // every pixel off by one step is rounding
  for (int i = 0; i < W * H; i++)
    gc[i] = (uint16_t)(rc[i] + 1);
  r = cmp_pictures(rc, ra, gc, ga, W, H, NULL, dif);
  CHECK(r.pass && r.differing == 0 && dif[100] == 1);

  // one pixel off by 10 steps in a flat area: the share is small but the step is too big
  flat(0x2A55, 31);
  gc[W * 20 + 30] = (uint16_t)(rc[0] + 10);
  r = cmp_pictures(rc, ra, gc, ga, W, H, NULL, dif);
  CHECK(!r.pass && r.differing == 1 && r.worst == 10 && dif[W * 20 + 30] == 3);

  // many pixels off by 2 steps: each one is small but together they pass the 0.1 percent
  flat(0x2A55, 31);
  for (int i = 0; i < 20; i++)
    gc[i * 7] = (uint16_t)(rc[0] + 2);
  r = cmp_pictures(rc, ra, gc, ga, W, H, NULL, NULL);
  CHECK(!r.pass && r.differing == 20 && r.worst == 2 && r.percent > 0.1);

  // three pixels off by 2 in 3072 are about 0.098 percent: pass
  flat(0x2A55, 31);
  for (int i = 0; i < 3; i++)
    gc[i * 11] = (uint16_t)(rc[0] + 2);
  r = cmp_pictures(rc, ra, gc, ga, W, H, NULL, NULL);
  CHECK(r.pass && r.differing == 3 && r.percent < 0.1);

  // a boundary between two colours moved by one column: those pixels are edge, not counted
  flat(0x0000, 31);
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++)
    {
      rc[y * W + x] = x < 32 ? 0x0000 : 0x7FFF;
      gc[y * W + x] = x < 33 ? 0x0000 : 0x7FFF;
    }
  r = cmp_pictures(rc, ra, gc, ga, W, H, NULL, dif);
  CHECK(r.pass && r.edge == H && r.differing == 0 && dif[5 * W + 32] == 2);

  // low contrast boundary (4 steps, not an edge): a pixel that shows its neighbour's colour is
  // shifted, counted on its own and not as differing
  flat(0x0000, 31);
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++)
      rc[y * W + x] = gc[y * W + x] = x < 32 ? 0x0000 : 0x0004;
  gc[10 * W + 31] = 0x0004;
  r = cmp_pictures(rc, ra, gc, ga, W, H, NULL, dif);
  CHECK(r.pass && r.shifted == 1 && r.differing == 0 && r.edge == 0 && dif[10 * W + 31] == 5);

  // a colour that no neighbour has is differing, not shifted
  gc[10 * W + 31] = 0x0009;
  r = cmp_pictures(rc, ra, gc, ga, W, H, NULL, dif);
  CHECK(!r.pass && r.shifted == 0 && r.differing == 1 && r.worst == 9);

  // too many shifted pixels fail as well: 0.1 percent is the limit for them too
  gc[10 * W + 31] = 0x0000;
  for (int y = 0; y < 10; y++)
    gc[y * W + 31] = 0x0004;
  r = cmp_pictures(rc, ra, gc, ga, W, H, NULL, NULL);
  CHECK(!r.pass && r.shifted == 10 && r.differing == 0 && r.shifted_percent > 0.1);

  // transparency wrong in a flat area
  flat(0x2A55, 31);
  ga[W * 10 + 10] = 0;
  r = cmp_pictures(rc, ra, gc, ga, W, H, NULL, dif);
  CHECK(!r.pass && r.alpha_wrong == 1 && dif[W * 10 + 10] == 4);

  // colour of a pixel that is transparent in both pictures does not matter
  flat(0x2A55, 0);
  for (int i = 0; i < W * H; i++)
    gc[i] = 0x7FFF;
  r = cmp_pictures(rc, ra, gc, ga, W, H, NULL, NULL);
  CHECK(r.pass && r.differing == 0);

  // a stricter rule from the caller is used
  flat(0x2A55, 31);
  gc[5] = (uint16_t)(rc[0] + 2);
  CmpRule strict = {0, 1, 0.0, 4};
  r = cmp_pictures(rc, ra, gc, ga, W, H, &strict, NULL);
  CHECK(!r.pass && r.differing == 1);

  // missing pictures never pass
  r = cmp_pictures(NULL, ra, gc, ga, W, H, NULL, NULL);
  CHECK(!r.pass);
  r = cmp_pictures(rc, ra, gc, ga, 0, H, NULL, NULL);
  CHECK(!r.pass);
}

int main(void)
{
  test_roundtrip();
  test_compare();
  printf(failures ? "%d checks failed\n" : "all checks passed\n", failures);
  return failures != 0;
}
