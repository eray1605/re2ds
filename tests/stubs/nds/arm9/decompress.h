#pragma once
typedef enum { LZ77, LZ77Vram } DecompressType;
static inline void decompress(const void *src, void *dst, DecompressType t) { (void)src; (void)dst; (void)t; }
