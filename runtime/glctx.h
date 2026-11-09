// OpenGL 4.5 context for the GPU 3D drawing, made on a hidden window.
//
// Where it fits: the drawing thread of gpu3d.c (and the offline test tool in tests/) call
// glctx_create once. It returns a context that is current on the calling thread, with every
// function of glload.h loaded. The game window (window.c) keeps its own, older context, and the
// two do not share anything: the picture goes from one to the other through the CPU.
// Why: the GPU drawing needs OpenGL 4.5 core. The window code uses old fixed-function calls,
// which a core context does not have, so the drawing gets a context of its own.
//
// Failure: glctx_create returns NULL and writes a plain sentence in `err`, for example when the
// graphics driver has no OpenGL 4.5. Nothing is left open in that case.
#pragma once
#include <stddef.h>

typedef struct GlCtx GlCtx;

// Creates the hidden window and the context, makes it current on this thread and loads the
// functions. major and minor tell the version that was really given (4.5 or higher).
GlCtx* glctx_create(char* err, size_t err_size);
void glctx_destroy(GlCtx* c);
void glctx_version(const GlCtx* c, int* major, int* minor);

// A small check that the context really works: it compiles and links a shader, draws a triangle
// into an integer texture, reads the pixels back, and tests a fence and a texture barrier.
// Returns 1 when everything came out right, else 0 with a sentence in `err`.
int glctx_selftest(char* err, size_t err_size);
