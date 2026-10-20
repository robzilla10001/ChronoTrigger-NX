// slang_image.c - CtSlLoadImage: a PNG as RGBA8 (the look-up textures of a preset). libpng, where the build has it (CT_HAVE_PNG); otherwise NULL.
#include "slang_chain.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef CT_HAVE_PNG
#include <png.h>
uint8_t* CtSlLoadImage(const char* path, int* w, int* h) {
  FILE* f = fopen(path, "rb"); if (!f) return NULL;
  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  png_infop info = png ? png_create_info_struct(png) : NULL;
  uint8_t* px = NULL; png_bytep* rows = NULL;
  if (!png || !info || setjmp(png_jmpbuf(png))) { if (png) png_destroy_read_struct(&png, info ? &info : NULL, NULL); fclose(f); free(px); free(rows); return NULL; }
  png_init_io(png, f); png_read_info(png, info);
  png_uint_32 W = png_get_image_width(png, info), H = png_get_image_height(png, info);
  int ct = png_get_color_type(png, info), bd = png_get_bit_depth(png, info);
  if (bd == 16) png_set_strip_16(png);
  if (ct == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
  if (ct == PNG_COLOR_TYPE_GRAY && bd < 8) png_set_expand_gray_1_2_4_to_8(png);
  if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
  if (ct == PNG_COLOR_TYPE_GRAY || ct == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
  if (ct == PNG_COLOR_TYPE_RGB || ct == PNG_COLOR_TYPE_GRAY || ct == PNG_COLOR_TYPE_PALETTE) png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
  png_read_update_info(png, info);
  if (W < 1 || H < 1 || W > 16384 || H > 16384) longjmp(png_jmpbuf(png), 1);
  px = (uint8_t*)malloc((size_t)W * H * 4); rows = (png_bytep*)malloc(H * sizeof(png_bytep));
  if (!px || !rows) longjmp(png_jmpbuf(png), 1);
  for (png_uint_32 y = 0; y < H; y++) rows[y] = px + (size_t)y * W * 4;
  png_read_image(png, rows); png_destroy_read_struct(&png, &info, NULL); fclose(f); free(rows);
  *w = (int)W; *h = (int)H; return px;
}
int CtSlSavePng(const char* path, const uint8_t* rgba, int w, int h) {
  FILE* f = fopen(path, "wb"); if (!f) return 0;
  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL); png_infop info = png ? png_create_info_struct(png) : NULL;
  if (!png || !info || setjmp(png_jmpbuf(png))) { if (png) png_destroy_write_struct(&png, info ? &info : NULL); fclose(f); return 0; }
  png_init_io(png, f); png_set_IHDR(png, info, (png_uint_32)w, (png_uint_32)h, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
  png_write_info(png, info);
  for (int y = 0; y < h; y++) png_write_row(png, (png_bytep)(rgba + (size_t)y * w * 4));
  png_write_end(png, NULL); png_destroy_write_struct(&png, &info); fclose(f); return 1;
}
#else
uint8_t* CtSlLoadImage(const char* path, int* w, int* h) { (void)path; (void)w; (void)h; return NULL; }
int CtSlSavePng(const char* path, const uint8_t* rgba, int w, int h) { (void)path; (void)rgba; (void)w; (void)h; return 0; }
#endif
