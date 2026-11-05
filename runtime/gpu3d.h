// 3D engine of the DS (gpu3d.c)
#pragma once
#include <stdint.h>

void gx_init(void);
void gx_write(uint32_t a, uint32_t v); // 0x04000400-0x040005FF: FIFO and command ports
uint32_t gx_read(uint32_t a); // 0x04000600-0x040006A3: state and results
void gx_vblank(const uint8_t* io9); // if SWAP_BUFFERS was requested, paints the scene

// Last 3D image (256x192): RGB555 color and alpha 0..31 (0 = transparent)
const uint16_t* gx_color(void);
const uint8_t* gx_alpha(void);
// The same at big size (256*esc x 192*esc), with the rescale (video_scale() > 1)
const uint16_t* gx_hd_color(void);
const uint8_t* gx_hd_alpha(void);
int gx_indice(void);
const uint16_t* gx_color_of(int i);
const uint8_t* gx_alpha_of(int i);
const uint16_t* gx_hd_color_of(int i);
const uint8_t* gx_hd_alpha_of(int i);
int gx_polys(void); // polygons of the last scene (for debugging)
uint64_t gx_frames(void);
uint64_t gx_skipped(void); // scenes dropped because the drawing threads were still busy
