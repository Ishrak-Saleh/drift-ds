// Scene dump: saves one 3D scene to a file, and reads it back.
//
// Where it fits: gpu3d.c calls scenedump_write from the drawing thread when SCENE_DUMP is set in
// settings.ini (or DRIFT_SCENE_DUMP in the environment). The test tools in tests/ read the files
// with scenedump_read.
// Why: the GPU drawing (branch graphics) has to give the same picture as the CPU drawing. A dump
// holds everything the drawing needs (registers, polygons, textures, palettes) and the picture the
// CPU drawing made from it, so the two can be compared without running the game.
//
// Privacy and licence: a dump contains texture and palette data from the game, so the scenes/
// folder is git-ignored and must never be committed or shared.
//
// File layout (little endian, as this PC): header (SceneHeader), io registers (0x400 bytes), the
// mapped texture slots (0x20000 bytes each), the mapped palette slots (0x4000 bytes each), the
// polygons (npolys * sizeof(Poly)), the colour picture (w*h uint16, RGB555), the alpha picture
// (w*h uint8, 0..31). w = 256 * scale, h = 192 * scale.
#pragma once
#include "gpu3d_types.h"
#include <stdint.h>

#define SCENE_MAGIC 0x4E435344u // "DSCN"
#define SCENE_VERSION 1u
#define SCENE_NTEX 4
#define SCENE_NPAL 6
#define SCENE_TEX_SIZE 0x20000
#define SCENE_PAL_SIZE 0x4000

typedef struct
{
  uint32_t magic, version;
  uint32_t poly_size; // sizeof(Poly) when written, so a changed struct is noticed
  uint32_t scale; // 1..4
  int32_t wbuf; // 1 = W buffer, 0 = Z buffer
  int32_t vp_x1, vp_y1, vp_x2, vp_y2;
  uint32_t npolys;
  uint32_t tex_mask, pal_mask; // bit n set: that texture or palette slot is mapped
} SceneHeader;

// One scene. When it is filled for writing the pointers are borrowed. When it comes from
// scenedump_read the arrays are owned and scenedump_free releases them.
typedef struct
{
  int scale, wbuf, vp_x1, vp_y1, vp_x2, vp_y2;
  const uint8_t* io; // 0x400 bytes, the 3D registers (0x04000000 + 0x000..0x3FF)
  const uint8_t* tex[SCENE_NTEX]; // NULL = not mapped
  const uint8_t* pal[SCENE_NPAL]; // NULL = not mapped
  const Poly* polys;
  int npolys;
  const uint16_t* color; // the picture the CPU drawing made (reference), w*h RGB555
  const uint8_t* alpha; // w*h, 0..31
} Scene;

// Returns 1 when the file was written. Writes to a temporary name first, so a half written file
// never looks like a scene.
int scenedump_write(const char* path, const Scene* s);

// Returns 1 on success. Rejects a wrong magic, version or Poly size, odd sizes and a file that is
// too short or too long. On failure nothing is allocated.
int scenedump_read(const char* path, Scene* out);

void scenedump_free(Scene* s);
