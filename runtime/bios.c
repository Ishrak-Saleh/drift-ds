// BIOS functions of the ARM9 and ARM7 (the SWI calls), written in C.
// Nintendo BIOS is not used: what each one does is reimplemented.
#include "ds.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void cpu_set(Cpu* c, int fast)
{
  uint32_t src = c->r[0], dst = c->r[1], ctl = c->r[2];
  uint32_t n = ctl & 0x1FFFFF;
  int fill = (ctl >> 24) & 1;
  if (fast || (ctl >> 26) & 1)
  {
    uint32_t v = brd32(c, src);
    for (uint32_t i = 0; i < n; i++)
      bwr32(c, dst + i * 4, fill ? v : brd32(c, src + i * 4));
  }
  else
  {
    uint32_t v = brd16(c, src);
    for (uint32_t i = 0; i < n; i++)
      bwr16(c, dst + i * 2, fill ? v : brd16(c, src + i * 2));
  }
}

// writes an uncompressed buffer: byte by byte (WRAM) or 16 bit (VRAM)
static void take(Cpu* c, uint32_t dst, const uint8_t* buf, uint32_t total, int vram)
{
  for (uint32_t j = 0; j < total; j += vram ? 2 : 1)
  {
    if (vram)
      bwr16(c, dst + j, buf[j] | (j + 1 < total ? buf[j + 1] << 8 : 0));
    else
      bwr8(c, dst + j, buf[j]);
  }
}

// LZ77 (SWI 0x11 and 0x12): 4 byte header, then blocks of 8 with flags
static void lz77(Cpu* c, int vram)
{
  uint32_t src = c->r[0];
  uint32_t total = brd32(c, src) >> 8, done = 0;
  src += 4;
  uint8_t* buf = malloc(total + 1);
  while (done < total)
  {
    uint32_t flags = brd8(c, src++);
    for (int k = 0; k < 8 && done < total; k++, flags <<= 1)
    {
      if (flags & 0x80)
      {
        uint32_t b1 = brd8(c, src), b2 = brd8(c, src + 1);
        src += 2;
        uint32_t len = (b1 >> 4) + 3, disp = (((b1 & 15) << 8) | b2) + 1;
        for (uint32_t j = 0; j < len && done < total; j++, done++)
          buf[done] = done >= disp ? buf[done - disp] : 0;
      }
      else
      {
        buf[done++] = (uint8_t)brd8(c, src++);
      }
    }
  }
  take(c, c->r[1], buf, total, vram);
  free(buf);
}

// RL (SWI 0x14 and 0x15)
static void rl(Cpu* c, int vram)
{
  uint32_t src = c->r[0];
  uint32_t total = brd32(c, src) >> 8, done = 0;
  src += 4;
  uint8_t* buf = malloc(total + 1);
  while (done < total)
  {
    uint32_t f = brd8(c, src++);
    if (f & 0x80)
    {
      uint32_t n = (f & 0x7F) + 3, b = brd8(c, src++);
      for (uint32_t j = 0; j < n && done < total; j++)
        buf[done++] = (uint8_t)b;
    }
    else
    {
      uint32_t n = (f & 0x7F) + 1;
      for (uint32_t j = 0; j < n && done < total; j++)
        buf[done++] = (uint8_t)brd8(c, src++);
    }
  }
  take(c, c->r[1], buf, total, vram);
  free(buf);
}

// Huffman (SWI 0x13): tree of 8 or 4 bits, data in 32 bit words
static void huffman(Cpu* c)
{
  uint32_t src = c->r[0], hdr = brd32(c, src);
  uint32_t bits = hdr & 15, total = hdr >> 8, done = 0;
  uint32_t tree = src + 4, raw_data = tree + (brd8(c, tree) + 1) * 2;
  uint8_t* buf = calloc(total + 4, 1);
  uint32_t node = tree + 1, acc = 0, nacc = 0;
  while (done < total)
  {
    uint32_t w = brd32(c, raw_data);
    raw_data += 4;
    for (int i = 31; i >= 0 && done < total; i--)
    {
      uint32_t b = (w >> i) & 1, n = brd8(c, node);
      uint32_t sig = (node & ~1u) + (n & 0x3F) * 2 + 2 + b;
      if (n & (b ? 0x40 : 0x80))
      { // hoja
        acc |= brd8(c, sig) << nacc;
        nacc += bits;
        node = tree + 1;
        if (nacc == 8)
        {
          buf[done++] = (uint8_t)acc;
          acc = 0;
          nacc = 0;
        }
      }
      else
      {
        node = sig;
      }
    }
  }
  take(c, c->r[1], buf, total, 0);
  free(buf);
}

static uint32_t crc16(Cpu* c, uint32_t crc, uint32_t src, uint32_t len)
{
  static const uint16_t val[8] = {0xC0C1, 0xC181, 0xC301, 0xC601, 0xCC01, 0xD801, 0xF001, 0xA001};
  for (uint32_t i = 0; i < len; i++)
  {
    crc ^= brd8(c, src + i);
    for (int j = 0; j < 8; j++)
    {
      int carry = crc & 1;
      crc >>= 1;
      if (carry)
        crc ^= (uint32_t)val[j] << (7 - j);
    }
  }
  return crc & 0xFFFF;
}

// BitUnPack (SWI 0x10): widens data from N bits to M bits
static void bit_unpack(Cpu* c)
{
  uint32_t src = c->r[0], dst = c->r[1], info = c->r[2];
  uint32_t len = brd16(c, info), wsrc = brd8(c, info + 2), wdst = brd8(c, info + 3);
  uint32_t off = brd32(c, info + 4), zero = off >> 31;
  off &= 0x7FFFFFFF;
  uint32_t out = 0, nout = 0;
  for (uint32_t i = 0; i < len; i++)
  {
    uint32_t b = brd8(c, src + i);
    for (uint32_t k = 0; k < 8; k += wsrc)
    {
      uint32_t v = (b >> k) & ((1u << wsrc) - 1);
      if (v || zero)
        v += off;
      out |= v << nout;
      nout += wdst;
      if (nout >= 32)
      {
        bwr32(c, dst, out);
        dst += 4;
        out = 0;
        nout = 0;
      }
    }
  }
}

void bios_swi(Cpu* c, uint32_t n)
{
  switch (n)
  {
  case 0x03:
    ds_yield(c);
    return; // WaitByLoop: busy wait, good moment to give up the turn
  case 0x04:
  case 0x05:
  case 0x06: // IntrWait, VBlankIntrWait, Halt
    ds_wait(c, (int)n);
    return;
  case 0x07:
    ds_wait(c, 6);
    return; // Sleep (ARM7): same as Halt
  case 0x08:
    return; // SoundBias (ARM7)
  case 0x09:
  { // Div
    int32_t a = (int32_t)c->r[0], b = (int32_t)c->r[1];
    if (b == 0)
    {
      c->r[0] = a < 0 ? (uint32_t)-1 : 1;
      c->r[1] = (uint32_t)a;
      c->r[3] = 1;
      return;
    }
    if (a == INT32_MIN && b == -1)
    {
      c->r[0] = (uint32_t)a;
      c->r[1] = 0;
      c->r[3] = (uint32_t)a;
      return;
    }
    int32_t q = a / b;
    c->r[0] = (uint32_t)q;
    c->r[1] = (uint32_t)(a % b);
    c->r[3] = (uint32_t)(q < 0 ? -q : q);
    return;
  }
  case 0x0B:
    cpu_set(c, 0);
    return;
  case 0x0C:
    cpu_set(c, 1);
    return;
  case 0x0D:
  { // Sqrt
    uint32_t v = c->r[0], r = 0, bit = 1u << 30;
    while (bit > v)
      bit >>= 2;
    while (bit)
    {
      if (v >= r + bit)
      {
        v -= r + bit;
        r = (r >> 1) + bit;
      }
      else
        r >>= 1;
      bit >>= 2;
    }
    c->r[0] = r;
    return;
  }
  case 0x0E:
    c->r[0] = crc16(c, c->r[0], c->r[1], c->r[2]);
    return;
  case 0x0F:
    c->r[0] = 0;
    return; // IsDebugger
  case 0x10:
    bit_unpack(c);
    return;
  case 0x11:
    lz77(c, 0);
    return;
  case 0x12:
    lz77(c, 1);
    return;
  case 0x13:
    huffman(c);
    return;
  case 0x14:
    rl(c, 0);
    return;
  case 0x15:
    rl(c, 1);
    return;
  case 0x1A: // GetSineTable (ARM7): sine from 0 to 90 degrees in 64 steps, in 1/4096
    c->r[0] = (uint32_t)(int32_t)lround(sin((c->r[0] & 63) * 3.14159265358979 / 128) * 4096);
    return;
  case 0x1B: // GetPitchTable (ARM7): (2^(n/768) - 1) * 0x10000
    c->r[0] = (uint32_t)lround((pow(2.0, (c->r[0] % 768) / 768.0) - 1) * 65536) & 0xFFFF;
    return;
  case 0x1C:
  { // GetVolumeTable (ARM7): 724 steps of 1/8 dB, each 6 dB section repeats 64..127
    uint32_t i = c->r[0] % 724;
    double db = (i - 723.0) / 8.0;
    double amp = pow(10.0, db / 20.0);
    while (amp < 0.5 && amp > 0)
      amp *= 2; // the section comes from the channel divider
    c->r[0] = i == 0 ? 0 : (uint32_t)lround(amp * 127);
    return;
  }
  case 0x1F:
    return; // CustomPost
  }
  fprintf(stderr, "[cpu] SWI %02X of the %s BIOS is not implemented yet\n", n,
          c->id ? "ARM7" : "ARM9");
  exit(4);
}
