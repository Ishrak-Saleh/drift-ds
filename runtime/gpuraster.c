// GPU drawing of one 3D scene. See gpuraster.h.
#include "gpuraster.h"
#include "glload.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static double now_ms(void)
{
  LARGE_INTEGER f, c;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return 1000.0 * (double)c.QuadPart / (double)f.QuadPart;
}

#define ATLAS_W 4096
#ifndef ATLAS_H // a test build makes it small to force the atlas to fill up
#define ATLAS_H 2048
#endif
#define RES_LIMIT (1 << 22) // most texture cache entry numbers the atlas table follows
#define GRID_W 64 // the screen is cut into GRID_W x GRID_H cells to find which polygons touch
#define GRID_H 48

// One corner of a triangle as the shader gets it. The first 12 floats are the screen position and
// the attributes already divided by w (like the CPU drawing), the 8 integers are the same for the
// whole polygon.
typedef struct
{
  float x, y, z, iw, r, g, b, s, t, pad0, pad1, pad2;
  int32_t p0[4]; // atlas x, atlas y, texture width, texture height
  int32_t p1[4]; // flags, 0, 0, 0 (see FLAG_*)
} GVert;

#define FLAG_REP_S 1
#define FLAG_REP_T 2
#define FLAG_FLIP_S 4
#define FLAG_FLIP_T 8
#define FLAG_TEXTURED 16
#define FLAG_DEPTH_SAME 32
#define FLAG_ACT_DEPTH 64
#define FLAG_LAYER_TRANS 128
#define FLAG_MODE_SHIFT 8 // 2 bits
#define FLAG_ALPHA_SHIFT 10 // 5 bits, the polygon alpha
#define FLAG_ID_SHIFT 16 // 6 bits, the polygon id

typedef struct
{
  int valid, x, y, w, h;
} Res;

typedef struct
{
  int x0, y0, x1, y1;
} Box;

struct GpuRaster
{
  int scale, AN, AT;
  GLuint prog, clear_prog, clear_vao, vao, vbo, fbo, atlas, pix, depth;
  GLint u_clear_pix, u_clear_depth;
  GLint u_size, u_wbuf, u_test_alpha, u_ref_alpha, u_mix_alpha, u_highlight, u_toon;
  TexCache* cache;
  Res* res; // atlas place of each texture cache entry, indexed by entry number, grows as needed
  int res_cap;
  int shelf_x, shelf_y, shelf_h;
  int need_reset;
  GVert* verts;
  int cap_verts, nverts;
  uint32_t* readback;
  GpuStats stats;
  // groups of the current scene: first vertex and vertex count of each, and where the translucent
  // ones start
  int* grp_first; // level k: first entry in pr_first/pr_count, and how many
  int* grp_count;
  int ngrp, cap_grp;
  // one entry per polygon: first vertex and vertex count, sorted by level for drawing
  int *pr_first, *pr_count, *tmp_first, *tmp_count, *tmp_level;
  int cap_poly;
};

// ---- shaders ----

static const char* VS =
    "#version 450 core\n"
    "layout(location = 0) in vec4 a0;\n" // x y z iw
    "layout(location = 1) in vec4 a1;\n" // r g b s
    "layout(location = 2) in float a2;\n" // t
    "layout(location = 3) in ivec4 q0;\n"
    "layout(location = 4) in ivec4 q1;\n"
    "uniform vec2 u_size;\n"
    "noperspective out float v_z;\n"
    "noperspective out float v_iw;\n"
    "noperspective out vec4 v_rgbs;\n"
    "noperspective out float v_t;\n"
    "flat out ivec4 f_p0;\n"
    "flat out ivec4 f_p1;\n"
    "void main(){\n"
    // screen row 0 is framebuffer row 0, so the picture is read back without a flip
    "  gl_Position = vec4(a0.x / u_size.x * 2.0 - 1.0, a0.y / u_size.y * 2.0 - 1.0, 0.0, 1.0);\n"
    "  v_z = a0.z; v_iw = a0.w; v_rgbs = a1; v_t = a2; f_p0 = q0; f_p1 = q1;\n"
    "}\n";

// The loop body of triangle() in gpu3d.c, one pixel per invocation, with the same integers.
static const char* FS =
    "#version 450 core\n"
    "layout(binding = 0) uniform usampler2D atlas;\n"
    "layout(binding = 1) uniform usampler2D pixtex;\n" // color | alpha << 15 | id << 20
    "layout(binding = 2) uniform usampler2D depthtex;\n"
    "uniform int u_wbuf;\n"
    "uniform int u_test_alpha;\n"
    "uniform int u_ref_alpha;\n"
    "uniform int u_mix_alpha;\n"
    "uniform int u_highlight;\n"
    "uniform uint u_toon[32];\n"
    "noperspective in float v_z;\n"
    "noperspective in float v_iw;\n"
    "noperspective in vec4 v_rgbs;\n"
    "noperspective in float v_t;\n"
    "flat in ivec4 f_p0;\n"
    "flat in ivec4 f_p1;\n"
    "layout(location = 0) out uvec4 o_pix;\n"
    "layout(location = 1) out uvec4 o_depth;\n"
    "int c6(float v){ return int(min(max(v * 2.0 + 0.5, 0.0), 64.0)); }\n"
    "int wrap(int v, int n, bool rep, bool flip){\n"
    "  if (rep){\n"
    "    if (flip){ int per = v & (2 * n - 1); return per >= n ? 2 * n - 1 - per : per; }\n"
    "    return v & (n - 1);\n"
    "  }\n"
    "  return v < 0 ? 0 : (v >= n ? n - 1 : v);\n"
    "}\n"
    "void main(){\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "  float iwp = v_iw;\n"
    "  if (!(iwp > 0.0)) discard;\n"
    "  int fl = f_p1.x;\n"
    "  float w; uint zi;\n"
    "  if (u_wbuf != 0){ w = 1.0 / iwp; zi = uint(min(w * 4096.0, 4.0e9)); }\n"
    "  else { w = 1.0 / iwp; zi = uint(clamp(v_z * 16777215.0, 0.0, 4.0e9)); }\n"
    "  uint depth = texelFetch(depthtex, p, 0).r;\n"
    "  bool same = (fl & 32) != 0;\n"
    "  if (same ? (zi > depth + 0x200u || zi + 0x200u < depth) : zi >= depth) discard;\n"
    "  int mode = (fl >> 8) & 3;\n"
    "  int va = ((fl >> 10) & 31) * 2 + 1;\n"
    "  int idp = (fl >> 16) & 63;\n"
    "  int vr = min(c6(v_rgbs.r * w), 63), vg = min(c6(v_rgbs.g * w), 63), vb = min(c6(v_rgbs.b * "
    "w), "
    "63);\n"
    "  int cr = vr, cg = vg, cb = vb, ca = va;\n"
    "  if (mode == 2){\n"
    "    uint tc = u_toon[vr >> 1];\n"
    "    if (u_highlight != 0){\n"
    "      cr = min(vr + int(tc & 31u) * 2, 63);\n"
    "      cg = min(vr + int((tc >> 5) & 31u) * 2, 63);\n"
    "      cb = min(vr + int((tc >> 10) & 31u) * 2, 63);\n"
    "    } else {\n"
    "      cr = int(tc & 31u) * 2 + 1;\n"
    "      cg = int((tc >> 5) & 31u) * 2 + 1;\n"
    "      cb = int((tc >> 10) & 31u) * 2 + 1;\n"
    "    }\n"
    "    vr = cr; vg = cg; vb = cb;\n"
    "  }\n"
    "  if ((fl & 16) != 0){\n"
    "    float s = v_rgbs.w * w, t = v_t * w;\n"
    "    int si = int(clamp(floor(s), -1.0e6, 1.0e6)), ti = int(clamp(floor(t), -1.0e6, 1.0e6));\n"
    "    int tw = f_p0.z, th = f_p0.w;\n"
    "    si = wrap(si, tw, (fl & 1) != 0, (fl & 4) != 0);\n"
    "    ti = wrap(ti, th, (fl & 2) != 0, (fl & 8) != 0);\n"
    "    uvec4 tx = texelFetch(atlas, ivec2(f_p0.x + si, f_p0.y + ti), 0);\n"
    "    int r5 = int(tx.r), g5 = int(tx.g), b5 = int(tx.b), ta = int(tx.a);\n"
    "    int tr = r5 * 2 + (r5 != 0 ? 1 : 0);\n"
    "    int tg = g5 * 2 + (g5 != 0 ? 1 : 0);\n"
    "    int tb = b5 * 2 + (b5 != 0 ? 1 : 0);\n"
    "    int ta6 = ta * 2 + (ta != 0 ? 1 : 0);\n"
    "    if (mode == 1){\n"
    "      cr = (tr * ta6 + vr * (63 - ta6)) / 64;\n"
    "      cg = (tg * ta6 + vg * (63 - ta6)) / 64;\n"
    "      cb = (tb * ta6 + vb * (63 - ta6)) / 64;\n"
    "    } else {\n"
    "      cr = ((tr + 1) * (vr + 1) - 1) / 64;\n"
    "      cg = ((tg + 1) * (vg + 1) - 1) / 64;\n"
    "      cb = ((tb + 1) * (vb + 1) - 1) / 64;\n"
    "      ca = ((ta6 + 1) * (va + 1) - 1) / 64;\n"
    "    }\n"
    "  }\n"
    "  int a5 = ca >> 1;\n"
    "  if (a5 == 0) discard;\n"
    "  if (u_test_alpha != 0 && a5 <= u_ref_alpha) discard;\n"
    "  int col = (cr >> 1) | ((cg >> 1) << 5) | ((cb >> 1) << 10);\n"
    "  uint pix = texelFetch(pixtex, p, 0).r;\n"
    "  int da = int((pix >> 15) & 31u), did = int((pix >> 20) & 63u);\n"
    "  uint nd = depth;\n"
    "  if (a5 < 31 && u_mix_alpha != 0){\n"
    "    if ((fl & 128) != 0 && did == idp && da < 31 && da > 0) discard;\n"
    "    if (da != 0){\n"
    "      int d0 = int(pix & 0x7FFFu);\n"
    "      int dr = d0 & 31, dg = (d0 >> 5) & 31, db = (d0 >> 10) & 31;\n"
    "      int nr = ((cr >> 1) * (a5 + 1) + dr * (31 - a5)) / 32;\n"
    "      int ng = ((cg >> 1) * (a5 + 1) + dg * (31 - a5)) / 32;\n"
    "      int nb = ((cb >> 1) * (a5 + 1) + db * (31 - a5)) / 32;\n"
    "      col = nr | (ng << 5) | (nb << 10);\n"
    "      a5 = max(a5, da);\n"
    "    }\n"
    "    if ((fl & 64) != 0) nd = zi;\n"
    "  } else {\n"
    "    nd = zi;\n"
    "  }\n"
    "  o_pix = uvec4(uint(col) | (uint(a5) << 15) | (uint(idp) << 20), 0u, 0u, 0u);\n"
    "  o_depth = uvec4(nd, 0u, 0u, 0u);\n"
    "}\n";

// Clearing the two integer images with glClearBufferuiv gave wrong reads on the first draw (the
// driver keeps a "fast clear" mark that the shader read does not honour while the image is also
// the render target). So the clear is a full screen triangle that writes the values.
static const char* CLEAR_VS = "#version 450 core\n"
                              "void main(){\n"
                              "  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
                              "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
                              "}\n";
static const char* CLEAR_FS =
    "#version 450 core\n"
    "uniform uint u_pix;\n"
    "uniform uint u_depth;\n"
    "layout(location = 0) out uvec4 o_pix;\n"
    "layout(location = 1) out uvec4 o_depth;\n"
    "void main(){ o_pix = uvec4(u_pix, 0u, 0u, 0u); o_depth = uvec4(u_depth, 0u, 0u, 0u); }\n";

static int compile(GLuint* out, GLenum type, const char* src, char* err, size_t n)
{
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GLX_COMPILE_STATUS, &ok);
  if (!ok)
  {
    char log[600];
    glGetShaderInfoLog(s, sizeof log, NULL, log);
    snprintf(err, n, "3D shader did not compile: %s", log);
    glDeleteShader(s);
    return 0;
  }
  *out = s;
  return 1;
}

static void tex_params(GLuint t)
{
  glBindTexture(GL_TEXTURE_2D, t);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

GpuRaster* gpuraster_new(int scale, char* err, size_t err_size)
{
  if (scale < 1 || scale > 4)
  {
    snprintf(err, err_size, "scale %d is not supported", scale);
    return NULL;
  }
  GpuRaster* g = calloc(1, sizeof *g);
  if (!g)
  {
    snprintf(err, err_size, "out of memory");
    return NULL;
  }
  g->scale = scale;
  g->AN = 256 * scale;
  g->AT = 192 * scale;
  g->cap_poly = MAX_POLYS;
  g->pr_first = malloc((size_t)g->cap_poly * sizeof(int));
  g->pr_count = malloc((size_t)g->cap_poly * sizeof(int));
  g->tmp_first = malloc((size_t)g->cap_poly * sizeof(int));
  g->tmp_count = malloc((size_t)g->cap_poly * sizeof(int));
  g->tmp_level = malloc((size_t)g->cap_poly * sizeof(int));
  g->cap_verts = MAX_POLYS * 3 * 8;
  g->verts = malloc((size_t)g->cap_verts * sizeof(GVert));
  g->readback = malloc((size_t)g->AN * (size_t)g->AT * 4);
  g->cache = texcache_new(96u * 1024 * 1024);
  if (!g->verts || !g->readback || !g->cache || !g->pr_first || !g->pr_count || !g->tmp_first ||
      !g->tmp_count || !g->tmp_level)
  {
    snprintf(err, err_size, "out of memory");
    gpuraster_free(g);
    return NULL;
  }
  while (glGetError() != GL_NO_ERROR)
    ;
  GLuint vs = 0, fs = 0;
  if (!compile(&vs, GLX_VERTEX_SHADER, VS, err, err_size) ||
      !compile(&fs, GLX_FRAGMENT_SHADER, FS, err, err_size))
  {
    if (vs)
      glDeleteShader(vs);
    gpuraster_free(g);
    return NULL;
  }
  g->prog = glCreateProgram();
  glAttachShader(g->prog, vs);
  glAttachShader(g->prog, fs);
  glLinkProgram(g->prog);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint linked = 0;
  glGetProgramiv(g->prog, GLX_LINK_STATUS, &linked);
  if (!linked)
  {
    char log[600];
    glGetProgramInfoLog(g->prog, sizeof log, NULL, log);
    snprintf(err, err_size, "3D shader program did not link: %s", log);
    gpuraster_free(g);
    return NULL;
  }
  {
    GLuint cvs = 0, cfs = 0;
    if (!compile(&cvs, GLX_VERTEX_SHADER, CLEAR_VS, err, err_size) ||
        !compile(&cfs, GLX_FRAGMENT_SHADER, CLEAR_FS, err, err_size))
    {
      if (cvs)
        glDeleteShader(cvs);
      gpuraster_free(g);
      return NULL;
    }
    g->clear_prog = glCreateProgram();
    glAttachShader(g->clear_prog, cvs);
    glAttachShader(g->clear_prog, cfs);
    glLinkProgram(g->clear_prog);
    glDeleteShader(cvs);
    glDeleteShader(cfs);
    glGetProgramiv(g->clear_prog, GLX_LINK_STATUS, &linked);
    if (!linked)
    {
      snprintf(err, err_size, "3D clear shader did not link");
      gpuraster_free(g);
      return NULL;
    }
    g->u_clear_pix = glGetUniformLocation(g->clear_prog, "u_pix");
    g->u_clear_depth = glGetUniformLocation(g->clear_prog, "u_depth");
  }
  g->u_size = glGetUniformLocation(g->prog, "u_size");
  g->u_wbuf = glGetUniformLocation(g->prog, "u_wbuf");
  g->u_test_alpha = glGetUniformLocation(g->prog, "u_test_alpha");
  g->u_ref_alpha = glGetUniformLocation(g->prog, "u_ref_alpha");
  g->u_mix_alpha = glGetUniformLocation(g->prog, "u_mix_alpha");
  g->u_highlight = glGetUniformLocation(g->prog, "u_highlight");
  g->u_toon = glGetUniformLocation(g->prog, "u_toon");

  GLuint t[3];
  glGenTextures(3, t);
  g->atlas = t[0];
  g->pix = t[1];
  g->depth = t[2];
  tex_params(g->atlas);
  glTexStorage2D(GL_TEXTURE_2D, 1, GLX_RGBA8UI, ATLAS_W, ATLAS_H);
  tex_params(g->pix);
  glTexStorage2D(GL_TEXTURE_2D, 1, GLX_R32UI, g->AN, g->AT);
  tex_params(g->depth);
  glTexStorage2D(GL_TEXTURE_2D, 1, GLX_R32UI, g->AN, g->AT);

  glGenFramebuffers(1, &g->fbo);
  glBindFramebuffer(GLX_FRAMEBUFFER, g->fbo);
  glFramebufferTexture2D(GLX_FRAMEBUFFER, GLX_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->pix, 0);
  glFramebufferTexture2D(GLX_FRAMEBUFFER, GLX_COLOR_ATTACHMENT0 + 1, GL_TEXTURE_2D, g->depth, 0);
  const GLenum bufs[2] = {GLX_COLOR_ATTACHMENT0, GLX_COLOR_ATTACHMENT0 + 1};
  glDrawBuffers(2, bufs);
  if (glCheckFramebufferStatus(GLX_FRAMEBUFFER) != GLX_FRAMEBUFFER_COMPLETE)
  {
    snprintf(err, err_size, "the 3D framebuffer is not complete");
    gpuraster_free(g);
    return NULL;
  }

  glGenVertexArrays(1, &g->clear_vao); // no attributes: the clear needs no vertex data
  glGenVertexArrays(1, &g->vao);
  glBindVertexArray(g->vao);
  glGenBuffers(1, &g->vbo);
  glBindBuffer(GLX_ARRAY_BUFFER, g->vbo);
  const GLsizei stride = (GLsizei)sizeof(GVert);
  glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, stride, (const void*)0);
  glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (const void*)16);
  glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, (const void*)32);
  glVertexAttribIPointer(3, 4, GL_INT, stride, (const void*)48);
  glVertexAttribIPointer(4, 4, GL_INT, stride, (const void*)64);
  for (int i = 0; i < 5; i++)
    glEnableVertexAttribArray((GLuint)i);

  GLenum e = glGetError();
  if (e != GL_NO_ERROR)
  {
    snprintf(err, err_size, "OpenGL reported error 0x%X while preparing the 3D drawing", e);
    gpuraster_free(g);
    return NULL;
  }
  return g;
}

void gpuraster_free(GpuRaster* g)
{
  if (!g)
    return;
  if (g->prog)
    glDeleteProgram(g->prog);
  if (g->clear_prog)
    glDeleteProgram(g->clear_prog);
  if (g->vao)
    glDeleteVertexArrays(1, &g->vao);
  if (g->clear_vao)
    glDeleteVertexArrays(1, &g->clear_vao);
  if (g->vbo)
    glDeleteBuffers(1, &g->vbo);
  if (g->fbo)
    glDeleteFramebuffers(1, &g->fbo);
  GLuint t[3] = {g->atlas, g->pix, g->depth};
  glDeleteTextures(3, t);
  texcache_free(g->cache);
  free(g->res);
  free(g->verts);
  free(g->readback);
  free(g->grp_first);
  free(g->pr_first);
  free(g->pr_count);
  free(g->tmp_first);
  free(g->tmp_count);
  free(g->tmp_level);
  free(g->grp_count);
  free(g);
}

const GpuStats* gpuraster_stats(const GpuRaster* g)
{
  return &g->stats;
}

// ---- texture atlas ----

static void atlas_reset(GpuRaster* g)
{
  memset(g->res, 0, (size_t)g->res_cap * sizeof *g->res);
  g->shelf_x = g->shelf_y = g->shelf_h = 0;
  g->stats.atlas_resets++;
}

static int atlas_alloc(GpuRaster* g, int w, int h, int* x, int* y)
{
  if (g->shelf_x + w > ATLAS_W)
  {
    g->shelf_y += g->shelf_h;
    g->shelf_x = 0;
    g->shelf_h = 0;
  }
  if (g->shelf_y + h > ATLAS_H || w > ATLAS_W)
    return 0;
  *x = g->shelf_x;
  *y = g->shelf_y;
  g->shelf_x += w;
  if (h > g->shelf_h)
    g->shelf_h = h;
  return 1;
}

// Finds the atlas place of a texture, decoding and sending it when it is new or changed. Returns
// 0 when there is no texture to use.
static int texture_place(GpuRaster* g, const GpuScene* s, uint32_t ti, uint32_t pltt, Res* out)
{
  TexEntry e;
  if (!texcache_get(g->cache, &s->mem, ti, pltt, &e))
    return 0;
  if (e.id >= (uint32_t)g->res_cap)
  {
    if (e.id >= RES_LIMIT)
      return 0;
    int nc = g->res_cap ? g->res_cap : 256;
    while ((uint32_t)nc <= e.id)
      nc *= 2;
    Res* nr = realloc(g->res, (size_t)nc * sizeof *nr);
    if (!nr)
      return 0;
    memset(nr + g->res_cap, 0, (size_t)(nc - g->res_cap) * sizeof *nr);
    g->res = nr;
    g->res_cap = nc;
  }
  Res* r = &g->res[e.id];
  if (r->valid && (r->w != e.w || r->h != e.h))
    r->valid = 0; // the slot of the cache got another texture
  if (!r->valid)
  {
    int x, y;
    if (!atlas_alloc(g, e.w, e.h, &x, &y))
    {
      g->need_reset = 1;
      return 0;
    }
    *r = (Res){1, x, y, e.w, e.h};
    e.fresh = 1;
  }
  if (e.fresh)
  {
    glBindTexture(GL_TEXTURE_2D, g->atlas);
    glTexSubImage2D(GL_TEXTURE_2D, 0, r->x, r->y, e.w, e.h, GLX_RGBA_INTEGER, GL_UNSIGNED_BYTE,
                    e.px);
    g->stats.textures_uploaded++;
  }
  *out = *r;
  return 1;
}

// ---- building the vertex list ----

// Same selection and screen maths as prepare() in gpu3d.c. Pass 0 takes the opaque polygons, pass
// 1 the translucent ones, each in list order.
static void build(GpuRaster* g, const GpuScene* s)
{
  const uint8_t* io = s->io;
  const int AN = g->AN, AT = g->AT, esc = g->scale;
  uint16_t disp3d = (uint16_t)(io[0x60] | io[0x61] << 8);
  float vw = (float)(s->vp_x2 - s->vp_x1 + 1), vh = (float)(s->vp_y2 - s->vp_y1 + 1);
  g->nverts = 0;
  g->ngrp = 0;
  g->need_reset = 0;
  g->stats.groups = 0;
  g->stats.polygons = 0;
  g->stats.triangles = 0;
  g->stats.textures_uploaded = 0;
  g->stats.textures_missing = 0;
  int cell[GRID_W * GRID_H]; // highest level of a polygon that touched the cell
  memset(cell, 0, sizeof cell);
  int npoly = 0, nlevel = 0;
  for (int pass = 0; pass < 2; pass++)
  {
    for (int k = 0; k < s->npolys; k++)
    {
      const Poly* p = &s->polys[k];
      if (p->n < 3 || (p->translucent != 0) != pass)
        continue;
      int alpha = (p->attr >> 16) & 31, mode = (p->attr >> 4) & 3;
      if (alpha == 0 || mode == 3)
        continue; // wireframe and shadows are not drawn
      GVert v[MAX_VERT_POLY];
      float area = 0, minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
      for (int i = 0; i < p->n; i++)
      {
        const Vert* a = &p->v[i];
        float w = a->w == 0 ? 1e-6f : a->w, iw = 1.0f / w;
        GVert* o = &v[i];
        memset(o, 0, sizeof *o);
        o->x = ((a->x * iw + 1) * vw / 2 + (float)s->vp_x1) * (float)esc;
        o->y = ((1 - a->y * iw) * vh / 2 + (float)(191 - s->vp_y2)) * (float)esc;
        o->z = s->wbuf ? w : (a->z * iw + 1) / 2;
        o->iw = iw;
        o->r = a->r * iw;
        o->g = a->g * iw;
        o->b = a->b * iw;
        o->s = a->s * iw;
        o->t = a->t * iw;
        if (o->x < minx)
          minx = o->x;
        if (o->x > maxx)
          maxx = o->x;
        if (o->y < miny)
          miny = o->y;
        if (o->y > maxy)
          maxy = o->y;
      }
      for (int i = 0; i < p->n; i++)
      {
        const GVert *a = &v[i], *b = &v[(i + 1) % p->n];
        area += a->x * b->y - b->x * a->y;
      }
      int front = area < 0;
      if (front && !((p->attr >> 7) & 1))
        continue;
      if (!front && !((p->attr >> 6) & 1))
        continue;
      // nothing of it lands in the image
      Box bx;
      bx.x0 = (int)floorf(fmaxf(minx, 0.0f));
      bx.y0 = (int)floorf(fmaxf(miny, 0.0f));
      bx.x1 = (int)ceilf(fminf(maxx, (float)(AN - 1)));
      bx.y1 = (int)ceilf(fminf(maxy, (float)(AT - 1)));
      if (maxx < 0 || maxy < 0 || minx > (float)(AN - 1) || miny > (float)(AT - 1) ||
          bx.x0 > bx.x1 || bx.y0 > bx.y1)
        continue;
      if (g->nverts + (p->n - 2) * 3 > g->cap_verts)
        continue;

      int fmt = (p->teximage >> 26) & 7;
      int32_t flags = 0;
      int32_t p0[4] = {0, 0, 0, 0};
      if ((disp3d & 1) && fmt != 0)
      {
        Res r;
        if (texture_place(g, s, p->teximage, p->pltt, &r))
        {
          flags |= FLAG_TEXTURED;
          p0[0] = r.x;
          p0[1] = r.y;
          p0[2] = r.w;
          p0[3] = r.h;
          flags |= ((p->teximage >> 16) & 1) ? FLAG_REP_S : 0;
          flags |= ((p->teximage >> 17) & 1) ? FLAG_REP_T : 0;
          flags |= ((p->teximage >> 18) & 1) ? FLAG_FLIP_S : 0;
          flags |= ((p->teximage >> 19) & 1) ? FLAG_FLIP_T : 0;
        }
        else
          g->stats.textures_missing++;
      }
      if ((p->attr >> 14) & 1)
        flags |= FLAG_DEPTH_SAME;
      if ((p->attr >> 11) & 1)
        flags |= FLAG_ACT_DEPTH;
      if (pass)
        flags |= FLAG_LAYER_TRANS;
      flags |= mode << FLAG_MODE_SHIFT;
      flags |= alpha << FLAG_ALPHA_SHIFT;
      flags |= (int32_t)((p->attr >> 24) & 63) << FLAG_ID_SHIFT;
      for (int i = 0; i < p->n; i++)
      {
        memcpy(v[i].p0, p0, sizeof p0);
        v[i].p1[0] = flags;
      }

      if (npoly == g->cap_poly)
        continue;
      // a polygon is drawn one level after the highest level of the polygons it touches
      int cx0 = bx.x0 * GRID_W / AN, cx1 = bx.x1 * GRID_W / AN;
      int cy0 = bx.y0 * GRID_H / AT, cy1 = bx.y1 * GRID_H / AT;
      int level = 0;
      for (int cy = cy0; cy <= cy1; cy++)
        for (int cx = cx0; cx <= cx1; cx++)
          if (cell[cy * GRID_W + cx] > level)
            level = cell[cy * GRID_W + cx];
      level++;
      for (int cy = cy0; cy <= cy1; cy++)
        for (int cx = cx0; cx <= cx1; cx++)
          cell[cy * GRID_W + cx] = level;
      g->tmp_first[npoly] = g->nverts;
      g->tmp_level[npoly] = level;
      if (level > nlevel)
        nlevel = level;
      g->stats.polygons++;
      for (int j = 1; j + 1 < p->n; j++)
      {
        g->verts[g->nverts++] = v[0];
        g->verts[g->nverts++] = v[j];
        g->verts[g->nverts++] = v[j + 1];
        g->stats.triangles++;
      }
      g->tmp_count[npoly] = g->nverts - g->tmp_first[npoly];
      npoly++;
    }
  }
  // sort the polygons by level (counting sort keeps the list order inside a level)
  if (nlevel > g->cap_grp)
  {
    g->grp_first = realloc(g->grp_first, (size_t)nlevel * sizeof(int));
    g->grp_count = realloc(g->grp_count, (size_t)nlevel * sizeof(int));
    g->cap_grp = nlevel;
  }
  for (int k = 0; k < nlevel; k++)
    g->grp_count[k] = 0;
  for (int i = 0; i < npoly; i++)
    g->grp_count[g->tmp_level[i] - 1]++;
  for (int k = 0, at = 0; k < nlevel; k++)
  {
    g->grp_first[k] = at;
    at += g->grp_count[k];
    g->grp_count[k] = 0;
  }
  for (int i = 0; i < npoly; i++)
  {
    int k = g->tmp_level[i] - 1, at = g->grp_first[k] + g->grp_count[k]++;
    g->pr_first[at] = g->tmp_first[i];
    g->pr_count[at] = g->tmp_count[i];
  }
  g->ngrp = nlevel;
  g->stats.groups = nlevel;
}

int gpuraster_draw(GpuRaster* g, const GpuScene* s, uint16_t* color, uint8_t* alpha)
{
  const int AN = g->AN, AT = g->AT;
  const uint8_t* io = s->io;
  double t0 = now_ms();
  texcache_scene(g->cache);
  build(g, s);
  if (g->need_reset)
  { // the atlas was full: empty it and place this scene's textures again
    atlas_reset(g);
    build(g, s);
  }

  double t1 = now_ms();
  glBindFramebuffer(GLX_FRAMEBUFFER, g->fbo);
  glViewport(0, 0, AN, AT);
  uint32_t clear = (uint32_t)(io[0x350] | io[0x351] << 8 | io[0x352] << 16 | io[0x353] << 24);
  uint32_t clear_depth = (uint32_t)((io[0x354] | io[0x355] << 8) & 0x7FFF);
  GLuint cpix[4] = {(clear & 0x7FFF) | ((clear >> 16) & 31) << 15 | ((clear >> 24) & 63) << 20, 0,
                    0, 0};
  GLuint cdep[4] = {clear_depth * 0x200 + 0x1FF, 0, 0, 0};
  glBindVertexArray(g->clear_vao);
  glUseProgram(g->clear_prog);
  glUniform1ui(g->u_clear_pix, cpix[0]);
  glUniform1ui(g->u_clear_depth, cdep[0]);
  glDrawArrays(GL_TRIANGLES, 0, 3);

  glUseProgram(g->prog);
  uint16_t disp3d = (uint16_t)(io[0x60] | io[0x61] << 8);
  glUniform2f(g->u_size, (float)AN, (float)AT);
  glUniform1i(g->u_wbuf, s->wbuf != 0);
  glUniform1i(g->u_test_alpha, (disp3d >> 2) & 1);
  glUniform1i(g->u_ref_alpha, io[0x340] & 31);
  glUniform1i(g->u_mix_alpha, (disp3d >> 3) & 1);
  glUniform1i(g->u_highlight, (disp3d >> 1) & 1);
  GLuint toon[32];
  for (int i = 0; i < 32; i++)
    toon[i] = (GLuint)(io[0x380 + i * 2] | io[0x381 + i * 2] << 8);
  glUniform1uiv(g->u_toon, 32, toon);

  glActiveTexture(GLX_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, g->atlas);
  glActiveTexture(GLX_TEXTURE0 + 1);
  glBindTexture(GL_TEXTURE_2D, g->pix);
  glActiveTexture(GLX_TEXTURE0 + 2);
  glBindTexture(GL_TEXTURE_2D, g->depth);

  glBindVertexArray(g->vao);
  glBindBuffer(GLX_ARRAY_BUFFER, g->vbo);
  glBufferData(GLX_ARRAY_BUFFER, (GLsizeiptr_)((size_t)g->nverts * sizeof(GVert)), g->verts,
               GLX_STREAM_DRAW);
  for (int k = 0; k < g->ngrp; k++)
  {
    glTextureBarrier(); // the shader reads what the groups before it wrote
    glMultiDrawArrays(GL_TRIANGLES, g->pr_first + g->grp_first[k], g->pr_count + g->grp_first[k],
                      g->grp_count[k]);
  }
  glTextureBarrier();
  GLenum e = glGetError();
  if (e != GL_NO_ERROR)
    return 0;

  double t2 = now_ms();
  g->stats.ms_build = t1 - t0;
  g->stats.ms_submit = t2 - t1;
  if (color || alpha)
  {
    glReadBuffer(GLX_COLOR_ATTACHMENT0);
    glReadPixels(0, 0, AN, AT, GLX_RED_INTEGER, GL_UNSIGNED_INT, g->readback);
    double t3 = now_ms();
    g->stats.ms_read = t3 - t2;
    for (int i = 0; i < AN * AT; i++)
    {
      uint32_t px = g->readback[i];
      if (color)
        color[i] = (uint16_t)(px & 0x7FFF);
      if (alpha)
        alpha[i] = (uint8_t)((px >> 15) & 31);
    }
    g->stats.ms_unpack = now_ms() - t3;
  }
  return glGetError() == GL_NO_ERROR;
}
