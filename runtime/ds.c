//DS hardware for the recompiled code of both processors.
//Both processors take turns in the same Windows thread (each in its
//fiber): every certain "budget" (c->cycles) the translated code calls
//cpu_tick, which looks at the clock, delivers interrupts and lets the other one run.
//So there are no races between ARM9 and ARM7.
//Time is the PC's: one VBlank every 1/60 s, timers at 33.5 MHz.
#include "compat.h" //before cpu.h: its R macro clashes with windows.h
#include "ds.h"
#include "region.h"
#include "video.h"
#include "gpu3d.h"
#include "log.h"

//sound.c
void sound_init(void);
extern volatile uint32_t g_snd_gen[16];
uint8_t* ds_io7(void);
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

Cpu g_cpu9, g_cpu7;
uint8_t g_wramcnt = 3;
SoConfig g_so[2];
int g_no_irq[2];

#define CLOCK 33513982.0 //cycles per second of the DS bus
#define CYCLES_LINE 2130
#define LINES 263
#define CYCLES_FRAME (CYCLES_LINE * LINES)
#define TURN 20000 //budget between tick and tick

typedef struct
{
  uint8_t io[0x10000]; //registers stored as they are written
  uint32_t ie, if_, ime;
  uint16_t dispstat;
  struct
  {
    uint16_t reload, ctl;
    uint64_t start; //cycle when it started
    uint64_t overflows; //overflows already announced
    uint32_t pend; //interrupts still to deliver (if it fired several times between turns)
  } tm[4];
  uint16_t sync; //IPCSYNC: what this processor writes
  uint16_t fifocnt;
  uint32_t queue[16]; //input FIFO (what the other one sends)
  int n;
  uint32_t last; //last value read from the FIFO
  int stopped; //waiting for an interrupt
  void* fiber; //where this processor continues when its turn comes
} Hw;

static Hw hw[2];
static LARGE_INTEGER t0, freq;
static uint64_t frame_seen; //last VBlank delivered
static int line_seen[2];

static Cpu* cpu_of(int id)
{
  return id ? &g_cpu7 : &g_cpu9;
}

static uint64_t now_ms(void)
{
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return (uint64_t)((double)(t.QuadPart - t0.QuadPart) * CLOCK / (double)freq.QuadPart);
}

static uint32_t reg32(Hw* h, uint32_t a)
{
  uint32_t v;
  memcpy(&v, h->io + (a & 0xFFFF), 4);
  return v;
}
static void set_reg32(Hw* h, uint32_t a, uint32_t v)
{
  memcpy(h->io + (a & 0xFFFF), &v, 4);
}

static uint64_t now_ms(void);
static void irq(int id, uint32_t bits)
{
  hw[id].if_ |= bits;
}

//---- timers --------------------------------------------------------
static const int prescaler[4] = {1, 64, 256, 1024};

static uint64_t tm_ticks(Hw* h, int n, uint64_t t)
{
  uint16_t ctl = h->tm[n].ctl;
  if (!(ctl & 0x80))
    return 0;
  if ((ctl & 4) && n > 0)
  { //cascade: counts overflows of the previous one
    uint64_t p = 0x10000 - h->tm[n - 1].reload;
    return p ? tm_ticks(h, n - 1, t) / p : 0;
  }
  return (t - h->tm[n].start) / prescaler[ctl & 3];
}

static uint16_t tm_value(Hw* h, int n, uint64_t t)
{
  if (!(h->tm[n].ctl & 0x80))
    return h->tm[n].reload;
  uint64_t p = 0x10000 - h->tm[n].reload;
  return (uint16_t)(h->tm[n].reload + tm_ticks(h, n, t) % p);
}

static void tm_control(Hw* h, int n, uint16_t v, uint64_t t)
{
  if ((v & 0x80) && !(h->tm[n].ctl & 0x80))
  {
    h->tm[n].start = t;
    h->tm[n].overflows = 0;
  }
  h->tm[n].ctl = v;
}

//---- DMA -------------------------------------------------------------------
static void dma_run(int id, int n)
{
  Hw* h = &hw[id];
  Cpu* c = cpu_of(id);
  uint32_t base = 0x040000B0 + 12 * n;
  uint32_t src = reg32(h, base), dst = reg32(h, base + 4), cnt = reg32(h, base + 8);
  uint32_t num = cnt & (id ? 0xFFFF : 0x1FFFFF);
  if (num == 0)
    num = id ? 0x10000 : 0x200000;
  int width = (cnt >> 26) & 1 ? 4 : 2;
  int dctl = (cnt >> 21) & 3, sctl = (cnt >> 23) & 3;
  int32_t ds = dctl == 1 ? -width : dctl == 2 ? 0 : width;
  int32_t ss = sctl == 1 ? -width : sctl == 2 ? 0 : width;
  for (uint32_t i = 0; i < num; i++)
  {
    if (width == 4)
      bwr32(c, dst, brd32(c, src));
    else
      bwr16(c, dst, brd16(c, src));
    src += ss;
    dst += ds;
  }
  set_reg32(h, base, src);
  if (dctl != 3)
    set_reg32(h, base + 4, dst);
  int when = id ? (cnt >> 28) & 3 : (cnt >> 27) & 7;
  if (!(cnt & 0x02000000) || when == 0)
    set_reg32(h, base + 8, cnt & ~0x80000000u);
  if (cnt & 0x40000000)
    irq(id, 0x100u << n);
}

//checks if there are DMAs waiting for the moment `when` and does them
static void dma_moment(int id, int when)
{
  for (int n = 0; n < 4; n++)
  {
    uint32_t cnt = reg32(&hw[id], 0x040000B8 + 12 * n);
    int t = id ? (cnt >> 28) & 3 : (cnt >> 27) & 7;
    if ((cnt & 0x80000000) && t == when)
      dma_run(id, n);
  }
}

static void dma_written(int id, int n)
{
  uint32_t cnt = reg32(&hw[id], 0x040000B8 + 12 * n);
  if (!(cnt & 0x80000000))
    return;
  int t = id ? (cnt >> 28) & 3 : (cnt >> 27) & 7;
  //immediate, and the one of the 3D FIFO (which for now swallows everything)
  if (t == 0 || (!id && t == 7))
    dma_run(id, n);
}

//---- time: VBlank, lines and timers ----
static void mic_update(uint64_t t);

static void update(void)
{
  uint64_t t = now_ms();
  uint64_t frame = t / CYCLES_FRAME;
  mic_update(t);
  int line = (int)((t / CYCLES_LINE) % LINES);
  if ((reg32(&hw[0], 0x04000600) >> 30) & 3)
    irq(0, 1u << 21); //the 3D FIFO is always empty
  for (int id = 0; id < 2; id++)
  {
    Hw* h = &hw[id];
    //VCOUNT match
    int goal = (h->dispstat >> 8) | ((h->dispstat & 0x80) << 1);
    if ((h->dispstat & 0x20) && line_seen[id] != line)
    {
      int a = line_seen[id], b = line;
      if ((a < goal && goal <= b) || (b < a && (goal > a || goal <= b)))
        irq(id, 4);
    }
    line_seen[id] = line;
    //timers
    for (int n = 0; n < 4; n++)
    {
      if (!(h->tm[n].ctl & 0x80))
        continue;
      uint64_t p = 0x10000 - h->tm[n].reload;
      uint64_t d = tm_ticks(h, n, t) / p;
      if (d > h->tm[n].overflows)
      {
        if (h->tm[n].ctl & 0x40)
          h->tm[n].pend += (uint32_t)(d - h->tm[n].overflows);
        if (h->tm[n].pend > 16)
          h->tm[n].pend = 16; //without catching up forever
        h->tm[n].overflows = d;
      }
      //one by one: the next when the game has served the previous
      if (h->tm[n].pend && !(h->if_ & (8u << n)))
      {
        irq(id, 8u << n);
        h->tm[n].pend--;
      }
    }
  }
  if (frame != frame_seen)
  { //new VBlank: draw the screens
    frame_seen = frame;
    log_note_game_frame();
    static uint32_t up[256 * 192], bottom[256 * 192];
    gx_vblank(hw[0].io); //if the game asked for SWAP_BUFFERS, its 3D scene is painted
    { //FPS counter (screen and 3D scenes finished by the game), every second
      static uint64_t t_fps, frames_fps, scenes_before;
      frames_fps++;
      if (t >= t_fps + (uint64_t)CLOCK)
      {
        extern int window_title_fps(int fps, int fps3d);
        int shown = window_title_fps((int)frames_fps, (int)(gx_frames() - scenes_before));
        log_second((int)frames_fps, shown);
        scenes_before = gx_frames();
        frames_fps = 0;
        t_fps = t;
      }
    }
    //the image is built on another thread from a snapshot of the VRAM (video_draw_separate):
    //building it here took up to 6 ms with the 3D at 4x and the ARM7 skipped sound steps (slow
    //music). With screen capture on it is built right here: the game reads what was captured in
    //the next frame.
    if (hw[0].io[0x67] & 0x80)
    {
      video_wait_assembly();
      video_draw(hw[0].io, up, bottom);
      hw[0].io[0x67] &= 0x7F; //the capture (DISPCAPCNT) lasts one frame and turns itself off
      window_show(up, bottom);
    }
    else
    {
      video_draw_separate(hw[0].io);
    }
    extern void name_per_frame(void);
    //the launcher name in the save (main.c)
    //launcher name written into the save, main.c
    name_per_frame();
    save_per_frame();
    window_messages();
    for (int id = 0; id < 2; id++)
    {
      if (hw[id].dispstat & 8)
        irq(id, 1);
      dma_moment(id, 1);
    }
  }
}

//what OS_IrqHandler_ThreadSwitch of the SDK does before switching thread:
//wake the threads that were waiting for an interrupt.
static void wake_threads(Cpu* c)
{
  SoConfig* s = &g_so[c->id];
  if (!s->queue_irq)
    return;
  uint32_t t = brd32(c, s->queue_irq);
  if (!t)
    return;
  while (t)
  {
    bwr32(c, t + s->off_state, 1);
    bwr32(c, t + s->off_link, 0);
    bwr32(c, t + s->off_link + 4, 0);
    uint32_t sig = brd32(c, t + s->off_sig);
    bwr32(c, t + s->off_sig, 0);
    t = sig;
  }
  bwr32(c, s->queue_irq, 0);
  bwr32(c, s->queue_irq + 4, 0);
  bwr16(c, s->flag_resched, 1);
}

static int irq_list(int id)
{
  return (hw[id].ime & 1) && (hw[id].ie & hw[id].if_);
}

//calls the interrupt handler of the game as the BIOS would.
//`forzar` (force): the BIOS (IntrWait) accepts them even if the CPSR has them disabled.
static void deliver(Cpu* c, int forzar)
{
  if (!(hw[c->id].ie & hw[c->id].if_))
    return;
  if (g_no_irq[c->id])
    return; //saving or loading a thread context (threads.c): later
  if (!forzar && (!(hw[c->id].ime & 1) || (c->ctl & 0x80)))
    return;
  Cpu before = *c;
  uint32_t cpsr = cpu_get_cpsr(c);
  cpu_set_cpsr(c, (cpsr & ~0x3Fu) | 0x92); //mode IRQ, interrupciones apagadas, ARM
  c->spsr = cpsr;
  c->r[13] -= 24; //the BIOS saves r0-r3, r12, lr on the IRQ stack
  c->r[14] = 0xFFFF0000;
  uint32_t vector = c->id ? rd32_7(0x0380FFFC) : rd32_9(g_dtcm_base + 0x3FFC);
  cpu_call(c, vector);
  wake_threads(c);
  uint32_t func = c->func;
  *c = before;
  c->func = func;
  //pending thread switch: done already outside the interrupt, but only on returning to the thread.
  //If this interrupt arrived inside another (the Multiplayer wifi reopens them in its notices),
  //the thread would be switched in the middle of the outer one and its IRQ stack would be
  //overwritten. The outer one will do it on finishing
  SoConfig* s = &g_so[c->id];
  int nested = (cpu_get_cpsr(c) & 0x1F) == 0x12;
  if (!nested && s->flag_resched && brd16(c, s->flag_resched))
  {
    bwr16(c, s->flag_resched, 0);
    Cpu store = *c;
    cpu_call(c, s->reschedule);
    func = c->func;
    *c = store;
    c->func = func;
  }
}

//---- turns between processors ----
static void change(Cpu* c)
{
  Hw *yo = &hw[c->id], *other = &hw[!c->id];
  yo->fiber = GetCurrentFiber();
  SwitchToFiber(other->fiber);
}

static int other_can(Cpu* c)
{
  Hw* o = &hw[!c->id];
  return o->fiber && (!o->stopped || (o->ie & o->if_));
}

void cpu_tick(Cpu* c)
{
  c->cycles = TURN;
  update();
  if (irq_list(c->id))
    deliver(c, 0);
  if (other_can(c))
    change(c);
}

void ds_yield(Cpu* c)
{
  update();
  if (other_can(c))
    change(c);
}

static void wait_event(Cpu* c)
{
  Hw* h = &hw[c->id];
  h->stopped = 1;
  for (;;)
  {
    update();
    if (h->ie & h->if_)
      break;
    if (other_can(c))
    {
      change(c);
      continue;
    }
    //both stopped: sleep a little (1 ms) until the next event
    Sleep(1);
  }
  h->stopped = 0;
}

void ds_wait(Cpu* c, int swi)
{
  if (swi == 4 || swi == 5)
  { //IntrWait / VBlankIntrWait
    uint32_t chk = c->id ? 0x0380FFF8 : g_dtcm_base + 0x3FF8;
    uint32_t mask = swi == 5 ? 1 : c->r[1];
    int discard = swi == 5 ? 1 : (int)c->r[0];
    hw[c->id].ime = 1;
    if (discard)
      bwr32(c, chk, brd32(c, chk) & ~mask);
    for (;;)
    {
      uint32_t f = brd32(c, chk);
      if (f & mask)
      {
        bwr32(c, chk, f & ~mask);
        return;
      }
      wait_event(c);
      deliver(c, 1);
    }
  }
  wait_event(c); //Halt
  deliver(c, 0);
}

//while the game records, the samples of the PC microphone are written in its buffer at the pace it
//asked for and the address of the last one is left at 0x027FFF90 (the game looks for it there).
//When the buffer fills, the ARM9 is told (command 0x51) and, if it is a loop, it goes back to the
//start.
void mic_pc_open(void);
void mic_pc_close(void);
int16_t mic_pc_show(void);
static struct
{
  int active, type, loop;
  uint32_t buf, nbytes, rate, pos;
  uint64_t start, done;
} mic;

static void mic_a_arm9(uint32_t v)
{ //a message from the "ARM7" to the ARM9 through the FIFO
  Hw* h = &hw[0];
  if (h->n >= 16)
    return;
  h->queue[h->n++] = v;
  if (h->n == 1 && (h->fifocnt & 0x400))
    irq(0, 1u << 18);
}

static void mic_start(uint32_t buf, uint32_t nbytes, uint32_t rate, int type, int loop)
{
  if (rate < 100 || !nbytes || (buf & 0xFF000000) != 0x02000000)
    return;
  mic.buf = buf;
  mic.nbytes = nbytes;
  mic.rate = rate;
  mic.type = type;
  mic.loop = loop;
  mic.pos = 0;
  mic.start = now_ms();
  mic.done = 0;
  mic.active = 1;
  mic_pc_open();
}

static void mic_stop(void)
{
  mic.active = 0;
  mic_pc_close();
}

static void mic_update(uint64_t t)
{
  if (!mic.active)
    return;
  uint64_t due = (t - mic.start) / mic.rate;
  uint64_t n = due - mic.done;
  if (n > mic.nbytes)
    n = mic.nbytes; //after a pause, no more than one buffer
  mic.done = due;
  int two = mic.type & 1, with_sign = (mic.type & 2) != 0; //12 bits (in 16) or 8. Signed or not
  for (uint64_t k = 0; k < n && mic.active; k++)
  {
    int16_t s = mic_pc_show();
    uint32_t a = (mic.buf + mic.pos) & 0x3FFFFF;
    if (two)
    {
      uint16_t v = (uint16_t)(with_sign ? (uint16_t)s & 0xFFF0 : ((uint16_t)(s + 32768)) & 0xFFF0);
      memcpy(g_main + a, &v, 2);
    }
    else
    {
      int b = s >> 8;
      if (b < -127)
        b = -127; //the DS microphone does not give -128: the game demands a peak of exactly 127
      g_main[a] = with_sign ? (uint8_t)b : (uint8_t)(b + 128);
    }
    uint32_t last = mic.buf + mic.pos;
    memcpy(g_main + (0x027FFF90 & 0x3FFFFF), &last, 4);
    mic.pos += two ? 2 : 1;
    if (mic.pos >= mic.nbytes)
    {
      mic_a_arm9((0x3000000u | (0xD1u << 8)) << 6 | 9); //buffer full
      if (mic.loop)
        mic.pos = 0;
      else
        mic_stop();
    }
  }
}

//---- IPC: IPCSYNC and the two FIFOs ----

static uint32_t ipc_rd(int id, uint32_t a)
{
  Hw *h = &hw[id], *o = &hw[!id];
  //whoever looks at the IPC is usually waiting for the other processor: hand over the turn now
  if (cpu_of(id)->cycles > 200)
    cpu_of(id)->cycles = 200;
  switch (a)
  {
  case 0x04000180:
    return (h->sync & 0x6F00) | ((o->sync >> 8) & 15);
  case 0x04000184:
    return (h->fifocnt & 0x8404) | (o->n == 0 ? 1 : 0) | (o->n == 16 ? 2 : 0) |
           (h->n == 0 ? 0x100 : 0) | (h->n == 16 ? 0x200 : 0);
  case 0x04100000:
    if (h->n && (h->fifocnt & 0x8000))
    {
      h->last = h->queue[0];
      memmove(h->queue, h->queue + 1, --h->n * 4);
      if (h->n == 0 && (o->fifocnt & 4))
        irq(!id, 1u << 17); //to the other: send empty
    }
    return h->last;
  }
  return 0;
}

static void ipc_wr(int id, uint32_t a, uint32_t v)
{
  Hw *h = &hw[id], *o = &hw[!id];
  if (cpu_of(id)->cycles > 200)
    cpu_of(id)->cycles = 200; //the other one has to react: hand over the turn soon
  switch (a)
  {
  case 0x04000180:
    h->sync = (uint16_t)(v & 0x4F00);
    if ((v & 0x2000) && (o->sync & 0x4000))
      irq(!id, 1u << 16);
    break;
  case 0x04000184:
    if (v & 8)
      o->n = 0; //empty what was sent
    if ((v & 4) && !(h->fifocnt & 4) && o->n == 0)
      irq(id, 1u << 17);
    if ((v & 0x400) && !(h->fifocnt & 0x400) && h->n)
      irq(id, 1u << 18);
    h->fifocnt = (uint16_t)((h->fifocnt & ~0x8404u) | (v & 0x8404));
    break;
  case 0x04000188:
    if (!(h->fifocnt & 0x8000) || o->n == 16)
      break;
    if (id == 0 && (v & 31) == 9)
    { //service 9: microphone
      //the microphone commands are answered by the port ("done") without passing them to the ARM7:
      //the ARM7 answered "busy" on stopping and the Balloon Battle hung. Without a PC microphone
      //yet, blowing does nothing. Packet: bit 25 = first word (command in bits 8-15), bit 24 =
      //last. Reply = (3 << 24)
      //|((command | 0x80) << 8) | result (0 = fine). Record (command 0x41): first word = sample
      //type | loop (0x10). Then words (index << 16) | 16 bits: 1-2 buffer, 3-4 size, 5-6 rhythm
      //(cycles per sample). Stop: 0x42. The recording is done by mic_update with the PC microphone
      //(microphone.c).
      static uint32_t command_mic, param_mic[8];
      uint32_t d = v >> 6;
      if (d & 0x2000000)
      {
        command_mic = (d >> 8) & 0xFF;
        memset(param_mic, 0, sizeof param_mic);
        param_mic[0] = d & 0xFF;
      }
      else
      {
        param_mic[(d >> 16) & 7] = d & 0xFFFF;
      }
      if (d & 0x1000000)
      {
        param_mic[(d >> 16) & 7] = d & 0xFFFF;
        if (command_mic == 0x41)
          mic_start(param_mic[1] << 16 | param_mic[2], param_mic[3] << 16 | param_mic[4],
                    param_mic[5] << 16 | param_mic[6], (int)(param_mic[0] & 0x0F),
                    (param_mic[0] & 0x10) != 0);
        else if (command_mic == 0x42)
          mic_stop();
        mic_a_arm9((0x3000000u | ((command_mic | 0x80) << 8)) << 6 | 9);
      }
      break;
    }
    o->queue[o->n++] = v;
    if (o->n == 1 && (o->fifocnt & 0x400))
      irq(!id, 1u << 18);
    break;
  }
}

//---- ARM9 divider and square root ----
static void divider(Hw* h)
{
  uint16_t cnt = (uint16_t)reg32(h, 0x04000280);
  int64_t num, den;
  int64_t n64, d64;
  memcpy(&n64, h->io + 0x290, 8);
  memcpy(&d64, h->io + 0x298, 8);
  switch (cnt & 3)
  {
  case 0:
    num = (int32_t)n64;
    den = (int32_t)d64;
    break;
  case 1:
    num = n64;
    den = (int32_t)d64;
    break;
  default:
    num = n64;
    den = d64;
    break;
  }
  int64_t q, r;
  if (den == 0)
  {
    q = num < 0 ? 1 : -1;
    r = num;
  }
  else if (num == INT64_MIN && den == -1)
  {
    q = num;
    r = 0;
  }
  else
  {
    q = num / den;
    r = num % den;
  }
  memcpy(h->io + 0x2A0, &q, 8);
  memcpy(h->io + 0x2A8, &r, 8);
  cnt = (uint16_t)((cnt & 3) | (d64 == 0 ? 0x4000 : 0));
  memcpy(h->io + 0x280, &cnt, 2);
}

static void root(Hw* h)
{
  uint16_t cnt = (uint16_t)reg32(h, 0x040002B0);
  uint64_t v;
  memcpy(&v, h->io + 0x2B8, 8);
  if (!(cnt & 1))
    v &= 0xFFFFFFFF;
  uint64_t r = 0, bit = 1ULL << 62;
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
  set_reg32(h, 0x040002B4, (uint32_t)r);
  cnt &= 1;
  memcpy(h->io + 0x2B0, &cnt, 2);
}

//---- card (slot 1) ----------------------------------------------------------
static const uint8_t* rom;
static size_t rom_size;
static struct
{
  uint32_t ctrl;
  uint8_t cmd[8];
  uint32_t pos, total, dir, mode;
  uint16_t spicnt;
} card;

void ds_set_rom(const uint8_t* r, size_t n)
{
  rom = r;
  rom_size = n;
}

static void card_start(int id, uint32_t v)
{
  card.ctrl = v;
  if (!(v & 0x80000000))
    return;
  int bs = (v >> 24) & 7;
  card.total = bs == 0 ? 0 : bs == 7 ? 4 : 0x100u << bs;
  card.pos = 0;
  card.mode = card.cmd[0];
  card.dir = (uint32_t)card.cmd[1] << 24 | card.cmd[2] << 16 | card.cmd[3] << 8 | card.cmd[4];
  if (card.mode == 0xB7 && card.dir < 0x8000)
    card.dir = 0x8000 + (card.dir & 0x1FF);
  if (card.total == 0)
  {
    card.ctrl &= ~0x80000000u;
    if (card.spicnt & 0x4000)
      irq(id, 1u << 19);
    return;
  }
  card.ctrl |= 0x00800000; //there is data
  dma_moment(id, id ? 2 : 5); //DMA waiting for the card
}

static uint32_t card_data(int id)
{
  if (!(card.ctrl & 0x00800000))
    return 0;
  uint32_t v = 0xFFFFFFFF;
  if (card.mode == 0xB7)
  {
    uint32_t a = card.dir + card.pos;
    if (rom && a + 4 <= rom_size)
      memcpy(&v, rom + a, 4);
  }
  else if (card.mode == 0xB8)
  {
    v = 0x00001FC2; //chip ID (32 MB)
  }
  card.pos += 4;
  if (card.pos >= card.total)
  {
    card.ctrl &= ~0x80800000u;
    if (card.spicnt & 0x4000)
      irq(id, 1u << 19);
  }
  return v;
}

//EEPROM or serial flash. It is saved in saves/save.sav.
//serial flash. kept in saves/save.sav
static uint8_t save[0x80000];
static uint32_t save_size = 0x40000; //MKDS: 256 KB serial flash
static int save_flash = 1; //1 = flash (3 byte addresses)
static struct
{
  int pos;
  uint8_t cmd, data, wel;
  uint32_t dir;
  int written;
} sav;

//how the save file is kept
//the game writes flash in many small commands. a command only marks the save dirty
//the file is written once per half second at most, on request from the menu and on exit
//each write goes to a temp file first so a crash never leaves a half written save
#define SAVE_FOLDER "saves"
#define SAVE_FILE SAVE_FOLDER "/save.sav"
#define SAVE_TEMPORAL SAVE_FOLDER "/save.sav.tmp"
#define SAVE_FILE_OLD                                                                              \
  "save.sav" //where older versions kept it: read once if the new file is missing
#define SAVE_WAIT_FRAMES 30 //frames between a dirty mark and the disk write
static volatile int save_dirty; //1 = memory is newer than the file
static int save_wait; //frames since the save became dirty

static void save_load(void)
{
  CreateDirectoryA(SAVE_FOLDER, NULL);
  memset(save, 0xFF, sizeof save);
  FILE* f = fopen(SAVE_FILE, "rb");
  if (!f)
    f = fopen(SAVE_TEMPORAL, "rb"); //crash between remove and rename left only the temp file
  if (!f)
    f = fopen(SAVE_FILE_OLD, "rb");
  if (f)
  {
    fread(save, 1, sizeof save, f);
    fclose(f);
  }
}

//write the whole save through the temp file. returns 1 when the new file is in place
static int save_write(void)
{
  CreateDirectoryA(SAVE_FOLDER, NULL);
  FILE* f = fopen(SAVE_TEMPORAL, "wb");
  if (!f)
    return 0;
  size_t n = fwrite(save, 1, save_size, f);
  if (fclose(f) != 0 || n != save_size)
  {
    remove(SAVE_TEMPORAL);
    return 0;
  }
  remove(SAVE_FILE); //rename does not replace an existing file on Windows
  return rename(SAVE_TEMPORAL, SAVE_FILE) == 0;
}

//write now if there is anything to write. called on exit and by save_per_frame
void save_write_pending(void)
{
  if (!save_dirty)
    return;
  static int warned;
  LARGE_INTEGER write_start, write_end;
  QueryPerformanceCounter(&write_start);
  int written = save_write();
  QueryPerformanceCounter(&write_end);
  log_note_save(write_end.QuadPart - write_start.QuadPart, written);
  if (written)
  {
    save_dirty = 0;
    warned = 0;
    return;
  }
  if (!warned)
  {
    fprintf(stderr, "[game] could not write " SAVE_FILE "\n");
    warned = 1;
  }
}

//menu button save now. the game thread does the write on the next frame

//once per frame from the main loop
void save_per_frame(void)
{
  if (!save_dirty)
  {
    save_wait = 0;
    return;
  }
  if (++save_wait >= SAVE_WAIT_FRAMES)
  {
    save_wait = 0;
    save_write_pending();
  }
}

static uint8_t sav_byte(uint8_t b)
{
  int pos = sav.pos++;
  if (pos == 0)
  {
    sav.cmd = b;
    sav.dir = 0;
    if (b == 0x06)
      sav.wel = 1;
    if (b == 0x04)
      sav.wel = 0;
    if (b == 0x0A || b == 0xDB || b == 0xD8 || b == 0x9F)
      save_flash = 1;
    return 0xFF;
  }
  int ad = save_flash ? 3 : 2; //address bytes
  switch (sav.cmd)
  {
  case 0x05:
    return (uint8_t)(sav.wel << 1); //status: free. Bit 1 = write allowed
  case 0x03:
  case 0x0B: //leer
    if (pos <= ad)
    {
      sav.dir = (sav.dir << 8) | b;
      return 0xFF;
    }
    if (sav.cmd == 0x0B && pos == ad + 1)
      return 0xFF;
    return save[sav.dir++ % save_size];
  case 0x02:
  case 0x0A: //write_out
    if (pos <= ad)
    {
      sav.dir = (sav.dir << 8) | b;
      return 0xFF;
    }
    if (sav.wel)
    {
      save[sav.dir % save_size] = b;
      sav.written = 1;
    }
    sav.dir++;
    return 0xFF;
  case 0x9F:
    return pos == 1 ? 0x20 : pos == 2 ? 0x40 : 0x12; //ID (flash)
  }
  return 0xFF;
}

static void sav_data(uint8_t v)
{
  if (!(card.spicnt & 0x8000) || !(card.spicnt & 0x2000))
    return; //bit 13 = 1: mode serie (stored)
  sav.data = sav_byte(v);
  if (!(card.spicnt & 0x40))
  { //end of the command (without holding the selection)
    if (sav.written)
    {
      save_dirty = 1;
      sav.written = 0;
      sav.wel = 0;
    } //written to disk later by save_per_frame
    sav.pos = 0;
  }
}

//---- ARM7 SPI: power manager, firmware and touch screen ----
static uint8_t firmware[0x40000];
static struct
{
  uint16_t cnt;
  uint8_t data;
  int pos;
  uint8_t cmd;
  uint32_t dir;
  uint8_t wel;
} spi;
static uint8_t pm_regs[8] = {0x0C, 0x00, 0x01, 0x00, 0x01};
static uint8_t tsc_out[2];
static int tsc_n;

static uint8_t spi_byte(uint8_t b)
{
  int dev = (spi.cnt >> 8) & 3;
  int pos = spi.pos++;
  if (dev == 0)
  { //power manager: index (bit 7 = read) and then data
    if (pos == 0)
    {
      spi.cmd = b;
      return 0;
    }
    if (spi.cmd & 0x80)
      return pm_regs[spi.cmd & 7];
    pm_regs[spi.cmd & 7] = b;
    return 0;
  }
  if (dev == 1)
  { //firmware flash memory (ST M45PE20)
    if (pos == 0)
    {
      spi.cmd = b;
      spi.dir = 0;
      if (b == 0x06)
        spi.wel = 1; //WREN: permite write_out
      if (b == 0x04)
        spi.wel = 0; //WRDI
      return 0;
    }
    switch (spi.cmd)
    {
    case 0x03:
    case 0x0B:
      if (pos <= 3)
      {
        spi.dir = (spi.dir << 8) | b;
        return 0;
      }
      if (spi.cmd == 0x0B && pos == 4)
        return 0;
      return firmware[spi.dir++ & 0x3FFFF];
    case 0x05:
      return spi.wel << 1; //status: free. Bit 1 = write allowed
    case 0x9F:
      return pos == 1 ? 0x20 : pos == 2 ? 0x40 : 0x12;
    case 0x0A:
    case 0x02: //write page
      if (pos <= 3)
      {
        spi.dir = (spi.dir << 8) | b;
        return 0;
      }
      if (spi.wel)
      {
        uint32_t d = spi.dir & 0x3FF00 | (spi.dir & 0xFF);
        firmware[d] = b;
      }
      spi.dir = (spi.dir & ~0xFFu) | ((spi.dir + 1) & 0xFF);
      return 0;
    case 0xDB:
    case 0xD8: //erase page / sector
      if (pos <= 3)
      {
        spi.dir = (spi.dir << 8) | b;
      }
      if (pos == 3 && spi.wel)
      {
        memset(firmware + (spi.dir & (spi.cmd == 0xDB ? 0x3FF00 : 0x30000)), 0xFF,
               spi.cmd == 0xDB ? 0x100 : 0x10000);
      }
      return 0;
    }
    return 0xFF;
  }
  if (dev == 2)
  { //touch screen (TSC2046): without touching, all 0
    uint8_t r = tsc_n ? tsc_out[0] : 0;
    if (tsc_n)
    {
      tsc_out[0] = tsc_out[1];
      tsc_n--;
    }
    if (b & 0x80)
    {
      //channel 5 = X, 1 = Y (the firmware calibration is pixel * 16), 3/4 = pressure
      int channel = (b >> 4) & 7;
      uint16_t val = 0;
      if (g_stylus)
      {
        if (channel == 5)
          val = (uint16_t)(g_stylus_x * 16);
        else if (channel == 1)
          val = (uint16_t)(g_stylus_y * 16);
        else if (channel == 3)
          val = 0x200;
        else if (channel == 4)
          val = 0xE00;
      }
      tsc_out[0] = (uint8_t)(val >> 5);
      tsc_out[1] = (uint8_t)(val << 3);
      tsc_n = 2;
    }
    return r;
  }
  return 0xFF;
}

//Standard CRC-16 (0xA001), the same result as the BIOS (SWI 0x0E)
static uint16_t crc16(uint16_t crc, const uint8_t* p, size_t n)
{
  uint32_t c = crc;
  for (size_t i = 0; i < n; i++)
  {
    c ^= p[i];
    for (int j = 0; j < 8; j++)
      c = (c & 1) ? (c >> 1) ^ 0xA001 : c >> 1;
  }
  return (uint16_t)c;
}

//an invented firmware with the console settings (name, language, touch)
static void prepare_firmware(void)
{
  memset(firmware, 0xFF, sizeof firmware);
  memset(firmware, 0, 0x200);
  firmware[0x1D] = 0x20; //DS Lite
  firmware[0x20] = 0xC0;
  firmware[0x21] = 0x7F; //settings at 0x3FE00 (0x7FC0 * 8)
  static const uint8_t mac[6] = {0x00, 0x09, 0xBF, 0x12,
                                 0x34, 0x56}; //fixed: there is no network play in this version
  memcpy(firmware + 0x36, mac, 6);
  firmware[0x3C] = 0xFE;
  firmware[0x3D] = 0x3F; //canales wifi 1-13
  uint8_t u[0x100];
  memset(u, 0, sizeof u);
  u[0x00] = 5; //version
  u[0x02] = 11; //color favorito
  u[0x03] = 1;
  u[0x04] = 1; //birthday
  //console name: the player's (NAME in settings.ini). The game copies it to a
  //new save (the game shows it during the races)
  wchar_t label[11] = L"Jugador";
  const char* nom = getenv("DRIFT_NAME");
  if (nom && *nom)
  {
    memset(label, 0, sizeof label);
    MultiByteToWideChar(CP_UTF8, 0, nom, -1, label, 11);
    label[10] = 0;
  }
  int span = 0;
  for (; span < 10 && label[span]; span++)
    memcpy(u + 0x06 + span * 2, &label[span], 2);
  u[0x1A] = (uint8_t)span;
  //touch screen calibration: ADC = pixel * 16
  uint16_t cal[] = {32 * 16, 32 * 16};
  memcpy(u + 0x58, cal, 4);
  u[0x5C] = 32;
  u[0x5D] = 32;
  uint16_t cal2[] = {224 * 16, 160 * 16};
  memcpy(u + 0x5E, cal2, 4);
  u[0x62] = 224;
  u[0x63] = 160;
  //console language (the European game uses it): 1 English, 2 French, 3 German, 4 Italian, 5
  //Spanish. DRIFT_LANGUAGE (LANGUAGE in settings.ini). English by default
  int language = getenv("DRIFT_LANGUAGE") ? atoi(getenv("DRIFT_LANGUAGE")) : 1;
  if (language < 1 || language > 5)
    language = 1;
  u[0x64] = (uint8_t)(language | (3 << 4)); //and maximum brightness
  u[0x70] = 1; //save counter
  uint16_t crc = crc16(0xFFFF, u, 0x70);
  memcpy(u + 0x72, &crc, 2);
  memcpy(firmware + 0x3FE00, u, 0x100);
  u[0x70] = 0;
  crc = crc16(0xFFFF, u, 0x70);
  memcpy(u + 0x72, &crc, 2);
  memcpy(firmware + 0x3FF00, u, 0x100);
  //what the console boot leaves in RAM: settings and card data
  memcpy(g_main + 0x3FFC80, firmware + 0x3FE00, 0x70);
  uint32_t chip = 0x00001FC2;
  memcpy(g_main + 0x3FF800, &chip, 4);
  memcpy(g_main + 0x3FF804, &chip, 4);
  memcpy(g_main + 0x3FFC00, &chip, 4);
  memcpy(g_main + 0x3FFC04, &chip, 4);
  uint16_t boot = 1; //booted from card
  memcpy(g_main + 0x3FFC40, &boot, 2);
}

//---- RTC (clock): serial protocol through 0x04000138 ----
static struct
{
  uint8_t io, cmd, in, outbuf[8];
  int bit_in, pos_in, bit_out, pos_out;
  uint8_t state1, state2;
} rtc = {.state1 = 0x02};

static uint8_t bcd(int v)
{
  return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static void rtc_byte(uint8_t b)
{
  if (rtc.pos_in == 0)
  {
    if ((b & 0xF0) != 0x60 && (b & 0x0F) == 0x06)
    { //it arrived reversed
      uint8_t r = 0;
      for (int i = 0; i < 8; i++)
        r |= ((b >> i) & 1) << (7 - i);
      b = r;
    }
    rtc.cmd = b;
    memset(rtc.outbuf, 0, sizeof rtc.outbuf);
    if (b & 1)
    { //lectura
      time_t t = time(NULL);
      struct tm* f = localtime(&t);
      switch ((b >> 1) & 7)
      {
      case 0:
        rtc.outbuf[0] = rtc.state1;
        break;
      case 1:
        rtc.outbuf[0] = rtc.state2;
        break;
      case 2:
        rtc.outbuf[0] = bcd(f->tm_year % 100);
        rtc.outbuf[1] = bcd(f->tm_mon + 1);
        rtc.outbuf[2] = bcd(f->tm_mday);
        rtc.outbuf[3] = (uint8_t)f->tm_wday;
        rtc.outbuf[4] = bcd(f->tm_hour) | (f->tm_hour >= 12 ? 0x40 : 0);
        rtc.outbuf[5] = bcd(f->tm_min);
        rtc.outbuf[6] = bcd(f->tm_sec);
        break;
      case 3:
        rtc.outbuf[0] = bcd(f->tm_hour) | (f->tm_hour >= 12 ? 0x40 : 0);
        rtc.outbuf[1] = bcd(f->tm_min);
        rtc.outbuf[2] = bcd(f->tm_sec);
        break;
      }
    }
  }
  else if (!(rtc.cmd & 1))
  { //write
    if (((rtc.cmd >> 1) & 7) == 0)
      rtc.state1 = (rtc.state1 & 0xF0) | (b & 0x0E);
    if (((rtc.cmd >> 1) & 7) == 1)
      rtc.state2 = b;
  }
  rtc.pos_in++;
}

//bit 0 data, bit 1 clock, bit 2 select, bit 4 direction of the data (1 = the CPU writes)
static void rtc_wr(uint8_t v)
{
  uint8_t before = rtc.io;
  uint8_t data = (v & 0x10) ? (v & 1) : (rtc.io & 1);
  if (!(v & 4) || !(before & 4))
  { //not selected or just selected: start
    rtc.bit_in = rtc.pos_in = rtc.bit_out = rtc.pos_out = 0;
    rtc.in = 0;
  }
  else if (!(before & 2) && (v & 2))
  { //rising edge of the clock
    if (v & 0x10)
    {
      rtc.in |= (uint8_t)((v & 1) << rtc.bit_in);
      if (++rtc.bit_in == 8)
      {
        rtc_byte(rtc.in);
        rtc.in = 0;
        rtc.bit_in = 0;
      }
    }
    else
    {
      data = (rtc.outbuf[rtc.pos_out] >> rtc.bit_out) & 1;
      if (++rtc.bit_out == 8)
      {
        rtc.bit_out = 0;
        if (rtc.pos_out < 7)
          rtc.pos_out++;
      }
    }
  }
  rtc.io = (uint8_t)((v & ~1) | data);
}

//---- registers common to both processors ----
static int register_common(int id, uint32_t a, uint32_t* v)
{
  Hw* h = &hw[id];
  uint64_t t;
  switch (a & ~3u)
  {
  case 0x04000004:
  { //DISPSTAT + VCOUNT
    t = now_ms();
    int line = (int)((t / CYCLES_LINE) % LINES);
    int goal = (h->dispstat >> 8) | ((h->dispstat & 0x80) << 1);
    uint32_t ds = (h->dispstat & 0xFFB8) | (line >= 192 && line < 262 ? 1 : 0) |
                  ((t % CYCLES_LINE) > 1606 ? 2 : 0) | (line == goal ? 4 : 0);
    *v = ds | ((uint32_t)line << 16);
    return 1;
  }
  case 0x04000100:
  case 0x04000104:
  case 0x04000108:
  case 0x0400010C:
  {
    int n = (a >> 2) & 3;
    *v = tm_value(h, n, now_ms()) | ((uint32_t)h->tm[n].ctl << 16);
    return 1;
  }
  case 0x04000130:
    *v = g_keyinput | (reg32(h, 0x04000130) & 0xFFFF0000);
    return 1; //keys
  case 0x04000180:
  case 0x04000184:
  case 0x04100000:
    *v = ipc_rd(id, a & ~3u);
    return 1;
  case 0x040001A4:
    *v = card.ctrl;
    return 1;
  case 0x040001A0:
    *v = card.spicnt | (uint32_t)sav.data << 16;
    return 1;
  case 0x04100010:
    *v = card_data(id);
    return 1;
  case 0x04000208:
    *v = h->ime;
    return 1;
  case 0x04000210:
    *v = h->ie;
    return 1;
  case 0x04000214:
    *v = h->if_;
    return 1;
  }
  return 0;
}

static int write_common(int id, uint32_t a, uint32_t v, int bytes)
{
  Hw* h = &hw[id];
  uint32_t a4 = a & ~3u;
  //8/16 bit writes are combined with what was already there
  uint32_t old = 0, w;
  if (!register_common(id, a4, &old))
    old = reg32(h, a4);
  uint32_t shift = (a & 3) * 8, m = bytes == 4 ? 0xFFFFFFFF : ((1u << (bytes * 8)) - 1) << shift;
  w = (old & ~m) | ((v << shift) & m);
  switch (a4)
  {
  case 0x04000004:
    if (m & 0xFFFF)
      h->dispstat = (uint16_t)((h->dispstat & 7) | (w & 0xFFB8));
    return 1;
  case 0x04000100:
  case 0x04000104:
  case 0x04000108:
  case 0x0400010C:
  {
    int n = (a4 >> 2) & 3;
    if (m & 0xFFFF)
      h->tm[n].reload = (uint16_t)w;
    if (m & 0xFFFF0000)
      tm_control(h, n, (uint16_t)(w >> 16), now_ms());
    return 1;
  }
  case 0x040000B8:
  case 0x040000C4:
  case 0x040000D0:
  case 0x040000DC:
    set_reg32(h, a4, w);
    if (m & 0xFFFF0000)
      dma_written(id, (a4 - 0x040000B8) / 12);
    return 1;
  case 0x04000180:
  case 0x04000184:
  case 0x04000188:
    ipc_wr(id, a4, w);
    return 1;
  case 0x040001A0:
    if (m & 0xFFFF)
      card.spicnt = (uint16_t)w;
    if (m & 0x00FF0000)
      sav_data((uint8_t)(w >> 16));
    return 1;
  case 0x040001A4:
    card_start(id, w);
    return 1;
  case 0x040001A8:
  case 0x040001AC:
    memcpy(card.cmd + (a - 0x040001A8), &v, bytes);
    return 1;
  case 0x04000208:
    h->ime = w & 1;
    return 1;
  case 0x04000210:
    h->ie = w;
    return 1;
  case 0x04000214:
    h->if_ &= ~(v << shift);
    return 1; //writing 1 = already served
  }
  return 0;
}

static uint32_t read_io(int id, uint32_t a, int bytes)
{
  uint32_t v;
  if (register_common(id, a & ~3u, &v))
    return (v >> ((a & 3) * 8)) & (bytes == 4 ? ~0u : (1u << (bytes * 8)) - 1);
  v = 0;
  if (a < 0x04010000)
    memcpy(&v, hw[id].io + (a & 0xFFFF), bytes);
  return v;
}

//---- ARM9 --------------------------------------------------------------------
uint32_t io9_rd(uint32_t a, int bytes)
{
  Hw* h = &hw[0];
  if (a >= 0x04000600 && a < 0x040006A4)
  { //state and results of the 3D engine
    uint32_t v = gx_read(a & ~3u);
    if ((a & ~3u) == 0x04000600)
      v |= reg32(h, 0x04000600) & 0xC0000000; //IRQ mode of the FIFO
    return v >> ((a & 3) * 8);
  }
  uint32_t v = read_io(0, a, bytes);
  if ((a >= 0x04000280 && a < 0x040002C0) || (a >= 0x04000000 && a < 0x04000070) ||
      (a >= 0x04001000 && a < 0x04001070) || (a >= 0x040000B0 && a < 0x04000110) ||
      (a >= 0x04000240 && a < 0x04000250))
    return v;
  (void)h;
  return v;
}

void io9_wr(uint32_t a, uint32_t v, int bytes)
{
  Hw* h = &hw[0];
  if (write_common(0, a, v, bytes))
    return;
  if (a < 0x04010000)
    memcpy(h->io + (a & 0xFFFF), &v, bytes);
  if (a >= 0x04000280 && a < 0x040002A0)
  {
    divider(h);
    return;
  }
  if (a >= 0x040002B0 && a < 0x040002C0)
  {
    root(h);
    return;
  }
  if (a == 0x04000247)
  {
    g_wramcnt = (uint8_t)(v & 3);
    return;
  }
  if (a >= 0x04000240 && a < 0x0400024A)
  { //VRAMCNT A..I (byte a byte)
    for (int k = 0; k < bytes; k++)
    {
      uint32_t r = a + k - 0x04000240;
      if (r == 7)
        g_wramcnt = (uint8_t)((v >> (8 * k)) & 3);
      else if (r < 10)
        video_vramcnt(r < 7 ? (int)r : (int)r - 1, (uint8_t)(v >> (8 * k)));
    }
    return;
  }
  if (a >= 0x04000400 && a < 0x04000600)
  {
    gx_write(a & ~3u, v);
    return;
  } //motor 3D
  if (a >= 0x04000600 && a < 0x04000700)
    return;
  if ((a >= 0x04000000 && a < 0x04000070) || (a >= 0x04001000 && a < 0x04001070) ||
      (a >= 0x04000240 && a < 0x04000250) || (a >= 0x040000B0 && a < 0x040000F0) ||
      a == 0x04000204 || a == 0x04000300 || a == 0x04000304 || a == 0x04000132)
    return;
}

//---- ARM7 --------------------------------------------------------------------
uint32_t io7_rd(uint32_t a, int bytes)
{
  Hw* h = &hw[1];
  switch (a & ~3u)
  {
  case 0x04000134: //RCNT and KEYXY: X/Y not pressed, no stylus, lid open
    //bit 6 = stylus (0 = touching), bit 7 = lid (0 = open)
    return ((uint32_t)((g_keyxy & 3) | 0x3C | (g_stylus ? 0 : 0x40)) << 16 |
            (reg32(h, 0x04000134) & 0xFFFF)) >>
           ((a & 3) * 8);
  case 0x04000138:
    return rtc.io;
  case 0x040001C0:
    return ((a & 2) ? spi.data : (uint32_t)(spi.cnt & ~0x80) | ((uint32_t)spi.data << 16)) &
           (bytes == 4 ? ~0u : 0xFFFF);
  case 0x04000204:
    return reg32(&hw[0], 0x04000204) >> ((a & 3) * 8); //EXMEMSTAT
  case 0x04000240:
    return ((uint32_t)g_wramcnt << 8 | 3) >> ((a & 3) * 8); //VRAMSTAT/WRAMSTAT
  }
  uint32_t v = read_io(1, a, bytes);
  if ((a >= 0x040000B0 && a < 0x04000110) || a == 0x04000300 || a == 0x04000304)
    return v;
  return v;
}

void io7_wr(uint32_t a, uint32_t v, int bytes)
{
  Hw* h = &hw[1];
  if (write_common(1, a, v, bytes))
    return;
  if (a >= 0x04000400 && a < 0x04000500)
  { //sound channel: is it started?
    uint32_t o = a & 0xFFFF, base = o & ~15u, before = reg32(h, base);
    memcpy(h->io + o, &v, bytes);
    if (!(before & 0x80000000) && (reg32(h, base) & 0x80000000))
      g_snd_gen[(base - 0x400) / 16]++;
    return;
  }
  switch (a)
  {
  case 0x04000138:
    rtc_wr((uint8_t)v);
    return;
  case 0x040001C0:
    spi.cnt = (uint16_t)v;
    if (bytes == 4)
      goto data;
    if (!(v & 0x8000))
      spi.pos = 0;
    return;
  case 0x040001C2:
  data:
    if (bytes == 4)
      v >>= 16;
    if (spi.cnt & 0x8000)
    {
      spi.data = spi_byte((uint8_t)v);
      if (!(spi.cnt & 0x800))
      { //end of the command
        if (((spi.cnt >> 8) & 3) == 1 && spi.pos > 1 &&
            (spi.cmd == 0x0A || spi.cmd == 0x02 || spi.cmd == 0xDB || spi.cmd == 0xD8))
        {
          spi.wel = 0;
        }
        spi.pos = 0;
      }
      if (spi.cnt & 0x4000)
        irq(1, 1u << 23);
    }
    return;
  case 0x04000301: //HALTCNT
    if ((v & 0xC0) == 0x80 || (v & 0xC0) == 0xC0)
      ds_wait(&g_cpu7, 6);
    return;
  }
  if (a < 0x04010000)
    memcpy(h->io + (a & 0xFFFF), &v, bytes);
  if ((a >= 0x04000400 && a < 0x04000520) || (a >= 0x040000B0 && a < 0x040000E0) ||
      a == 0x04000300 || a == 0x04000304 || a == 0x04000134)
    return;
}

//---- boot ------------------------------------------------------------------
static uint32_t entry7;

static void WINAPI fiber_arm7(void* p)
{
  (void)p;
  fprintf(stderr, "[ds] ARM7 starting at %08X\n", entry7);
  dispatch7(&g_cpu7, entry7);
  fprintf(stderr, "[ds] the ARM7 boot returned (it should not)\n");
  exit(1);
}

void ds_start(uint32_t entry9, uint32_t e7)
{
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&t0);
  timeBeginPeriod(1); //a real 1 ms Sleep(1)
  prepare_firmware();
  save_load();
  gx_init(); //before the window: a PC without OpenGL 4.5 gets its message with no empty window
  window_init();
  sound_init();
  entry7 = e7;
  g_cpu9.id = 0;
  g_cpu7.id = 1;
  g_cpu9.ctl = g_cpu7.ctl = 0x1F;
  g_cpu9.r[14] = g_cpu7.r[14] = 0xFFFFFFF0;
  g_cpu9.cycles = g_cpu7.cycles = TURN;
  hw[0].fiber = ConvertThreadToFiber(NULL);
  hw[1].fiber = CreateFiberEx(64 * 1024, 64 * 1024 * 1024, 0, fiber_arm7, NULL);
  fprintf(stderr, "[ds] ARM9 starting at %08X\n", entry9);
  dispatch(&g_cpu9, entry9);
  fprintf(stderr, "[ds] the ARM9 boot returned (it should not)\n");
  exit(1);
}

uint8_t* ds_io7(void)
{
  return hw[1].io;
}
