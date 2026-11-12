//Threads of the DS operating system (NitroSDK) with Windows fibers.
//On the DS, switching thread means saving all registers in a "context"
//(OS_SaveContext) and loading those of another (OS_LoadContext), jumping to where it
//stopped. In C you cannot jump into the middle of another function, so each DS
//thread is a fiber (its own stack) and:
//- OS_SaveContext: notes which fiber owns the context and returns 0.
//- OS_LoadContext: switches to the owning fiber, which was stopped inside its
//own OS_LoadContext. It restores the registers and returns (see enter()).
//If the context belongs to a new thread, it creates its fiber and starts it.
//Both processors have their own threads (g_so[c->id]).
#include "compat.h" //before cpu.h: its R macro clashes with windows.h
#include "ds.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct
{
  uint32_t ctx; //address of the context in DS memory
  void* fiber;
  int stored;
} Thread;

static Thread threads[2][64];
static int nthreads[2];
static uint32_t loading[2]; //context being loaded on each processor

static Thread* search(Cpu* c, uint32_t ctx)
{
  Thread* t = threads[c->id];
  for (int i = 0; i < nthreads[c->id]; i++)
    if (t[i].ctx == ctx)
      return &t[i];
  if (nthreads[c->id] == 64)
  {
    fprintf(stderr, "[threads] too many threads\n");
    exit(8);
  }
  Thread* h = &t[nthreads[c->id]++];
  memset(h, 0, sizeof *h);
  h->ctx = ctx;
  return h;
}

//OS_InitContext: the game prepares a new thread in this context. If there was a
//previous thread with the same context (destroyed), its fiber is no longer valid.
void hle_init_context(Cpu* c)
{
  uint32_t ctx = c->r[0];
  Thread* t = threads[c->id];
  for (int i = 0; i < nthreads[c->id]; i++)
  {
    if (t[i].ctx != ctx)
      continue;
    //the old fiber is not deleted (it could be the main thread's): it is only forgotten
    t[i].fiber = NULL;
    t[i].stored = 0;
  }
}

void** hle_ctx_buf(Cpu* c, uint32_t ctx)
{
  (void)c;
  (void)ctx;
  return NULL; //setjmp is no longer used (see enter())
}

//save and load the state of the divider and square root (CP_SaveContext / CP_RestoreContext of the
//game). No interrupts meanwhile, as on the console: if one arrived in here (waiting for the
//divider) and switched thread, a half loaded context was saved and that thread later came back
//with the stack of another (with the 8 DWC threads in the CWF match search: "missing code" at
//0200DF24)
static void call(Cpu* c, uint32_t func, uint32_t r0)
{
  if (!func)
    return;
  uint32_t lr = c->r[14];
  c->r[0] = r0;
  g_no_irq[c->id]++;
  cpu_call(c, func);
  g_no_irq[c->id]--;
  c->r[14] = lr;
}

//Context format (same as the SDK): +0 cpsr, +4 r0-r12, +38 sp, +3C lr,
//+40 pc+4, +44 sp of the supervisor mode, +48 divider/root state (ARM9 only).
void hle_save_context(Cpu* c)
{
  uint32_t ctx = c->r[0];
  call(c, g_so[c->id].cp_save, ctx + 0x48);
  uint32_t r[13];
  memcpy(r, c->r, sizeof r);
  bwr32(c, ctx, cpu_get_cpsr(c));
  for (int i = 0; i < 13; i++)
    bwr32(c, ctx + 4 + i * 4, i == 0 ? 1 : r[i]);
  bwr32(c, ctx + 0x38, c->r[13]);
  bwr32(c, ctx + 0x3C, c->r[14]);
  bwr32(c, ctx + 0x40, c->r[14] + 4);
  bwr32(c, ctx + 0x44, c->bank_r13[3]);
  Thread* h = search(c, ctx);
  h->fiber = GetCurrentFiber();
  h->stored = 1;
  c->r[0] = 0;
}

static void restore(Cpu* c, uint32_t ctx)
{
  call(c, g_so[c->id].cp_restore, ctx + 0x48);
  cpu_set_cpsr(c, brd32(c, ctx));
  for (int i = 0; i < 13; i++)
    c->r[i] = brd32(c, ctx + 4 + i * 4);
  c->r[13] = brd32(c, ctx + 0x38);
  c->r[14] = brd32(c, ctx + 0x3C);
  c->bank_r13[3] = brd32(c, ctx + 0x44);
}

//on the owning fiber: continue where the context was saved. In the SDK, what comes
//after OS_LoadContext is the same as when OS_SaveContext returns 1
//(pop registers from the stack and return), so it is enough to restore the
//registers and return from OS_LoadContext.
static void enter(Cpu* c)
{
  restore(c, loading[c->id]);
  c->r[0] = 1;
}

typedef struct
{
  Cpu* c;
  Thread* h;
} Boot;

static void WINAPI fiber_new(void* p)
{
  Boot* a = p;
  Cpu* c = a->c;
  Thread* h = a->h;
  free(a);
  restore(c, h->ctx);
  uint32_t pc = brd32(c, h->ctx + 0x40) - 4;
  if (brd32(c, h->ctx) & 0x20)
    pc |= 1; //the thread starts in Thumb
  uint32_t output = c->r[14];
  cpu_call(c, pc);
  cpu_call(c, output); //the thread function ended: goes to OS_ExitThread
  fprintf(stderr, "[threads] a thread ended without switching to another\n");
  exit(8);
}

void hle_load_context(Cpu* c)
{
  uint32_t ctx = c->r[0];
  Thread* h = search(c, ctx);
  loading[c->id] = ctx;
  if (h->stored && h->fiber == GetCurrentFiber())
  {
    enter(c);
    return;
  }
  if (!h->fiber)
  {
    Boot* a = malloc(sizeof *a);
    a->c = c;
    a->h = h;
    h->fiber = CreateFiberEx(64 * 1024, 64 * 1024 * 1024, 0, fiber_new, a);
    if (!h->fiber)
    {
      fprintf(stderr, "[threads] could not create the fiber\n");
      exit(8);
    }
  }
  SwitchToFiber(h->fiber);
  enter(c); //someone has loaded a context of this fiber
}
