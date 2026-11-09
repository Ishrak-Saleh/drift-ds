// Picture compare for the GPU work: how different are two 3D pictures?
//
// Where it fits: the offline GPU test (later stages) draws a recorded scene with the GPU and
// compares the result with the picture stored in the scene dump (made by the CPU drawing).
// Why: "the same picture" needs a number. The rule agreed for the GPU path (decisions row 195):
// edge pixels are not counted, at most 0.1 percent of the other pixels may differ, and none of
// them may be off by more than a few colour steps.
#pragma once
#include <stdint.h>

typedef struct
{
  int total; // pixels compared
  int edge; // differing pixels next to a colour or alpha change in the reference (not counted)
  int differing; // non-edge pixels with a channel off by more than small_step
  int worst; // largest channel difference among non-edge pixels, in 5-bit steps (0..31)
  double percent; // differing / total * 100
  int shifted; // non-edge pixels whose colour equals a neighbouring reference pixel (not counted)
  double shifted_percent; // shifted / total * 100, held to the same limit as percent
  int alpha_wrong; // non-edge pixels where one picture is transparent and the other is not
  int pass; // 1 when percent and shifted_percent <= max_percent, worst <= max_step and alpha_wrong
            // == 0
} CmpResult;

typedef struct
{
  int small_step; // a channel difference up to this is rounding, not a difference (default 1)
  int max_step; // no non-edge pixel may be off by more than this (default 3)
  double max_percent; // allowed share of differing non-edge pixels (default 0.1)
  int edge_step; // a neighbour that differs from the reference pixel by more than this marks an
                 // edge
} CmpRule;

CmpRule cmp_default_rule(void);

// Pictures are w*h RGB555 colour plus 0..31 alpha. diff_out may be NULL, or w*h bytes that get 0
// for equal, 1 for rounding, 2 for edge, 3 for differing, 4 for transparency wrong, 5 for shifted.
CmpResult cmp_pictures(const uint16_t* ref_c, const uint8_t* ref_a, const uint16_t* got_c,
                       const uint8_t* got_a, int w, int h, const CmpRule* rule, uint8_t* diff_out);
