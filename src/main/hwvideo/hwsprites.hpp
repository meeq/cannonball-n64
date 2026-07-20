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

    // Allocate the atlas bump pool. Normally fires lazily on first render_rdp,
    // but callable early (after dfs_init) so the MAX-tier size has a chance
    // at a contiguous heap region before later allocs fragment it.
    void atlas_init();

private:
    // Clip values.
    uint16_t x1, x2;

    // Sega arcade hardware was 128 sprites @ 16 bytes each (0x400 bytes).
    // We grew to 256 slots after JUMP_ENTRIES_TOTAL in osprites bumped
    // past 128 to fix overpass scenery silent drops. Cap is the 12-bit
    // write_sprite16 address mask in video.cpp (& 0xfff = 4096 bytes =
    // 256 sprite slots). See SPRITE_ENTRIES.
    static const uint16_t SPRITE_RAM_SIZE = 256 * 8;
    static const uint32_t SPRITES_LENGTH = 0x100000 >> 2;
    static const uint16_t COLOR_BASE = 0x800;

    // Cart PI address of /sprites/sprites_native.bin (1 MiB pre-byte-swapped
    // sprite ROM, BE on disk so PI-DMA into RAM reproduces the legacy
    // sprites[] array exactly). Resolved at atlas_init() via dfs_rom_addr.
    // The CPU EOR-walk spillover path DMAs a 256 KiB bank slice on demand
    // into a static .cpp-local scratch instead of holding 1 MiB resident.
    uint32_t sprites_pi_addr;

    // Two halves of RAM
    uint16_t ram[SPRITE_RAM_SIZE];
    uint16_t ramBuff[SPRITE_RAM_SIZE];

    // Sprite atlas cache (N64 RDP path). Open-addressed hashmap keyed by
    // (bank, addr, height, pitch); values point into a segmented bump pool.
    //
    // Pool layout: ATLAS_SEGMENTS fixed-size segments forming a ring. New
    // extracts bump into the current segment; when it fills, we drain queued
    // RDP work, advance to the next segment, and tombstone any hash entries
    // pointing into the new-current segment. This converts the old wholesale
    // atlas_reset() (re-extracting every visible sprite = 14-30 ms stall) into
    // a segment retire that only invalidates entries in one quarter of the
    // pool (~5 ms re-extract for the affected subset).
    //
    // Hash deletion via tombstones (key=1). Real keys always have bit 63 set
    // (see hwsprites_atlas_key), so 0 (empty) and 1 (tombstone) are free
    // sentinels. Probe loop treats tombstones as "keep walking" but remembers
    // the first as the insertion candidate, so the table stays usable after
    // many retires without needing a compact rebuild.
    static constexpr uint32_t ATLAS_CAPACITY   = 1024;       // power of 2
    // ATLAS_SEGMENTS must be a power of 2 (we mask, not modulo). 4 is the
    // sweet spot — fewer segments means each retire invalidates more entries
    // (back toward today's wholesale-reset cost); more means each segment is
    // small enough that working-set-per-segment hit rate drops.
    static constexpr uint32_t ATLAS_SEGMENTS   = 4;
    // Atlas pool size is picked at atlas_init() from get_memory_size():
    //   8 MiB (Expansion Pak)  -> ATLAS_POOL_BYTES_EXPANSION (2 MiB)
    //   4 MiB (base console)   -> ATLAS_POOL_BYTES_BASE      (512 KiB)
    // Single value per session — no MIN/MAX fallback at runtime, just a
    // one-time branch on detected RAM.
    // 4 MiB sizing: 512 KiB total / 4 segments = 128 KiB/segment. The largest
    // OutRun sprite (max width 32 words × 8 = 256 px, max height 256 rows,
    // CI4 = 32 KiB) fits in one segment with room for several siblings, so
    // segment retires invalidate far fewer working-set entries than at 64
    // KiB/segment. Sized against the 469 KiB of free heap measured at
    // post-audio-prime on 4 MiB — leaves ~213 KiB cushion for any runtime
    // allocations the engine still makes lazily.
    static constexpr uint32_t ATLAS_POOL_BYTES_BASE      = 512u << 10;        // 512 KiB
    static constexpr uint32_t ATLAS_POOL_BYTES_EXPANSION = 2u << 20;          // 2 MiB
    struct AtlasEntry
    {
        uint64_t key;     // 0 = empty, 1 = tombstone (retired-segment slot)
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
    uint32_t   atlas_pool_bytes;     // chosen at atlas_init() from get_memory_size
    uint32_t   atlas_segment_bytes;  // atlas_pool_bytes / ATLAS_SEGMENTS
    uint32_t   atlas_segment_used[ATLAS_SEGMENTS];  // bump cursor per segment
    uint32_t   atlas_current_segment;               // 0..ATLAS_SEGMENTS-1
    uint32_t   atlas_extracts;
    uint32_t   atlas_hits;
    // atlas_overflows: incremented on drain-forced stalls — segment retires
    // that called rspq_wait() because LOAD_BLOCKs were queued, plus the rare
    // hash-table-full fallback. Drives the OUT/PLS "spr ovf" column. Pass-1
    // prepass retires don't count (drain_on_overflow=false).
    uint32_t   atlas_overflows;
    uint32_t   atlas_segment_retirements;  // all retires, drained or not

    // Ring of scratch TLUTs used by the shadow body pass: each entry is a copy
    // of sprite_tlut[color*16] with slot 10 zeroed (so the shadow texel becomes
    // transparent and only the body pixels survive alpha-compare). The ring
    // wraps per-frame; rdpq reads from the physical address at execute time,
    // so we never reuse a slot within a single render_rdp() call.
    //
    // alignas(16) = one full VR4300 D-cache line: the CPU writes this array
    // ONLY through an UncachedAddr alias (render_rdp's scratch_uc), so no
    // cacheline it occupies may ever be shared with a cached-accessed member.
    // At alignas(8) the array could start mid-line, putting its first entries
    // on the same line as the atlas counters above — a cached counter write
    // dirties that line, and its eventual writeback clobbers the uncached
    // TLUT bytes (intermittently wrong colors in a shadow body's palette).
    // The size is a multiple of 16, so an aligned start also keeps the
    // members below off the array's last line.
    static constexpr uint32_t SHADOW_TLUT_RING = 128;
    alignas(16) uint16_t shadow_body_tluts[SHADOW_TLUT_RING * 16];
    uint32_t   shadow_body_ring_idx;

    void atlas_reset();
    // Advance current segment to (current+1) mod ATLAS_SEGMENTS. Walks the
    // hash and tombstones any real entry whose ci4 falls in the new-current
    // segment; resets that segment's bump cursor.
    void atlas_advance_segment();
    // Bump-allocate `bytes` from the current segment. When the segment
    // doesn't have room, drain queued RDP work (when drain_on_overflow=true)
    // and advance to the next segment. drain_on_overflow=false is only safe
    // when the caller has NOT yet emitted any LOAD_BLOCK against the pool
    // snapshot the allocator is about to rewrite — used by render_rdp's pass
    // 1 prepass. Returns NULL only if the request is structurally impossible
    // (asserts on bytes > segment_bytes — single sprite that doesn't fit one
    // segment is a hard error).
    uint8_t* atlas_pool_alloc(uint32_t bytes, bool drain_on_overflow);
    const AtlasEntry* atlas_get_or_extract(uint16_t bank, uint16_t addr,
                                           uint16_t height, int16_t pitch,
                                           bool flip, uint16_t vzoom,
                                           bool drain_on_overflow);
};

