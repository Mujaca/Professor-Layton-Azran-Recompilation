/* Dekoder fuer Level-5 IMGC00 (.xi).
   Verfahren: 2 = Huffman4Bit, 3 = Huffman8Bit, 4 = RLE, 0 = roh.
   24-bpp-Bilder werden als RGB (BGR -> RGB) ausgegeben. */
#ifndef HLE_IMGC_H
#define HLE_IMGC_H

#include <stdint.h>

#define HLE_ASSET_CAP      0x1E500000u
#define HLE_ASSET_CAP_MAX  0x00300000u

int imgc_decode(const uint8_t *src, uint32_t srclen,
                uint8_t *rgb, int *out_w, int *out_h);

#endif
