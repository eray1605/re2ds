// Minimal stand-in for <nds.h> so door.c can run on the host (tests/door_host.c).
#pragma once
#include <stdint.h>
#include <stdbool.h>
typedef uint32_t u32;
typedef int32_t s32;
#define GL_RGB256 4
#define TEXTURE_SIZE_128 4
#define TEXTURE_SIZE_256 5
#define TEXGEN_TEXCOORD 0
#define POLY_ALPHA(n) 0
#define POLY_CULL_NONE 0
#define POLY_ID(n) 0
static inline void DC_FlushRange(const void *p, uint32_t n) { (void)p; (void)n; }
static inline int glGenTextures(int n, int *ids) { (void)n; ids[0] = 1; return 1; }
static inline void glBindTexture(int t, int id) { (void)t; (void)id; }
static inline int glTexImage2D(int a, int b, int c, int d, int e, int f, int g, const void *h) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h; return 1; }
static inline void glColorTableEXT(int a, int b, int c, int d, int e, const uint16_t *p) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)p; }
static inline void glDeleteTextures(int n, int *ids) { (void)n; (void)ids; }
static inline void glColor3b(int r, int g, int b) { (void)r; (void)g; (void)b; }
static inline void glPolyFmt(int f) { (void)f; }
static inline void glPushMatrix(void) {}
static inline void glPopMatrix(int n) { (void)n; }
static inline void glTranslatef32(int x, int y, int z) { (void)x; (void)y; (void)z; }
static inline void glRotateXi(int a) { (void)a; }
static inline void glRotateYi(int a) { (void)a; }
static inline void glRotateZi(int a) { (void)a; }
static inline void glCallList(const u32 *l) { (void)l; }
static inline void glFlush(int m) { (void)m; }
void setBrightness(int screen, int level);
