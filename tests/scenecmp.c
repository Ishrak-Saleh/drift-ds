// Picture compare for the GPU work. See scenecmp.h.
#include "scenecmp.h"
#include <stdlib.h>
#include <string.h>

CmpRule cmp_default_rule(void)
{
  CmpRule r = {1, 3, 0.1, 4};
  return r;
}

static int channel_diff(uint16_t a, uint16_t b)
{
  int d = 0;
  for (int s = 0; s <= 10; s += 5)
  {
    int x = abs(((a >> s) & 31) - ((b >> s) & 31));
    if (x > d)
      d = x;
  }
  return d;
}

// a transparent pixel has no colour that matters
static int pixel_diff(uint16_t ca, uint8_t aa, uint16_t cb, uint8_t ab)
{
  if (aa == 0 && ab == 0)
    return 0;
  int d = channel_diff(ca, cb);
  int da = abs((int)aa - (int)ab);
  return d > da ? d : da;
}

// shifted: the got pixel equals (within step) one of the 8 neighbours of the reference pixel, so
// the colour is right and only the sampling position moved a hair (a texel or polygon boundary)
static int is_shifted(const uint16_t* c, const uint8_t* a, int w, int h, int x, int y, uint16_t gc,
                      uint8_t ga, int step)
{
  for (int dy = -1; dy <= 1; dy++)
    for (int dx = -1; dx <= 1; dx++)
    {
      int nx = x + dx, ny = y + dy;
      if ((!dx && !dy) || nx < 0 || ny < 0 || nx >= w || ny >= h)
        continue;
      int j = ny * w + nx;
      if (pixel_diff(c[j], a[j], gc, ga) <= step && (a[j] == 0) == (ga == 0))
        return 1;
    }
  return 0;
}

// edge: some neighbour of the reference pixel is clearly different from it
static int is_edge(const uint16_t* c, const uint8_t* a, int w, int h, int x, int y, int step)
{
  for (int dy = -1; dy <= 1; dy++)
    for (int dx = -1; dx <= 1; dx++)
    {
      int nx = x + dx, ny = y + dy;
      if ((!dx && !dy) || nx < 0 || ny < 0 || nx >= w || ny >= h)
        continue;
      int i = y * w + x, j = ny * w + nx;
      if (pixel_diff(c[i], a[i], c[j], a[j]) > step || (a[i] == 0) != (a[j] == 0))
        return 1;
    }
  return 0;
}

CmpResult cmp_pictures(const uint16_t* ref_c, const uint8_t* ref_a, const uint16_t* got_c,
                       const uint8_t* got_a, int w, int h, const CmpRule* rule, uint8_t* diff_out)
{
  CmpResult r;
  memset(&r, 0, sizeof r);
  CmpRule rl = rule ? *rule : cmp_default_rule();
  if (!ref_c || !ref_a || !got_c || !got_a || w <= 0 || h <= 0)
    return r; // pass stays 0
  r.total = w * h;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
    {
      int i = y * w + x;
      uint8_t mark = 0;
      int d = pixel_diff(ref_c[i], ref_a[i], got_c[i], got_a[i]);
      if (d == 0)
        mark = 0;
      else if (is_edge(ref_c, ref_a, w, h, x, y, rl.edge_step))
      {
        r.edge++;
        mark = 2;
      }
      else if ((ref_a[i] == 0) != (got_a[i] == 0))
      {
        r.alpha_wrong++;
        r.differing++;
        mark = 4;
      }
      else if (d > rl.small_step &&
               is_shifted(ref_c, ref_a, w, h, x, y, got_c[i], got_a[i], rl.small_step))
      {
        r.shifted++;
        mark = 5;
      }
      else if (d > rl.small_step)
      {
        r.differing++;
        if (d > r.worst)
          r.worst = d;
        mark = 3;
      }
      else
        mark = 1;
      if (diff_out)
        diff_out[i] = mark;
    }
  // the share is taken of the whole picture, as the rule was agreed (0.1 percent of the pixels)
  r.percent = 100.0 * r.differing / r.total;
  r.shifted_percent = 100.0 * r.shifted / r.total;
  r.pass = r.percent <= rl.max_percent && r.shifted_percent <= rl.max_percent &&
           r.worst <= rl.max_step && r.alpha_wrong == 0;
  return r;
}
