// test_texdecode.c: checks for runtime/texdecode.c and texdecode.h. No framework, prints one line
// per failure.
// Build and run from the project folder:
//   clang -O2 -Iruntime -Itests tests/test_texdecode.c runtime/texdecode.c \
//     -o tests/test_texdecode.exe && tests/test_texdecode.exe
// It takes about half a minute: the hash check changes every byte of texture and palette memory.
#include "texdecode.h"
#include "texel_ref.h"
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

static uint64_t rng_state = 0x2545F4914F6CDD1Dull;
static uint32_t rnd(void)
{
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 7;
  rng_state ^= rng_state << 17;
  return (uint32_t)(rng_state >> 16);
}

// memory laid out like the engine's copies: one block per slot, in a row
static uint8_t tex_mem[4][0x20000], pal_mem[6][0x4000];
static TexMem mem;

static void fill_memory(void)
{
  for (int q = 0; q < 4; q++)
    for (int i = 0; i < 0x20000; i++)
      tex_mem[q][i] = (uint8_t)rnd();
  for (int q = 0; q < 6; q++)
    for (int i = 0; i < 0x4000; i++)
      pal_mem[q][i] = (uint8_t)rnd();
  for (int q = 0; q < 4; q++)
  {
    mem.tex[q] = tex_mem[q];
    ref_tex[q] = tex_mem[q];
  }
  for (int q = 0; q < 6; q++)
  {
    mem.pal[q] = pal_mem[q];
    ref_pal[q] = pal_mem[q];
  }
}

// TEXIMAGE_PARAM: base (units of 8 bytes), wrap flags, sizes, format, color 0 transparent
static uint32_t make_ti(uint32_t base8, int wrap, int sw, int sh, int fmt, int trans0)
{
  return (base8 & 0xFFFF) | (uint32_t)(wrap & 15) << 16 | (uint32_t)(sw & 7) << 20 |
         (uint32_t)(sh & 7) << 23 | (uint32_t)(fmt & 7) << 26 | (uint32_t)(trans0 & 1) << 29;
}

// the new decoder gives the same color and alpha as the old one, for every format, wrap mode and
// position, also outside the texture
static void test_same_as_old(void)
{
  fill_memory();
  int diffs = 0;
  for (int n = 0; n < 300000; n++)
  {
    uint32_t ti = make_ti(rnd(), (int)rnd(), (int)(rnd() % 8), (int)(rnd() % 8), (int)(rnd() % 8),
                          (int)rnd());
    uint32_t pltt = rnd() & 0x1FFF;
    int s = (int)(rnd() % 4096) - 1500, t = (int)(rnd() % 4096) - 1500;
    int a1 = -1, a2 = -2;
    uint16_t c1 = ref_texel(ti, pltt, s, t, &a1);
    uint16_t c2 = texmem_texel(&mem, ti, pltt, s, t, &a2);
    if (c1 != c2 || a1 != a2)
      diffs++;
  }
  CHECK(diffs == 0);

  // unmapped slots read as zero in both
  TexMem none;
  memset(&none, 0, sizeof none);
  for (int q = 0; q < 4; q++)
    ref_tex[q] = NULL;
  for (int q = 0; q < 6; q++)
    ref_pal[q] = NULL;
  diffs = 0;
  for (int n = 0; n < 20000; n++)
  {
    uint32_t ti = make_ti(rnd(), (int)rnd(), (int)(rnd() % 8), (int)(rnd() % 8), (int)(rnd() % 8),
                          (int)rnd());
    uint32_t pltt = rnd() & 0x1FFF;
    int s = (int)(rnd() % 200), t = (int)(rnd() % 200), a1, a2;
    if (ref_texel(ti, pltt, s, t, &a1) != texmem_texel(&none, ti, pltt, s, t, &a2) || a1 != a2)
      diffs++;
  }
  CHECK(diffs == 0);
  fill_memory();
}

static void test_decode_full(void)
{
  fill_memory();
  static uint32_t out[1024 * 1024];
  int bad = 0, checked = 0;
  for (int fmt = 1; fmt <= 7; fmt++)
    for (int rep = 0; rep < 3; rep++)
    {
      int sw = rep == 0 ? 0 : (int)(rnd() % 8), sh = rep == 0 ? 0 : (int)(rnd() % 5);
      uint32_t ti = make_ti(rnd(), (int)rnd(), sw, sh, fmt, rep & 1);
      uint32_t pltt = rnd() & 0x1FFF;
      CHECK(tex_decode(&mem, ti, pltt, out) == 1);
      int w = tex_width(ti), h = tex_height(ti);
      CHECK(w == (8 << sw) && h == (8 << sh));
      for (int t = 0; t < h; t++)
        for (int s = 0; s < w; s++)
        {
          int a;
          uint16_t c = ref_texel(ti, pltt, s, t, &a);
          uint32_t want = (uint32_t)(c & 31) | (uint32_t)((c >> 5) & 31) << 8 |
                          (uint32_t)((c >> 10) & 31) << 16 | (uint32_t)a << 24;
          checked++;
          if (out[t * w + s] != want)
            bad++;
        }
    }
  CHECK(bad == 0);
  CHECK(checked > 1000);
  CHECK(tex_decode(&mem, make_ti(0, 0, 0, 0, 0, 0), 0, out) == 0); // format 0 = no texture
}

// If a byte changes the decoded picture, the hash must change too. Every byte of the texture and
// palette memory is changed once, for each format.
static void test_hash_sound(void)
{
  fill_memory();
  static uint32_t before[64 * 64], after[64 * 64];
  int unsound = 0, tested = 0;
  for (int fmt = 1; fmt <= 7; fmt++)
    for (int rep = 0; rep < (fmt == 5 ? 3 : 1); rep++)
    {
      // small textures keep the test fast. The second and third compressed texture sit near the
      // end of a 128 KB page and across the 256 KB mark, where the info words move
      uint32_t base8 = rep == 0 ? 0x1230 : rep == 1 ? 0x3FF8 : 0x7FF0;
      uint32_t ti = make_ti(base8, 3, 1, 1, fmt, 1); // 16 x 16
      uint32_t pltt = 0x40 + (uint32_t)fmt * 3;
      tex_decode(&mem, ti, pltt, before);
      uint64_t h0 = tex_hash(&mem, ti, pltt);
      CHECK(tex_hash(&mem, ti, pltt) == h0);
      int w = tex_width(ti), h = tex_height(ti);
      for (int space = 0; space < 2; space++)
      {
        uint32_t size = space ? 6 * 0x4000 : 4 * 0x20000;
        for (uint32_t o = 0; o < size; o++)
        {
          uint8_t* p = space ? &pal_mem[o >> 14][o & 0x3FFF] : &tex_mem[o >> 17][o & 0x1FFFF];
          uint8_t old = *p;
          *p = (uint8_t)(old ^ 0xA5);
          tex_decode(&mem, ti, pltt, after);
          int changed = memcmp(before, after, (size_t)w * h * 4) != 0;
          tested++;
          if (changed && tex_hash(&mem, ti, pltt) == h0)
          {
            if (unsound < 5)
              printf("  unsound: format %d, %s byte 0x%X changes the picture but not the hash\n",
                     fmt, space ? "palette" : "texture", o);
            unsound++;
          }
          *p = old;
        }
      }
    }
  CHECK(unsound == 0);
  CHECK(tested > 1000000);

  // a slot that is not mapped does not hash like a slot of zeros
  TexMem some = mem;
  uint32_t ti = make_ti(0x1230, 3, 1, 1, 4, 0);
  uint64_t h_mapped = tex_hash(&some, ti, 5);
  some.tex[(0x1230 * 8 >> 17) & 3] = NULL;
  CHECK(tex_hash(&some, ti, 5) != h_mapped);
}

static void test_cache(void)
{
  fill_memory();
  TexCache* c = texcache_new(1 << 20); // 1 MB
  CHECK(c != NULL);
  uint32_t ti = make_ti(0x0100, 3, 2, 2, 4, 1); // 32 x 32, 256 colors
  TexEntry e1, e2;
  texcache_scene(c);
  CHECK(texcache_get(c, &mem, ti, 7, &e1) && e1.fresh && e1.w == 32 && e1.h == 32);
  CHECK(texcache_count(c) == 1 && texcache_decodes(c) == 1);
  CHECK(texcache_bytes(c) == 32 * 32 * 4);
  uint32_t copy[32 * 32];
  memcpy(copy, e1.px, sizeof copy);
  tex_decode(&mem, ti, 7, copy);
  CHECK(!memcmp(copy, e1.px, sizeof copy));

  // same scene again: same entry, no work
  CHECK(texcache_get(c, &mem, ti, 7, &e2) && !e2.fresh && e2.px == e1.px && e2.id == e1.id);
  // the wrap flags are not part of the picture: the same entry
  CHECK(texcache_get(c, &mem, ti ^ (15u << 16), 7, &e2) && !e2.fresh && e2.id == e1.id);
  // another palette is another entry
  CHECK(texcache_get(c, &mem, ti, 8, &e2) && e2.fresh && e2.id != e1.id);
  CHECK(texcache_count(c) == 2);

  // next scene, nothing changed: not decoded again
  texcache_scene(c);
  uint64_t d = texcache_decodes(c);
  CHECK(texcache_get(c, &mem, ti, 7, &e2) && !e2.fresh && texcache_decodes(c) == d);

  // a byte of the texture changes: decoded again in the next scene
  uint32_t base = (ti & 0xFFFF) * 8;
  tex_mem[(base >> 17) & 3][(base + 5) & 0x1FFFF] ^= 0xFF;
  texcache_scene(c);
  CHECK(texcache_get(c, &mem, ti, 7, &e2) && e2.fresh && e2.id == e1.id);
  tex_decode(&mem, ti, 7, copy);
  CHECK(!memcmp(copy, e2.px, sizeof copy));
  // a byte far away from it changes: nothing to do
  tex_mem[3][0x1FFF0] ^= 0xFF;
  texcache_scene(c);
  CHECK(texcache_get(c, &mem, ti, 7, &e2) && !e2.fresh);

  // direct color has no palette: two palette numbers, one entry
  uint32_t t7 = make_ti(0x0200, 0, 1, 1, 7, 0);
  int before = texcache_count(c);
  CHECK(texcache_get(c, &mem, t7, 1, &e1) && texcache_get(c, &mem, t7, 99, &e2));
  CHECK(e1.id == e2.id && texcache_count(c) == before + 1);

  // no texture, no entry
  CHECK(!texcache_get(c, &mem, make_ti(0, 0, 0, 0, 0, 0), 0, &e1));
  texcache_free(c);

  // a small cache: old entries go, entries of the current scene stay, and no room gives 0
  c = texcache_new(3 * 64 * 64 * 4); // room for three 64 x 64 textures
  texcache_scene(c);
  uint32_t big[5];
  for (int i = 0; i < 5; i++)
    big[i] = make_ti(0x1000 + (uint32_t)i * 0x400, 3, 3, 3, 4, 0); // 64 x 64
  for (int i = 0; i < 3; i++)
    CHECK(texcache_get(c, &mem, big[i], 1, &e1));
  CHECK(!texcache_get(c, &mem, big[3], 1, &e1)); // all three used by this scene: no room
  CHECK(texcache_count(c) == 3);
  texcache_scene(c);
  CHECK(texcache_get(c, &mem, big[3], 1, &e1)); // an old entry is dropped
  CHECK(texcache_count(c) == 3 && texcache_bytes(c) <= 3 * 64 * 64 * 4);
  CHECK(texcache_get(c, &mem, big[4], 1, &e1) && texcache_count(c) == 3);
  texcache_free(c);
}

int main(void)
{
  test_same_as_old();
  test_decode_full();
  test_cache();
  test_hash_sound();
  printf(failures ? "%d checks failed\n" : "all checks passed\n", failures);
  return failures != 0;
}
