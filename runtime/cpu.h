//State of the processors (ARM9 and ARM7) and helpers used by the translated code.
//The ARM7 code is compiled with -DCPU7: so its rd32/wr32... use the memory map
//of the ARM7.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef struct Cpu
{
  uint32_t r[16];
  uint32_t n, z, c, v, q; //flags of the CPSR, each 0 or 1
  uint32_t ctl; //bits 0-7 of the CPSR: mode, I, F, T
  uint32_t spsr;
  uint32_t bank_r13[16], bank_r14[16], bank_spsr[16]; //per mode (ctl & 15)
  int id; //0 = ARM9, 1 = ARM7
  int32_t cycles; //"budget": when it runs out the hardware is looked at (cpu_tick)
  volatile uint32_t func; //translated function being run (for debugging)
  //return address handed over by a tail call or branch to another translated function. 0 = none.
  //the callee uses it as its entry_lr instead of r14, which an inner bl may have overwritten
  uint32_t elr;
} Cpu;

typedef void (*Func)(Cpu* c);
typedef struct
{
  uint32_t key;
  Func fn;
} FuncEntry; //key = address | 1 if Thumb
extern const FuncEntry g_funcs[], g_funcs7[];
extern const size_t g_nfuncs, g_nfuncs7;

//ARM9 overlays: several functions can share an address. The "signature"
//(its first 4 bytes) says which one is loaded in memory now.
//ARM9 overlays: several functions can share an address. the signature, their first 4 bytes, tells
//which one is loaded now signature2 is 0 for most entries. it holds a hash of 64 bytes only when
//two overlays have a function at the same address with the same short signature. search_ov in
//cpu.c then compares the hash with memory to pick the right one
typedef struct
{
  uint32_t key;
  Func fn;
  uint32_t signature;
  uint32_t signature2;
} FuncEntryOv;
typedef struct
{
  const FuncEntryOv* t;
  const size_t* n;
} TableOv;
extern const TableOv g_overlays[];
extern const size_t g_noverlays;

#define R (c->r)

void dispatch(Cpu* c, uint32_t target); //ARM9
void dispatch7(Cpu* c, uint32_t target); //ARM7
void cpu_call(Cpu* c, uint32_t target); //either of the two according to c->id
void cpu_tick(Cpu* c); //hardware, interrupts and turn of the other processor
_Noreturn void cpu_undefined(Cpu* c, uint32_t addr, uint32_t op);
_Noreturn void cpu_missing(Cpu* c, uint32_t addr);
uint32_t cpu_get_cpsr(Cpu* c);
uint32_t cpu_get_spsr(Cpu* c);
void cpu_msr(Cpu* c, int spsr, uint32_t mask, uint32_t val);
void cpu_restore_spsr(Cpu* c);
void cpu_set_cpsr(Cpu* c, uint32_t v);
uint32_t cp15_read(Cpu* c, int crn, int crm, int op1, int op2);
void cp15_write(Cpu* c, int crn, int crm, int op1, int op2, uint32_t v);
void bios_swi(Cpu* c, uint32_t n);

//user mode register r (for ldm/stm with "^")
static inline uint32_t* cpu_usr(Cpu* c, int r)
{
  uint32_t m = c->ctl & 15;
  if (r < 13 || m == 0 || m == 15)
    return &c->r[r];
  return r == 13 ? &c->bank_r13[0] : &c->bank_r14[0];
}

//threads of the DS operating system (threads.c)
void hle_save_context(Cpu* c);
void hle_load_context(Cpu* c);
void hle_init_context(Cpu* c); //a new thread reuses this context

void** hle_ctx_buf(Cpu* c, uint32_t ctx);

//---- memory ---------------------------------------------------------------
extern uint8_t g_main[0x400000];
extern uint8_t g_itcm[0x8000];
extern uint8_t g_dtcm[0x4000];
extern uint32_t g_dtcm_base;
uint32_t mem_rd8(uint32_t a);
uint32_t mem_rd16(uint32_t a);
uint32_t mem_rd32(uint32_t a);
void mem_wr8(uint32_t a, uint32_t v);
void mem_wr16(uint32_t a, uint32_t v);
void mem_wr32(uint32_t a, uint32_t v);
uint32_t mem7_rd8(uint32_t a);
uint32_t mem7_rd16(uint32_t a);
uint32_t mem7_rd32(uint32_t a);
void mem7_wr8(uint32_t a, uint32_t v);
void mem7_wr16(uint32_t a, uint32_t v);
void mem7_wr32(uint32_t a, uint32_t v);

//Fast path: main RAM (0x02xxxxxx). On the ARM9, except where the DTCM is.
#define EN_MAIN9(a) (((a) >> 24) == 2 && (uint32_t)((a) - g_dtcm_base) >= 0x4000)
#define EN_MAIN7(a) (((a) >> 24) == 2)

#define ACCESOS(suf, EN, slow)                                                                     \
  static inline uint32_t rd8##suf(uint32_t a)                                                      \
  {                                                                                                \
    if (EN(a))                                                                                     \
      return g_main[a & 0x3FFFFF];                                                                 \
    return slow##rd8(a);                                                                           \
  }                                                                                                \
  static inline uint32_t rd16##suf(uint32_t a)                                                     \
  {                                                                                                \
    a &= ~1u;                                                                                      \
    if (EN(a))                                                                                     \
    {                                                                                              \
      uint16_t v;                                                                                  \
      memcpy(&v, g_main + (a & 0x3FFFFF), 2);                                                      \
      return v;                                                                                    \
    }                                                                                              \
    return slow##rd16(a);                                                                          \
  }                                                                                                \
  static inline uint32_t rd32##suf(uint32_t a)                                                     \
  {                                                                                                \
    a &= ~3u;                                                                                      \
    if (EN(a))                                                                                     \
    {                                                                                              \
      uint32_t v;                                                                                  \
      memcpy(&v, g_main + (a & 0x3FFFFF), 4);                                                      \
      return v;                                                                                    \
    }                                                                                              \
    return slow##rd32(a);                                                                          \
  }                                                                                                \
  static inline void wr8##suf(uint32_t a, uint32_t v)                                              \
  {                                                                                                \
    if (EN(a))                                                                                     \
    {                                                                                              \
      g_main[a & 0x3FFFFF] = (uint8_t)v;                                                           \
      return;                                                                                      \
    }                                                                                              \
    slow##wr8(a, v);                                                                               \
  }                                                                                                \
  static inline void wr16##suf(uint32_t a, uint32_t v)                                             \
  {                                                                                                \
    a &= ~1u;                                                                                      \
    if (EN(a))                                                                                     \
    {                                                                                              \
      uint16_t x = (uint16_t)v;                                                                    \
      memcpy(g_main + (a & 0x3FFFFF), &x, 2);                                                      \
      return;                                                                                      \
    }                                                                                              \
    slow##wr16(a, v);                                                                              \
  }                                                                                                \
  static inline void wr32##suf(uint32_t a, uint32_t v)                                             \
  {                                                                                                \
    a &= ~3u;                                                                                      \
    if (EN(a))                                                                                     \
    {                                                                                              \
      memcpy(g_main + (a & 0x3FFFFF), &v, 4);                                                      \
      return;                                                                                      \
    }                                                                                              \
    slow##wr32(a, v);                                                                              \
  }

ACCESOS(_9, EN_MAIN9, mem_)
ACCESOS(_7, EN_MAIN7, mem7_)

//the translated code uses rd32() etc.: of the ARM9 or of the ARM7 depending on how it is compiled
#ifdef CPU7
#define rd8 rd8_7
#define rd16 rd16_7
#define rd32 rd32_7
#define wr8 wr8_7
#define wr16 wr16_7
#define wr32 wr32_7
#else
#define rd8 rd8_9
#define rd16 rd16_9
#define rd32 rd32_9
#define wr8 wr8_9
#define wr16 wr16_9
#define wr32 wr32_9
#endif

//for the runtime, which works with both processors
static inline uint32_t brd8(Cpu* c, uint32_t a)
{
  return c->id ? rd8_7(a) : rd8_9(a);
}
static inline uint32_t brd16(Cpu* c, uint32_t a)
{
  return c->id ? rd16_7(a) : rd16_9(a);
}
static inline uint32_t brd32(Cpu* c, uint32_t a)
{
  return c->id ? rd32_7(a) : rd32_9(a);
}
static inline void bwr8(Cpu* c, uint32_t a, uint32_t v)
{
  if (c->id)
    wr8_7(a, v);
  else
    wr8_9(a, v);
}
static inline void bwr16(Cpu* c, uint32_t a, uint32_t v)
{
  if (c->id)
    wr16_7(a, v);
  else
    wr16_9(a, v);
}
static inline void bwr32(Cpu* c, uint32_t a, uint32_t v)
{
  if (c->id)
    wr32_7(a, v);
  else
    wr32_9(a, v);
}

//---- operaciones -----------------------------------------------------------
static inline uint32_t ror32(uint32_t v, uint32_t s)
{
  s &= 31;
  return s ? (v >> s) | (v << (32 - s)) : v;
}
//LDR with unaligned address: reads aligned and rotates
static inline uint32_t ldr32(uint32_t a)
{
  return ror32(rd32(a), (a & 3) * 8);
}
static inline uint32_t clz32(uint32_t v)
{
  return v ? (uint32_t)__builtin_clz(v) : 32;
}

static inline uint32_t nz(Cpu* c, uint32_t r)
{
  c->n = r >> 31;
  c->z = r == 0;
  return r;
}
static inline uint32_t add_f(Cpu* c, uint32_t a, uint32_t b)
{
  uint32_t r = a + b;
  c->n = r >> 31;
  c->z = r == 0;
  c->c = r < a;
  c->v = ((~(a ^ b)) & (a ^ r)) >> 31;
  return r;
}
static inline uint32_t adc_f(Cpu* c, uint32_t a, uint32_t b)
{
  uint64_t t = (uint64_t)a + b + c->c;
  uint32_t r = (uint32_t)t;
  c->n = r >> 31;
  c->z = r == 0;
  c->c = (uint32_t)(t >> 32);
  c->v = ((~(a ^ b)) & (a ^ r)) >> 31;
  return r;
}
static inline uint32_t sub_f(Cpu* c, uint32_t a, uint32_t b)
{
  uint32_t r = a - b;
  c->n = r >> 31;
  c->z = r == 0;
  c->c = a >= b;
  c->v = ((a ^ b) & (a ^ r)) >> 31;
  return r;
}
static inline uint32_t sbc_f(Cpu* c, uint32_t a, uint32_t b)
{
  uint32_t borrow = c->c ^ 1;
  uint32_t r = a - b - borrow;
  c->n = r >> 31;
  c->z = r == 0;
  c->c = (uint64_t)a >= (uint64_t)b + borrow;
  c->v = ((a ^ b) & (a ^ r)) >> 31;
  return r;
}

//shifts by register, with the bit that comes out (carry)
static inline uint32_t lsl_c(uint32_t v, uint32_t s, uint32_t* cy)
{
  if (s == 0)
    return v;
  if (s < 32)
  {
    *cy = (v >> (32 - s)) & 1;
    return v << s;
  }
  *cy = s == 32 ? (v & 1) : 0;
  return 0;
}
static inline uint32_t lsr_c(uint32_t v, uint32_t s, uint32_t* cy)
{
  if (s == 0)
    return v;
  if (s < 32)
  {
    *cy = (v >> (s - 1)) & 1;
    return v >> s;
  }
  *cy = s == 32 ? (v >> 31) : 0;
  return 0;
}
static inline uint32_t asr_c(uint32_t v, uint32_t s, uint32_t* cy)
{
  if (s == 0)
    return v;
  if (s < 32)
  {
    *cy = ((uint32_t)((int32_t)v >> (s - 1))) & 1;
    return (uint32_t)((int32_t)v >> s);
  }
  *cy = v >> 31;
  return (uint32_t)((int32_t)v >> 31);
}
static inline uint32_t ror_c(uint32_t v, uint32_t s, uint32_t* cy)
{
  if (s == 0)
    return v;
  s &= 31;
  if (s == 0)
  {
    *cy = v >> 31;
    return v;
  }
  *cy = (v >> (s - 1)) & 1;
  return (v >> s) | (v << (32 - s));
}

//saturated arithmetic (QADD and company) of the ARMv5TE
static inline uint32_t qadd(Cpu* c, uint32_t a, uint32_t b)
{
  int64_t r = (int64_t)(int32_t)a + (int32_t)b;
  if (r > INT32_MAX)
  {
    c->q = 1;
    return INT32_MAX;
  }
  if (r < INT32_MIN)
  {
    c->q = 1;
    return (uint32_t)INT32_MIN;
  }
  return (uint32_t)r;
}
static inline uint32_t qsub(Cpu* c, uint32_t a, uint32_t b)
{
  int64_t r = (int64_t)(int32_t)a - (int32_t)b;
  if (r > INT32_MAX)
  {
    c->q = 1;
    return INT32_MAX;
  }
  if (r < INT32_MIN)
  {
    c->q = 1;
    return (uint32_t)INT32_MIN;
  }
  return (uint32_t)r;
}
