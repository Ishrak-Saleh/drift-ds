//Texture decoding of the DS 3D engine: whole textures, a hash of their bytes, and a cache.
//See texdecode.h.
#include "texdecode.h"
#include <stdlib.h>
#include <string.h>

int tex_decode(const TexMem* m, uint32_t ti, uint32_t pltt, uint32_t* out)
{
  if (((ti >> 26) & 7) == 0)
    return 0;
  int w = tex_width(ti), h = tex_height(ti);
  for (int t = 0; t < h; t++)
    for (int s = 0; s < w; s++)
    {
      int a;
      uint16_t c = texmem_texel(m, ti, pltt, s, t, &a);
      out[(size_t)t * (size_t)w + (size_t)s] = (uint32_t)(c & 31) | (uint32_t)((c >> 5) & 31) << 8 |
                                               (uint32_t)((c >> 10) & 31) << 16 | (uint32_t)a << 24;
    }
  return 1;
}

//---- hash of the bytes a texture reads ----

static uint64_t mix(uint64_t h, uint64_t v)
{
  h ^= v;
  h *= 0x9E3779B97F4A7C15ull;
  h ^= h >> 29;
  return h;
}

//p == NULL is a slot that is not mapped: it reads as zeros, but it must not hash like mapped
//zeros, so the picture is not wrongly kept when a bank is mapped later
static uint64_t hash_bytes(uint64_t h, const uint8_t* p, size_t n)
{
  if (!p)
    return mix(h, 0x4E554C4C00000000ull ^ (uint64_t)n);
  size_t i = 0;
  for (; i + 8 <= n; i += 8)
  {
    uint64_t v;
    memcpy(&v, p + i, 8);
    h = mix(h, v);
  }
  uint64_t tail = 0;
  for (size_t k = 0; i < n; i++, k += 8)
    tail |= (uint64_t)p[i] << k;
  return mix(h, tail ^ ((uint64_t)n << 56));
}

//texture memory is 4 slots of 128 KB, an address wraps at 512 KB (texmem_tex8)
static uint64_t hash_tex_range(const TexMem* m, uint64_t h, uint32_t start, uint32_t len)
{
  while (len)
  {
    uint32_t slot = (start >> 17) & 3, off = start & 0x1FFFF;
    uint32_t n = 0x20000 - off;
    if (n > len)
      n = len;
    h = hash_bytes(h, m->tex[slot] ? m->tex[slot] + off : NULL, n);
    start += n;
    len -= n;
  }
  return h;
}

//palette memory is 6 slots of 16 KB, an address wraps at 96 KB (texmem_pal16)
static uint64_t hash_pal_range(const TexMem* m, uint64_t h, uint32_t start, uint32_t len)
{
  while (len)
  {
    uint32_t o = start % 0x18000, slot = o >> 14, off = o & 0x3FFF;
    uint32_t n = 0x4000 - off;
    if (n > len)
      n = len;
    h = hash_bytes(h, m->pal[slot] ? m->pal[slot] + off : NULL, n);
    start += n;
    len -= n;
  }
  return h;
}

uint64_t tex_hash(const TexMem* m, uint32_t ti, uint32_t pltt)
{
  int fmt = (ti >> 26) & 7;
  uint64_t h = 0x1234567ull ^ (uint64_t)(ti & TEX_KEY_MASK);
  if (fmt == 0)
    return h;
  uint32_t w = (uint32_t)tex_width(ti), hh = (uint32_t)tex_height(ti);
  uint32_t texels = w * hh, base = (ti & 0xFFFF) * 8, pb = pltt * 16;
  //how many bytes of texture data, and which part of the palette, each format reads
  uint32_t data = 0, pal_start = pb, pal_len = 0;
  switch (fmt)
  {
  case 1: //A3I5: 32 colors
    data = texels;
    pal_len = 64;
    break;
  case 2: //4 colors, the palette base counts in 8 byte steps
    data = texels / 4;
    pal_start = pltt * 8;
    pal_len = 8;
    break;
  case 3: //16 colors
    data = texels / 2;
    pal_len = 32;
    break;
  case 4: //256 colors
    data = texels;
    pal_len = 512;
    break;
  case 5: //compressed: any palette entry up to 0x3FFF * 4 + 8 bytes from the base can be used
    data = texels / 4;
    pal_len = 0x10010;
    break;
  case 6: //A5I3: 8 colors
    data = texels;
    pal_len = 16;
    break;
  case 7: //direct color
    data = texels * 2;
    break;
  }
  h = hash_tex_range(m, h, base, data);
  if (fmt == 5)
  { //each 4x4 block has a 16 bit info word in the second slot (see texmem_texel). Blocks that sit
    //in the same 128 KB page of the data have their words next to each other
    uint32_t end = base + data;
    for (uint32_t r0 = base; r0 < end;)
    {
      uint32_t r1 = (r0 | 0x1FFFF) + 1; //end of this 128 KB page of the data
      if (r1 > end)
        r1 = end;
      uint32_t slot = (r0 & 0x7FFFF) >= 0x40000 ? 0x10000 : 0;
      uint32_t info = 0x20000 + slot + ((r0 & 0x1FFFF) >> 1);
      h = hash_tex_range(m, h, info, (r1 - r0) / 4 * 2);
      r0 = r1;
    }
  }
  if (pal_len)
    h = hash_pal_range(m, h, pal_start, pal_len);
  return h;
}

//---- cache ----

typedef struct
{
  uint32_t key_ti, pltt; //key
  uint64_t hash; //of the bytes when it was decoded
  uint64_t last_scene; //last scene that asked for it
  uint64_t checked_scene; //last scene whose bytes were hashed for it
  uint32_t* px;
  int w, h;
  int next; //next entry in the same bucket, or the next free entry
  int used;
} Ent;

#define BUCKETS 1024 //power of 2

struct TexCache
{
  Ent* e;
  int n, cap; //entries in use or free (slots handed out), allocated
  int free_head; //first free slot, -1 if none
  int bucket[BUCKETS];
  size_t bytes, max_bytes;
  uint64_t scene, decodes;
  int alive;
};

TexCache* texcache_new(size_t max_bytes)
{
  TexCache* c = calloc(1, sizeof *c);
  if (!c)
    return NULL;
  for (int i = 0; i < BUCKETS; i++)
    c->bucket[i] = -1;
  c->free_head = -1;
  c->max_bytes = max_bytes;
  c->scene = 1;
  return c;
}

void texcache_free(TexCache* c)
{
  if (!c)
    return;
  for (int i = 0; i < c->n; i++)
    free(c->e[i].px);
  free(c->e);
  free(c);
}

void texcache_scene(TexCache* c)
{
  c->scene++;
}

static unsigned bucket_of(uint32_t key_ti, uint32_t pltt)
{
  uint64_t v = ((uint64_t)key_ti << 32 | pltt) * 0x9E3779B97F4A7C15ull;
  return (unsigned)(v >> 40) & (BUCKETS - 1);
}

static void remove_entry(TexCache* c, int i)
{
  Ent* e = &c->e[i];
  int* link = &c->bucket[bucket_of(e->key_ti, e->pltt)];
  while (*link != i && *link >= 0)
    link = &c->e[*link].next;
  if (*link == i)
    *link = e->next;
  c->bytes -= (size_t)e->w * (size_t)e->h * 4;
  free(e->px);
  e->px = NULL;
  e->used = 0;
  e->next = c->free_head;
  c->free_head = i;
  c->alive--;
}

//makes room for `need` bytes by dropping the entries that were used longest ago, never one that
//the current scene already used. Returns 1 when it fits.
static int make_room(TexCache* c, size_t need)
{
  while (c->bytes + need > c->max_bytes)
  {
    int best = -1;
    for (int i = 0; i < c->n; i++)
      if (c->e[i].used && c->e[i].last_scene < c->scene &&
          (best < 0 || c->e[i].last_scene < c->e[best].last_scene))
        best = i;
    if (best < 0)
      return 0;
    remove_entry(c, best);
  }
  return 1;
}

static int new_slot(TexCache* c)
{
  if (c->free_head >= 0)
  {
    int i = c->free_head;
    c->free_head = c->e[i].next;
    return i;
  }
  if (c->n == c->cap)
  {
    int cap = c->cap ? c->cap * 2 : 64;
    Ent* ne = realloc(c->e, (size_t)cap * sizeof *ne);
    if (!ne)
      return -1;
    c->e = ne;
    c->cap = cap;
  }
  return c->n++;
}

int texcache_get(TexCache* c, const TexMem* m, uint32_t ti, uint32_t pltt, TexEntry* out)
{
  if (((ti >> 26) & 7) == 0)
    return 0;
  uint32_t key = ti & TEX_KEY_MASK;
  if (((ti >> 26) & 7) == 7)
    pltt = 0; //direct color has no palette
  unsigned b = bucket_of(key, pltt);
  int i = c->bucket[b];
  while (i >= 0 && !(c->e[i].key_ti == key && c->e[i].pltt == pltt))
    i = c->e[i].next;
  int w = tex_width(ti), h = tex_height(ti);
  size_t bytes = (size_t)w * (size_t)h * 4;
  int fresh = 0;
  if (i >= 0)
  {
    Ent* e = &c->e[i];
    if (e->checked_scene != c->scene)
    { //first use in this scene: are the bytes still the same?
      uint64_t hs = tex_hash(m, ti, pltt);
      e->checked_scene = c->scene;
      if (hs != e->hash)
      {
        tex_decode(m, ti, pltt, e->px);
        e->hash = hs;
        c->decodes++;
        fresh = 1;
      }
    }
    e->last_scene = c->scene;
  }
  else
  {
    if (!make_room(c, bytes))
      return 0;
    uint32_t* px = malloc(bytes);
    if (!px)
      return 0;
    i = new_slot(c);
    if (i < 0)
    {
      free(px);
      return 0;
    }
    Ent* e = &c->e[i];
    memset(e, 0, sizeof *e);
    e->key_ti = key;
    e->pltt = pltt;
    e->px = px;
    e->w = w;
    e->h = h;
    e->used = 1;
    e->hash = tex_hash(m, ti, pltt);
    e->checked_scene = e->last_scene = c->scene;
    tex_decode(m, ti, pltt, px);
    e->next = c->bucket[b];
    c->bucket[b] = i;
    c->bytes += bytes;
    c->alive++;
    c->decodes++;
    fresh = 1;
  }
  Ent* e = &c->e[i];
  out->w = e->w;
  out->h = e->h;
  out->px = e->px;
  out->id = (uint32_t)i;
  out->fresh = fresh;
  return 1;
}

int texcache_count(const TexCache* c)
{
  return c->alive;
}

size_t texcache_bytes(const TexCache* c)
{
  return c->bytes;
}

uint64_t texcache_decodes(const TexCache* c)
{
  return c->decodes;
}
