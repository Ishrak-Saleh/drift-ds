// Addresses inside the game that the runtime itself touches, per region (the game code is the same
// in the European and the USA builds, but the data after the ARM9 code and the overlays are linked
// a few bytes apart). The USA values come from tools/map_region.py (aligns the symbol tables of the
// mkds decompilation); the translator side of this table is REGION_TABLE in tools/translate.py.
#pragma once
#include <stdint.h>

typedef struct
{
  const char* label;
  // ARM9 operating system (NitroSDK): thread switch.  Code addresses, the same in both regions.
  uint32_t cp_save, cp_restore, reschedule;
  uint32_t flag_resched; // OS "reschedule" flag (ARM9 data: moves by -32 in USA)
  uint32_t current_thread; // OSi_CurrentThread (only for the crash dump)
  // Missions menu (overlay 1 data): used by the "everything unlocked" cheat
  uint32_t obj_mission, obj_boss;
  // Loaded save game in the main-memory heap ("NKSY" ...): the player name is written there.
  // 0 = not known: runtime/main.c looks for it.
  uint32_t save;
  // Only for DRIFT_MICRO_LOG / DRIFT_WIFI_LOG
  uint32_t mic[7];
  uint32_t dwc; // DWC control pointer (overlay 0 data)
} Region;

extern const Region* g_region; // chosen in main.c from the game code in the ROM header
