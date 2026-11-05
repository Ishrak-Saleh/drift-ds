// Memory maps of the ARM9 and ARM7. Main RAM has a fast path in cpu.h.
// Everything else is here.
#include "ds.h"
#include "video.h"
#include <stdio.h>

uint8_t g_main[0x400000];
uint8_t g_itcm[0x8000];
uint8_t g_dtcm[0x4000];
uint32_t g_dtcm_base = 0x027E0000;

static uint8_t wram_comp[0x8000]; // WRAM compartida (32 KB)
static uint8_t wram7[0x10000]; // own WRAM of the ARM7 (64 KB)
static uint8_t vram7[0x40000]; // VRAM C/D lent to the ARM7 (wifi code)
static uint8_t wifi[0x10000]; // wifi registers and RAM (plain memory for now)

// Half of the shared WRAM that each processor sees, according to WRAMCNT
static uint8_t* wram_arm9(uint32_t a)
{
  switch (g_wramcnt & 3)
  {
  case 0:
    return wram_comp + (a & 0x7FFF);
  case 1:
    return wram_comp + 0x4000 + (a & 0x3FFF);
  case 2:
    return wram_comp + (a & 0x3FFF);
  }
  return NULL;
}
static uint8_t* wram_arm7(uint32_t a)
{
  switch (g_wramcnt & 3)
  {
  case 1:
    return wram_comp + (a & 0x3FFF);
  case 2:
    return wram_comp + 0x4000 + (a & 0x3FFF);
  case 3:
    return wram_comp + (a & 0x7FFF);
  }
  return wram7 + (a & 0xFFFF); // no shared WRAM: mirror of its own
}

// ---- ARM9 --------------------------------------------------------------------
static uint8_t* ptr9(uint32_t a)
{
  if ((uint32_t)(a - g_dtcm_base) < 0x4000)
    return g_dtcm + (a - g_dtcm_base);
  switch (a >> 24)
  {
  case 0x00:
  case 0x01:
    return g_itcm + (a & 0x7FFF);
  case 0x02:
    return g_main + (a & 0x3FFFFF);
  case 0x03:
    return wram_arm9(a);
  case 0x05:
    return g_palette + (a & 0x7FF);
  case 0x06:
    return vram_ptr9(a); // according to the assigned banks (video.c)
  case 0x07:
    return g_oam + (a & 0x7FF);
  }
  return NULL;
}

static uint32_t read9(uint32_t a, int n)
{
  uint8_t* p = ptr9(a);
  if (p)
  {
    uint32_t v = 0;
    memcpy(&v, p, n);
    return v;
  }
  if ((a >> 24) == 4)
    return io9_rd(a, n);
  return 0;
}
static void write9(uint32_t a, uint32_t v, int n)
{
  uint8_t* p = ptr9(a);
  if (p)
  {
    memcpy(p, &v, n);
    return;
  }
  if ((a >> 24) == 4)
  {
    io9_wr(a, v, n);
    return;
  }
  if ((a >> 24) == 6)
  {
  }
}

uint32_t mem_rd8(uint32_t a)
{
  return read9(a, 1);
}
uint32_t mem_rd16(uint32_t a)
{
  return read9(a, 2);
}
uint32_t mem_rd32(uint32_t a)
{
  return read9(a, 4);
}
void mem_wr8(uint32_t a, uint32_t v)
{
  write9(a, v, 1);
}
void mem_wr16(uint32_t a, uint32_t v)
{
  write9(a, v, 2);
}
void mem_wr32(uint32_t a, uint32_t v)
{
  write9(a, v, 4);
}

// ---- ARM7 --------------------------------------------------------------------
static uint8_t* ptr7(uint32_t a)
{
  switch (a >> 24)
  {
  case 0x02:
    return g_main + (a & 0x3FFFFF);
  case 0x03:
    return a >= 0x03800000 ? wram7 + (a & 0xFFFF) : wram_arm7(a);
  case 0x04:
    return a >= 0x04800000 ? wifi + (a & 0xFFFF) : NULL;
  case 0x06:
    return vram7 + (a & 0x3FFFF);
  }
  return NULL;
}

static uint32_t read7(uint32_t a, int n)
{
  uint8_t* p = ptr7(a);
  if (p)
  {
    uint32_t v = 0;
    memcpy(&v, p, n);
    return v;
  }
  if ((a >> 24) == 4)
    return io7_rd(a, n);
  return 0;
}
static void write7(uint32_t a, uint32_t v, int n)
{
  uint8_t* p = ptr7(a);
  if (p)
  {
    memcpy(p, &v, n);
    return;
  }
  if ((a >> 24) == 4)
    io7_wr(a, v, n);
}

uint32_t mem7_rd8(uint32_t a)
{
  return read7(a, 1);
}
uint32_t mem7_rd16(uint32_t a)
{
  return read7(a, 2);
}
uint32_t mem7_rd32(uint32_t a)
{
  return read7(a, 4);
}
void mem7_wr8(uint32_t a, uint32_t v)
{
  write7(a, v, 1);
}
void mem7_wr16(uint32_t a, uint32_t v)
{
  write7(a, v, 2);
}
void mem7_wr32(uint32_t a, uint32_t v)
{
  write7(a, v, 4);
}
