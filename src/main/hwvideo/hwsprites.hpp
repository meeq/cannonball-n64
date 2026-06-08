#pragma once

#include "stdint.hpp"

class video;

class hwsprites
{
public:
    hwsprites();
    ~hwsprites();
    void init(const uint8_t*);
    void reset();
    void set_x_clip(bool);
    void swap();
    uint8_t read(const uint16_t adr);
    void write(const uint16_t adr, const uint16_t data);

    // RDP sprite renderer. For each visible sprite at the requested priority:
    // pre-extracts a CI4 atlas (cached by bank/addr/height/pitch), uploads
    // the sprite TLUT slot, and emits one rdpq_tex_blit per sprite with
    // hzoom/vzoom scale + flip via blit parms. Shadow-flagged sprites use a
    // two-pass (darken + body) pipeline so the shadow correctly darkens the
    // composited framebuffer underneath rather than just the engine scratch.
    void render_rdp(uint8_t priority, const uint16_t* sprite_tlut,
                    int x_offset, int y_offset);

    // Diagnostics for the atlas cache (FPS overlay).
    uint32_t atlas_extract_count() const;
    uint32_t atlas_hit_count() const;
    uint32_t atlas_overflow_count() const;
    uint32_t atlas_used_bytes() const;

private:
    // Clip values.
    uint16_t x1, x2;

    // 128 sprites, 16 bytes each (0x400)
    static const uint16_t SPRITE_RAM_SIZE = 128 * 8;
    static const uint32_t SPRITES_LENGTH = 0x100000 >> 2;
    static const uint16_t COLOR_BASE = 0x800;

    // 8-byte aligned so atlas extraction reads source words from the same
    // alignment the rdpq DMAs expect for derived CI4 atlas blocks.
    alignas(8) uint32_t sprites[SPRITES_LENGTH]; // Converted sprites

    // Two halves of RAM
    uint16_t ram[SPRITE_RAM_SIZE];
    uint16_t ramBuff[SPRITE_RAM_SIZE];

    // Sprite atlas cache (N64 RDP path). Open-addressed hashmap keyed by
    // (bank, addr, height, pitch); values point into a single contiguous
    // bump pool sized for ~typical OutRun working set. On pool overflow the
    // whole cache is reset (data is re-extracted on next miss) so we never
    // need a true LRU walk.
    static constexpr uint32_t ATLAS_CAPACITY   = 1024;       // power of 2
    // 1 MiB. Boot heap reports ~3 MiB free but the heap is fragmented at
    // atlas_init time (ROM + sprites[] + rspq + audio + framebuffers all
    // allocated first), so memalign returns NULL for sizes >1 MiB while
    // 1 MiB succeeds reliably. Each ovf event costs a ~20ms frame to
    // re-extract every visible sprite — see
    // project_spr_spike_atlas_overflow. 1 MiB cuts the ovf rate ~6x vs
    // the original 512 KiB pool.
    static constexpr uint32_t ATLAS_POOL_BYTES = 1024u << 10;
    struct AtlasEntry
    {
        uint64_t key;     // 0 = empty
        uint8_t* ci4;     // 8-byte aligned, ci4_stride * h bytes
        uint16_t w;       // native pixel width (multiple of 8)
        uint16_t h;       // native pixel height
        uint8_t  has_shadow; // 1 if any pixel uses slot 0xa, else 0
        // Tight bounding box of slot-0xa pixels (inclusive sx0/sy0, exclusive
        // sx1/sy1). Lets render_rdp shrink the 2-cycle shadow rect to just the
        // shadow silhouette region instead of the full sprite footprint.
        // Undefined when has_shadow == 0.
        uint16_t shadow_x0, shadow_y0, shadow_x1, shadow_y1;
    };
    AtlasEntry atlas_entries[ATLAS_CAPACITY];
    uint8_t*   atlas_pool;
    uint32_t   atlas_used;
    uint32_t   atlas_extracts;
    uint32_t   atlas_hits;
    uint32_t   atlas_overflows;

    // Ring of scratch TLUTs used by the shadow body pass: each entry is a copy
    // of sprite_tlut[color*16] with slot 10 zeroed (so the shadow texel becomes
    // transparent and only the body pixels survive alpha-compare). The ring
    // wraps per-frame; rdpq reads from the physical address at execute time,
    // so we never reuse a slot within a single render_rdp() call.
    static constexpr uint32_t SHADOW_TLUT_RING = 128;
    alignas(8) uint16_t shadow_body_tluts[SHADOW_TLUT_RING * 16];
    uint32_t   shadow_body_ring_idx;

    void atlas_init();
    void atlas_reset();
    const AtlasEntry* atlas_get_or_extract(uint16_t bank, uint16_t addr,
                                           uint16_t height, int16_t pitch,
                                           bool flip, uint16_t vzoom);
};

