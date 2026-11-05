// Video of the DS: VRAM banks, 2D engines and the Windows window.
#pragma once
#include <stdint.h>

extern uint8_t g_palette[0x800], g_oam[0x800];

// VRAM as seen by the ARM9 (0x06000000-0x06FFFFFF). NULL if there is no bank there
uint8_t* vram_ptr9(uint32_t a);
void video_vramcnt(int bank, uint8_t v); // bank 0..8 = A..I

// Draws the two screens with the current state of the ARM9 registers
//(io9 = registers 0x04000000-0x0400FFFF). Leaves 256x192 RGB pixels on each.
void video_draw(const uint8_t* io9, uint32_t* up, uint32_t* bottom);
// The same on another thread: takes a snapshot of VRAM, palettes and registers, and builds and
// shows the image (window_show) from there. If the thread is still on the previous one, this image
// is skipped.
void video_draw_separate(const uint8_t* io9);
void video_wait_assembly(void); // before building on this thread: the other one must have finished

// Window
void window_init(void);
void window_show(const uint32_t* up, const uint32_t* bottom);
void window_show_en(const uint32_t* up, const uint32_t* bottom, int64_t ts); // ts: when (0 = now)
int64_t window_clock(void); // QueryPerformanceCounter
void window_messages(void);

// Input: DS keys (active at 0, as the hardware) and stylus
extern volatile uint16_t g_keyinput; // A B Select Start Right Left Up Down R L
extern volatile uint16_t g_keyxy; // bit 0 X, bit 1 Y (0 = pressed)
extern volatile int g_stylus, g_stylus_x, g_stylus_y;

// 3D render scale chosen in settings.ini (1 = DS size, up to 4)
int video_scale(void);
