// AUTO-GENERATED schema — describes the lookup-table emitted by
// tools/bake-sprites. The runtime binary-searches the sorted array by
// (bank, flip, pitch, addr) and PI-DMAs blob_offset..+blob_size_from_w_h
// out of the cart blob into the bump pool.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HwspritesBakedEntry {
    uint8_t  bank;            // 0..7
    uint8_t  flip;            // 0 | 1
    int16_t  pitch;           // signed row stride
    uint16_t addr;            // base word offset within bank
    uint16_t w;               // baked CI4 pixel width  (multiple of 8)
    uint16_t h;               // baked CI4 pixel height (max source_h seen)
    uint8_t  has_shadow;
    uint16_t shadow_x0, shadow_y0, shadow_x1, shadow_y1;
    uint32_t blob_offset;     // byte offset into sprite_atlas.bin
} HwspritesBakedEntry;

extern const uint32_t            hwsprites_baked_count;
extern const uint32_t            hwsprites_baked_blob_bytes;
extern const HwspritesBakedEntry hwsprites_baked_index[];

#ifdef __cplusplus
}
#endif
