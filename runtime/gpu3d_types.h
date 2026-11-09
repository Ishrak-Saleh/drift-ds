// Polygon list types of the 3D engine, shared by gpu3d.c and the scene dump (scenedump.c).
//
// Where it fits: the geometry part of gpu3d.c turns the game's commands into a list of Poly. The
// drawing part (CPU rasterizer today, GPU later) reads that list. Keeping the types in one header
// lets the scene dump and the offline test tools read the same list without copying it.
// Why: the GPU work (branch graphics) needs recorded scenes to compare pictures with.
#pragma once
#include <stdint.h>

typedef struct
{
  float x, y, z, w; // clip coordinates
  float r, g, b; // color 0..31
  float s, t; // texels
} Vert;

#define MAX_VERT_POLY 10
typedef struct
{
  Vert v[MAX_VERT_POLY];
  int n; // vertices after clipping (0 if outside the view)
  uint32_t attr, teximage, pltt;
  int translucent;
} Poly;

#define MAX_POLYS 4096
