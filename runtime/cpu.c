//Dispatch of computed jumps, processor modes and the CP15 coprocessor.
#include "ds.h"
#include "region.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>

static Func search(const FuncEntry* table_data, size_t n, uint32_t key)
{
  size_t lo = 0, hi = n;
  while (lo < hi)
  {
    size_t mid = (lo + hi) / 2;
    if (table_data[mid].key < key)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo < n && table_data[lo].key == key ? table_data[lo].fn : NULL;
}

//FNV-1a hash of the 16 words at an address. must give the same value as signature_long in
//tools/armc/recompiler.py
static uint32_t signature_long(uint32_t a)
{
  uint32_t hash = 2166136261u;
  for (uint32_t i = 0; i < 16; i++)
  {
    uint32_t word = rd16_9(a + 4 * i) | (uint32_t)rd16_9(a + 4 * i + 2) << 16;
    hash = (hash ^ word) * 16777619u;
  }
  return hash ? hash : 1;
}

//looks in the overlays for the function whose signature matches what is in memory
//finds the overlay function whose signature matches memory
//most functions exist in one overlay only and the short signature is enough
//when signature2 is not 0 the same address and signature exist in another overlay, so the long
//hash decides if no hash matches memory the first candidate is used, as before
static Func search_ov(uint32_t key)
{
  uint32_t a = key & ~1u;
  uint32_t signature = rd16_9(a) | rd16_9(a + 2) << 16;
  uint32_t long_one = 0;
  Func first_one = NULL;
  for (size_t k = 0; k < g_noverlays; k++)
  {
    const FuncEntryOv* t = g_overlays[k].t;
    size_t lo = 0, hi = *g_overlays[k].n;
    while (lo < hi)
    {
      size_t mid = (lo + hi) / 2;
      if (t[mid].key < key)
        lo = mid + 1;
      else
        hi = mid;
    }
    if (lo >= *g_overlays[k].n || t[lo].key != key || t[lo].signature != signature)
      continue;
    if (!t[lo].signature2)
      return t[lo].fn;
    if (!first_one)
      first_one = t[lo].fn;
    if (!long_one)
      long_one = signature_long(a);
    if (t[lo].signature2 == long_one)
      return t[lo].fn;
  }
  return first_one;
}

//the last computed jumps of the ARM9 (from which function, to where, r14 and r13), to understand a
//"missing code"
static struct
{
  uint32_t from, a, lr, sp;
} last[32];
static unsigned n_last;

void dispatch(Cpu* c, uint32_t t)
{
  uint32_t key = (t & 1) ? t : (t & ~3u);
  unsigned k = n_last++ & 31;
  last[k].from = c->func;
  last[k].a = t;
  last[k].lr = c->r[14];
  last[k].sp = c->r[13];
  Func f = search(g_funcs, g_nfuncs, key);
  if (!f)
    f = search_ov(key);
  if (!f)
    cpu_missing(c, key);
  f(c);
}

//print the last computed jumps of the ARM9 to o. used by the fault dump in main.c
void cpu_dump_jumps(FILE* o)
{
  fprintf(o, "    last computed jumps (from -> to, r14, r13):\n");
  for (unsigned i = n_last > 32 ? n_last - 32 : 0; i < n_last; i++)
    fprintf(o, "      %08X -> %08X  lr %08X  sp %08X\n", last[i & 31].from, last[i & 31].a,
            last[i & 31].lr, last[i & 31].sp);
}

void dispatch7(Cpu* c, uint32_t t)
{
  uint32_t key = (t & 1) ? t : (t & ~3u);
  Func f = search(g_funcs7, g_nfuncs7, key);
  if (!f)
    cpu_missing(c, key);
  f(c);
}

void cpu_call(Cpu* c, uint32_t t)
{
  if (c->id)
    dispatch7(c, t);
  else
    dispatch(c, t);
}

static void dump_a(FILE* o, Cpu* c)
{
  for (int i = 0; i < 16; i++)
    fprintf(o, "  r%-2d %08X%s", i, c->r[i], i % 4 == 3 ? "\n" : "");
  fprintf(o, "  cpsr %08X   last function %08X\n", cpu_get_cpsr(c), c->func);
}

static void dump(Cpu* c)
{
  dump_a(stderr, c);
}

//dump of a missing function, written to the console and to the log file
static void missing_a(FILE* o, Cpu* c, uint32_t addr)
{
  fprintf(o, "\n[cpu] missing translated code of the %s at %08X (%s)\n", c->id ? "ARM7" : "ARM9",
          addr & ~1u, (addr & 1) ? "Thumb" : "ARM");
  dump_a(o, c);
  if (!c->id)
  {
    fprintf(o, "  last computed jumps (from -> to, r14, r13):\n");
    for (unsigned i = n_last > 32 ? n_last - 32 : 0; i < n_last; i++)
      fprintf(o, "    %08X -> %08X  lr %08X  sp %08X\n", last[i & 31].from, last[i & 31].a,
              last[i & 31].lr, last[i & 31].sp);
  }
  fprintf(o, "  in memory:");
  for (int i = 0; i < 8; i++)
    fprintf(o, " %04X", c->id ? rd16_7((addr & ~1u) + i * 2) : rd16_9((addr & ~1u) + i * 2));
  fprintf(o, "\n");
}

//where a missing function is noted so that the next translation includes it
static const char* entry_list_for(Cpu* c)
{
  if (c->id)
    return "tools/entry_points/arm7.txt";
  return (g_region && g_region->label[0] == 'U') ? "tools/entry_points/usa_extra.txt"
                                                 : "tools/entry_points/eur.txt";
}

_Noreturn void cpu_missing(Cpu* c, uint32_t addr)
{
  FILE* lf = log_crash_begin("missing translated code");
  missing_a(stderr, c, addr);
  if (lf)
    missing_a(lf, c, addr);
  if (addr & ~1u) //jumping to address 0 is a fault, not missing code
  {
    const char* advice = "  to fix: add %s%08X to %s and translate again\n";
    fprintf(stderr, advice, (addr & 1) ? "T:" : "", addr & ~1u, entry_list_for(c));
    if (lf)
      fprintf(lf, advice, (addr & 1) ? "T:" : "", addr & ~1u, entry_list_for(c));
  }
  if (lf)
    fflush(lf);
  exit(2);
}

_Noreturn void cpu_undefined(Cpu* c, uint32_t addr, uint32_t op)
{
  FILE* lf = log_crash_begin("unsupported instruction");
  const char* what = "\n[cpu] unsupported instruction %08X at %08X (%s)\n";
  fprintf(stderr, what, op, addr, c->id ? "ARM7" : "ARM9");
  dump(c);
  if (lf)
  {
    fprintf(lf, what, op, addr, c->id ? "ARM7" : "ARM9");
    dump_a(lf, c);
    fflush(lf);
  }
  exit(3);
}

//each mode (IRQ, SVC...) has its own stack (r13), r14 and SPSR.
static int bank(uint32_t mode)
{
  int m = mode & 15;
  return m == 15 ? 0 : m; //user and system share registers
}

static void change_mode(Cpu* c, uint32_t new)
{
  int a = bank(c->ctl), b = bank(new);
  if (a != b)
  {
    c->bank_r13[a] = c->r[13];
    c->bank_r14[a] = c->r[14];
    c->bank_spsr[a] = c->spsr;
    c->r[13] = c->bank_r13[b];
    c->r[14] = c->bank_r14[b];
    c->spsr = c->bank_spsr[b];
  }
  c->ctl = (c->ctl & ~0x1Fu) | (new & 0x1F) | 0x10;
}

uint32_t cpu_get_cpsr(Cpu* c)
{
  return (c->n << 31) | (c->z << 30) | (c->c << 29) | (c->v << 28) | (c->q << 27) | c->ctl;
}

uint32_t cpu_get_spsr(Cpu* c)
{
  return c->spsr;
}

static uint32_t mix(uint32_t old, uint32_t new, uint32_t mask)
{
  uint32_t m = 0;
  for (int i = 0; i < 4; i++)
    if (mask & (1u << i))
      m |= 0xFFu << (8 * i);
  return (old & ~m) | (new & m);
}

static void set_cpsr(Cpu* c, uint32_t v)
{
  c->n = v >> 31;
  c->z = (v >> 30) & 1;
  c->c = (v >> 29) & 1;
  c->v = (v >> 28) & 1;
  c->q = (v >> 27) & 1;
  change_mode(c, v & 0x1F);
  c->ctl = v & 0xFF;
}

void cpu_msr(Cpu* c, int spsr, uint32_t mask, uint32_t val)
{
  if (spsr)
    c->spsr = mix(c->spsr, val, mask);
  else
    set_cpsr(c, mix(cpu_get_cpsr(c), val, mask));
}

void cpu_restore_spsr(Cpu* c)
{
  set_cpsr(c, c->spsr);
}

void cpu_set_cpsr(Cpu* c, uint32_t v)
{
  set_cpsr(c, v);
}

//---- CP15 ------------------------------------------------------------------
static uint32_t cp15[16][16][8][8];

uint32_t cp15_read(Cpu* c, int crn, int crm, int op1, int op2)
{
  (void)c;
  if (crn == 0 && crm == 0 && op2 == 0)
    return 0x41059461; //ARM946E-S
  if (crn == 0 && crm == 0 && op2 == 1)
    return 0x0F0D2112; //cache
  return cp15[crn][crm][op1][op2];
}

void cp15_write(Cpu* c, int crn, int crm, int op1, int op2, uint32_t v)
{
  cp15[crn][crm][op1][op2] = v;
  if (crn == 9 && crm == 1 && op2 == 0)
    g_dtcm_base = v & 0xFFFFF000u; //where the DTCM goes
  if (crn == 7 && ((crm == 0 && op2 == 4) || (crm == 8 && op2 == 2))) //wait for interrupt
    ds_wait(c, -1);
}
