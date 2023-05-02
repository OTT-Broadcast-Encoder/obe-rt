#ifndef YUV422P10LE_H
#define YUV422P10LE_H

#include <stdint.h>

#define YUV422P10LE_BOX_HEIGHT 12 /* must be a multiple of six (represents pixel width of each box drawn ) */

#ifdef __cplusplus
extern "C" {
#endif

struct YUV422P10LE_painter_s
{
        uint16_t *frame;
        uint16_t *ptr;

        int widthPixels;
        int heightPixels;
        int strideBytes;
	int interlaced;
};

/* KL paint 6 pixels in a single point */
#if 0
__inline__ void YUV422P10LE_draw_6_pixels(uint32_t *addr, uint32_t *coloring);
__inline__ void YUV422P10LE_draw_box(uint32_t *frame_addr, uint32_t stride, int color, int interlaced);
__inline__ void YUV422P10LE_draw_box_at(uint32_t *frame_addr, uint32_t stride, int color, int x, int y, int interlaced);
void YUV422P10LE_write_32bit_value(void *frame_bytes, uint32_t stride, uint32_t value, uint32_t lineNr, int interlaced);

uint32_t YUV422P10LE_read_32bit_value(void *frame_bytes, uint32_t stride, uint32_t lineNr, double scalefactor);
#endif

/* Write text to V210 packed planes */
int  YUV422P10LE_painter_reset(struct YUV422P10LE_painter_s *ctx, uint8_t *frame, int widthPixels, int heightPixels, int strideBytes);
void YUV422P10LE_painter_draw_ascii_at(struct YUV422P10LE_painter_s *ctx, int x, int y, const char *str);

#ifdef __cplusplus
};
#endif

#endif /* YUV422P10LE_H */
