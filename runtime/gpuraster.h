//GPU drawing of one 3D scene: polygons in, colour and alpha picture out.
//
//where it fits: gpu3d.c does the geometry and builds the polygon list (gpu3d_types.h). This module
//draws that list with OpenGL 4.5 and gives back the same two pictures the CPU drawing gives
//(RGB555 colour and a 0..31 alpha). The offline test (tests/test_gpuraster.c) calls it on recorded
//scenes and compares the result with the stored CPU picture.
//why: branch graphics replaces the CPU rasterizer with this one.
//
//how it draws: the screen positions are made on the CPU (same maths as the CPU drawing). The
//graphics card only rasterizes the triangles. The shader then does what the CPU loop did for
//every pixel: depth test with the console's equal window, texture, modulate or decal, toon table,
//alpha test and the 5 bit blend. Colour, alpha, polygon id and depth live in two integer images
//that the shader reads and writes itself, so the numbers are the same integers as on the CPU.
//Polygons are drawn in groups whose screen boxes do not touch, with a texture barrier between
//groups, so a pixel is always updated in polygon order.
//
//a GL 4.5 context must be current on the calling thread (see glctx.h).
#pragma once
#include "gpu3d_types.h"
#include "texdecode.h"
#include <stddef.h>
#include <stdint.h>

typedef struct GpuRaster GpuRaster;

//what one scene needs. io is the 0x400 bytes of 3D registers. mem is the texture and palette
//memory as it was for this scene.
typedef struct
{
  const uint8_t* io;
  int wbuf, vp_x1, vp_y1, vp_x2, vp_y2;
  const Poly* polys;
  int npolys;
  TexMem mem;
} GpuScene;

typedef struct
{
  int polygons; //polygons drawn (after the culling)
  int groups; //draw groups of the last scene (each has a texture barrier before it)
  int triangles; //triangles drawn
  int textures_uploaded; //textures sent to the card in the last scene
  int textures_missing; //polygons that wanted a texture the cache could not hold
  int atlas_resets; //times the texture atlas was emptied since the start
  double ms_build, ms_submit, ms_read, ms_unpack; //where the time of the last scene went
} GpuStats;

//scale 1..4: the picture is 256*scale by 192*scale. Returns NULL with a sentence in err on
//failure (a shader that does not compile, for example).
GpuRaster* gpuraster_new(int scale, char* err, size_t err_size);
void gpuraster_free(GpuRaster* g);

//draws the scene. color gets w*h RGB555 words and alpha w*h bytes (0..31), w = 256*scale,
//h = 192*scale. Either may be NULL. Returns 1 on success.
int gpuraster_draw(GpuRaster* g, const GpuScene* s, uint16_t* color, uint8_t* alpha);

const GpuStats* gpuraster_stats(const GpuRaster* g);
