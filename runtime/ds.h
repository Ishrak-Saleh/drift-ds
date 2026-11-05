// DS hardware shared by the two processors (ds.c).
#pragma once
#include "cpu.h"

extern Cpu g_cpu9, g_cpu7;
extern uint8_t g_wramcnt; // split of the shared WRAM (32 KB) between ARM9 and ARM7

uint32_t io9_rd(uint32_t a, int bytes);
void io9_wr(uint32_t a, uint32_t v, int bytes);
uint32_t io7_rd(uint32_t a, int bytes);
void io7_wr(uint32_t a, uint32_t v, int bytes);

// SWI Halt / IntrWait / VBlankIntrWait, or CP15 "wait for interrupt" (swi = -1)
void ds_wait(Cpu* c, int swi);

// Busy wait (WaitByLoop): lets the other processor run now
void ds_yield(Cpu* c);
// Starts the two processors (does not return)
void ds_start(uint32_t entry9, uint32_t entry7);

// ROM data for the card
// rom data for the cartridge
void ds_set_rom(const uint8_t* rom, size_t nbytes);

// save file, kept in ds.c. the game writes it in the background, these are the entry points
void save_write_pending(void); // write now if the save changed. call before exit
void save_per_frame(void); // once per frame from the main loop

// Configuration of the game operating system (NitroSDK) for each processor
typedef struct
{
  uint32_t cp_save, cp_restore; // 0 if none
  uint32_t reschedule; // OSi_RescheduleThread
  uint32_t queue_irq; // threads waiting for an interrupt
  uint32_t flag_resched; //"a thread switch is needed" (half word)
  uint32_t off_state, off_link, off_sig; // fields of the thread (state, queue links, next)
} SoConfig;
extern SoConfig g_so[2];
extern int
    g_no_irq[2]; //>0: do not deliver interrupts (threads.c, while saving or loading a context)
