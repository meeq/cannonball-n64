/***************************************************************************
    N64 pre-boot SEGA splash — implementation.

    Three-phase boot intro:

      Phase 0  Full-screen I8 disclaimer (assets/splash/disclaimer.png, baked
               to disclaimer.sprite). Held at full intensity for 10 s
               (skippable with START), then faded to black via a per-frame
               PRIMITIVE colour that scales the I8 luma: combiner is
               RGB = TEX0 * PRIM, alpha = TEX0_A. The fade-out always plays
               as the transition even on skip.

      Phase 1+ SEGA wordmark animation (existing two-sprite sweep + fade),
               bracketed by a fade-in from black and a final fade-out to
               black. Both edge fades are background-only: rdpq_clear's
               colour lerps black→white (entry) and white→black (exit)
               while no wordmark renders, so the existing white-background
               sprites need no per-frame tinting at the boundaries.

    Two-sprite animation, both 200x80, centered on a 320x240 white
    framebuffer.

      Phase 1  Sweep on the CI8 rainbow sprite (sweep.sprite, baked from
               assets/splash/sweep.png). The source PNG has a transparent
               background and an 18-hue rainbow ramp running through the
               wordmark; at boot we walk the CI8 pixels and re-index each
               x-stripe to its own slot. A 7-tap white→cyan→white pulse
               then slides through those ~45 slots from left to right at
               one step per frame so only the sweep's cyan ramp ever
               paints over the cleared white background.

      Phase 2  Hand-off to the IA4 silhouette sprite (fade.sprite, baked
               from assets/splash/fade.png). The runtime LERPs the RDP's
               PRIMITIVE colour each frame through five keyframes —
               white → pale cyan → cyan → cyan → deep blue — and a
               combiner LERP(PRIM, ONE, TEX0_I) tints the silhouette: I=0
               body pixels show PRIM, brighter AA edges blend toward the
               white background.

      Phase 3  Hold the deepest-blue PRIM for 180 frames (3 s) so the
               final blue-on-white silhouette stays on screen long
               enough for the accompanying SEGA jingle (sega.wav64, ~2.9
               s) to play to completion.

      Phase 4  Reverse-LERP back through the same keyframes (2 frames per
               segment), then hold on PRIM=white for 2 frames so the
               wordmark fades cleanly into the white background before
               the boot menu takes over.

    Holding START at any point during the animation skips the remainder
    of the loop. The current frame still renders and releases its
    framebuffer through rdpq_detach_show; cleanup runs identically to
    the normal exit path. If the user is still holding START when
    boot_menu::run() starts polling, its prompt will detect it and drop
    into the setup UI — same convention boot_menu uses for its own
    held-START prompt window.

    Audio
    -----
    Audio::init() runs *after* the splash in main(), so the splash owns
    its own short-lived audio + mixer bringup: audio_init(22050, 4) and
    mixer_init(2) at run() entry, wav64_play at the fade-phase boundary,
    audio_can_write/mixer_poll inside the per-frame loop to feed the AI
    buffers, and mixer_close / audio_close on the way out so Audio::init
    finds the subsystem in a clean state.

    Cache management
    ----------------
    The sweep sprite's pixel buffer (sprite_load → asset_load malloc) and
    the per-frame TLUT scratch live in cached CPU memory. We write to
    them through the normal cached pointer and call
    data_cache_hit_writeback on the touched range before the RDP DMA
    reads it (once after the pixel re-index, once per frame for the
    TLUT). Going through UncachedAddr while the cache still holds
    shadow values trips ares' "uncached write to cached line" warning
    and risks any later cached read seeing stale bytes — explicit
    writeback sidesteps both. The fade sprite is IA4 and needs no TLUT
    at all; tint comes entirely from the per-frame PRIM colour.
***************************************************************************/

#include "splash.hpp"
#include "input.hpp"

#include <libdragon.h>
#include <cstdint>
#include <cstddef>

namespace
{
    // ---- layout ----------------------------------------------------------

    constexpr int SCREEN_W = 320;
    constexpr int SCREEN_H = 240;
    constexpr int SPRITE_W = 200;
    constexpr int SPRITE_H = 80;
    constexpr int SPRITE_X = (SCREEN_W - SPRITE_W) / 2;  // 60
    constexpr int SPRITE_Y = (SCREEN_H - SPRITE_H) / 2;  // 80

    constexpr uint32_t WHITE_RGB = 0xFFFFFF;
    constexpr uint32_t BLACK_RGB = 0x000000;

    // ---- disclaimer phase ----------------------------------------------------

    constexpr int DISC_HOLD_FRAMES    = 600;   // 10 s @ 60 fps
    constexpr int DISC_FADEOUT_FRAMES = 30;    // 0.5 s

    // ---- splash background fades --------------------------------------------

    constexpr int SPLASH_FADEIN_FRAMES  = 30;  // black → white before sweep
    constexpr int SPLASH_FADEOUT_FRAMES = 30;  // white → black after fade-out

    // ---- sweep phase -----------------------------------------------------

    constexpr int  SWEEP_LEN       = 7;
    constexpr int  N_STRIPES_MAX   = 56;
    constexpr int  FRAMES_PER_STEP = 1;     // 1 frame per sweep step (~1 s)

    constexpr uint32_t SWEEP_SHAPE[SWEEP_LEN] = {
        0xFFFFFF, 0xB4FFFF, 0x48FFFF, 0x00DFFF, 0x48FFFF, 0xB4FFFF, 0xFFFFFF,
    //   white      pale      mid       cyan      mid       pale      white
    //   edge       cyan      cyan      PEAK      cyan      cyan      edge
    };

    // Sweep-sprite slot layout after re-indexing:
    //   0       = bg (transparent — TLUT alpha bit cleared so the RDP
    //             alpha-compare kills these texels and the cleared white
    //             framebuffer shows through)
    //   1..N    = one slot per x-stripe, left-to-right
    constexpr uint8_t SLOT_BG          = 0;
    constexpr uint8_t SLOT_STRIPE_BASE = 1;

    // ---- fade phase ------------------------------------------------------

    // Fade keyframes — cyan is listed twice so segment 2 dwells on it.
    constexpr int FADE_KEYFRAMES = 5;
    constexpr uint32_t FADE_COLORS[FADE_KEYFRAMES] = {
        0xFFFFFF,  // 0: white
        0xB0FFF0,  // 1: pale cyan
        0x00DFE0,  // 2: cyan
        0x006FE0,  // 3: dark cyan
        0x0000FC,  // 4: deep blue
    };
    constexpr int FADE_SEGMENTS = FADE_KEYFRAMES - 1;  // 4 LERP segments

    constexpr int FRAMES_PER_FADEIN_SEG  = 4;
    constexpr int FRAMES_PER_FADEOUT_SEG = 2;
    constexpr int WHITE_HOLD             = 2;
    constexpr int FINAL_HOLD             = 180;        // 3 seconds on deep blue
    constexpr int SWEEP_TO_FADE_GAP      = 2;          // blank-white pause

    constexpr int FADEIN_FRAMES  = FADE_SEGMENTS * FRAMES_PER_FADEIN_SEG;
    constexpr int FADEOUT_FRAMES = FADE_SEGMENTS * FRAMES_PER_FADEOUT_SEG
                                 + WHITE_HOLD;

    // ---- audio -----------------------------------------------------------

    constexpr int AUDIO_RATE     = 22050;     // wav64 was resampled to this
    constexpr int AUDIO_BUFFERS  = 4;         // ~160 ms slack at 22050 Hz
    constexpr int AUDIO_MIXER_CH = 2;         // stereo wav64 uses ch 0 + 1

    // Largest TLUT we ever upload — sweep phase only; the fade phase is
    // IA4 and untouched here.
    constexpr int SCRATCH_TLUT_ENTRIES = SLOT_STRIPE_BASE + N_STRIPES_MAX;

    inline uint16_t rgb_to_rgba16(uint32_t rgb)
    {
        const uint8_t r = (uint8_t)((rgb >> 16) & 0xFF);
        const uint8_t g = (uint8_t)((rgb >>  8) & 0xFF);
        const uint8_t b = (uint8_t)( rgb        & 0xFF);
        return color_to_packed16(RGBA32(r, g, b, 0xFF));
    }

    inline color_t rgb_to_color(uint32_t rgb)
    {
        return RGBA32((rgb >> 16) & 0xFF,
                      (rgb >>  8) & 0xFF,
                      ( rgb     ) & 0xFF,
                      0xFF);
    }

    // Locate the source bg slot in the freshly-baked sweep TLUT — the
    // RGBA16 entry whose alpha bit is clear, set there because the input
    // pixel was fully transparent. mksprite emits exactly one such entry.
    int find_bg_slot(const uint16_t* tlut, int n)
    {
        for (int i = 0; i < n; ++i)
            if ((tlut[i] & 1) == 0) return i;
        return -1;
    }

    // Walk the sweep sprite's CI8 pixel buffer once, mapping every hue
    // pixel (anything not the original bg slot) to a unique stripe slot
    // keyed by x. Returns the number of stripes detected. The caller is
    // responsible for the writeback that publishes the new indices to
    // RDRAM before the first blit.
    int reindex_pixels(uint8_t* data, int W, int H, int stride,
                       uint8_t orig_bg_slot)
    {
        int n_stripes     = 0;
        int last_hue_slot = -1;
        for (int x = 0; x < W; ++x)
        {
            int hue_at_x = -1;
            for (int y = 0; y < H; ++y)
            {
                const int idx = data[y * stride + x];
                if (idx != orig_bg_slot) { hue_at_x = idx; break; }
            }
            if (hue_at_x < 0)
            {
                // Pure background column.
                for (int y = 0; y < H; ++y)
                    if (data[y * stride + x] == orig_bg_slot)
                        data[y * stride + x] = SLOT_BG;
                continue;
            }
            if (hue_at_x != last_hue_slot)
            {
                ++n_stripes;
                last_hue_slot = hue_at_x;
            }
            assertf(n_stripes <= N_STRIPES_MAX,
                "splash: sweep.sprite has more than %d stripes (raise N_STRIPES_MAX)",
                N_STRIPES_MAX);
            const uint8_t new_slot = (uint8_t)(SLOT_STRIPE_BASE + (n_stripes - 1));
            for (int y = 0; y < H; ++y)
            {
                const int idx = data[y * stride + x];
                data[y * stride + x] =
                    (idx == orig_bg_slot) ? SLOT_BG : new_slot;
            }
        }
        return n_stripes;
    }

    // Phase 1 — write the sweep TLUT into the scratch. Slot 0 stays
    // alpha=0 (transparent); the wave colours fill the stripe slots.
    void compute_sweep_palette(int f, int n_stripes, uint16_t* tlut)
    {
        const int step       = f / FRAMES_PER_STEP;
        const int sweep_left = step - (SWEEP_LEN - 1);
        for (int i = 0; i < n_stripes; ++i)
        {
            const uint32_t rgb =
                (i >= sweep_left && i < sweep_left + SWEEP_LEN)
                    ? SWEEP_SHAPE[i - sweep_left]
                    : WHITE_RGB;
            tlut[SLOT_STRIPE_BASE + i] = rgb_to_rgba16(rgb);
        }
        tlut[SLOT_BG] = 0;  // alpha bit clear → killed by alphacompare(1)
    }

    inline uint8_t lerp_u8(uint8_t a, uint8_t b, int num, int den)
    {
        return (uint8_t)((int)a + ((int)b - (int)a) * num / den);
    }

    inline color_t lerp_rgb(uint32_t a, uint32_t b, int num, int den)
    {
        const uint8_t ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
        const uint8_t br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
        return RGBA32(lerp_u8(ar, br, num, den),
                      lerp_u8(ag, bg, num, den),
                      lerp_u8(ab, bb, num, den),
                      0xFF);
    }

    // Fade-in tint at frame rel ∈ [0, FADEIN_FRAMES). Walks the keyframes
    // forward, interpolating linearly within each FRAMES_PER_FADEIN_SEG-
    // frame segment.
    color_t fadein_color(int rel)
    {
        const int seg = rel / FRAMES_PER_FADEIN_SEG;
        const int sub = rel % FRAMES_PER_FADEIN_SEG;
        return lerp_rgb(FADE_COLORS[seg], FADE_COLORS[seg + 1],
                        sub, FRAMES_PER_FADEIN_SEG);
    }

    // Fade-out tint. First FADE_SEGMENTS*FRAMES_PER_FADEOUT_SEG frames
    // walk the keyframes in reverse (deep blue → white); the remaining
    // WHITE_HOLD frames stay on PRIM=white so the wordmark dissolves
    // cleanly into the white background.
    color_t fadeout_color(int rel)
    {
        const int reverse_window = FADE_SEGMENTS * FRAMES_PER_FADEOUT_SEG;
        if (rel >= reverse_window)
            return rgb_to_color(WHITE_RGB);
        const int seg = rel / FRAMES_PER_FADEOUT_SEG;
        const int sub = rel % FRAMES_PER_FADEOUT_SEG;
        // Walk segments end→start: (k-seg)→(k-seg-1) where k = FADE_SEGMENTS.
        return lerp_rgb(FADE_COLORS[FADE_SEGMENTS - seg],
                        FADE_COLORS[FADE_SEGMENTS - seg - 1],
                        sub, FRAMES_PER_FADEOUT_SEG);
    }

    // Round a byte count up to a full 16-byte cache line so
    // data_cache_hit_writeback covers every line the touched range
    // straddles.
    inline size_t cacheline_round_up(size_t n)
    {
        return (n + 15u) & ~size_t{15};
    }
}

namespace n64 { namespace splash
{

void run()
{
    // ---- audio bringup ---------------------------------------------------
    // Audio::init() hasn't run yet — own the audio + mixer here and tear
    // them back down before returning so Audio::init() can claim them
    // fresh. The mixer runs through the disclaimer phase too (only silence
    // until wav64_play hits at the splash fade boundary) so audio_can_write
    // never stalls the AI ring.

    audio_init(AUDIO_RATE, AUDIO_BUFFERS);
    mixer_init(AUDIO_MIXER_CH);

    wav64_t splash_wav;
    wav64_open(&splash_wav, "rom:/splash/sega.wav64");

    // ---- disclaimer sprite: load (I8) ------------------------------------

    sprite_t* disc_sprite = sprite_load("rom:/splash/disclaimer.sprite");
    assertf(disc_sprite, "splash: sprite_load(rom:/splash/disclaimer.sprite) failed");
    surface_t disc_pix = sprite_get_pixels(disc_sprite);

    // Disclaimer combiner: result.rgb = TEX0 * PRIM, result.alpha = TEX0_A.
    // PRIM=white passes the I8 luma through unchanged; PRIM=black scales
    // every channel to 0 so the fade-out lands cleanly on black.
    const rdpq_combiner_t disc_combiner =
        RDPQ_COMBINER1((TEX0, 0, PRIM, 0), (0, 0, 0, TEX0));

    const color_t clear_black = rgb_to_color(BLACK_RGB);
    const color_t clear_white = rgb_to_color(WHITE_RGB);

    // ---- disclaimer phase: hold + fade-out -------------------------------
    //
    // START during the hold accelerates straight into the fade-out by
    // rewriting hold_end to the current frame; the transition itself plays
    // to completion so the cut into the splash fade-in (which starts at
    // black) doesn't pop.

    int disc_hold_end = DISC_HOLD_FRAMES;
    int f = 0;
    while (true)
    {
        const int total = disc_hold_end + DISC_FADEOUT_FRAMES;
        if (f >= total) break;

        surface_t* fb = display_get();
        input.poll();
        const bool skip = input.is_pressed(Input::START);

        while (audio_can_write())
        {
            short* buf = audio_write_begin();
            mixer_poll(buf, audio_get_buffer_length());
            audio_write_end();
        }

        rdpq_attach(fb, NULL);
        rdpq_clear(clear_black);
        rdpq_set_mode_standard();
        rdpq_mode_combiner(disc_combiner);

        color_t prim;
        if (f < disc_hold_end)
        {
            prim = rgb_to_color(WHITE_RGB);
            if (skip)
                disc_hold_end = f + 1;        // start fade-out next frame
        }
        else
        {
            const int rel = f - disc_hold_end;
            const uint8_t v = (uint8_t)(255 - rel * 255 / DISC_FADEOUT_FRAMES);
            prim = RGBA32(v, v, v, 0xFF);
        }
        rdpq_set_prim_color(prim);
        rdpq_tex_blit(&disc_pix, 0, 0, NULL);

        rdpq_detach_show();
        input.frame_done();
        ++f;
    }

    // Drain RDP before freeing the disclaimer's pixel buffer — same hazard
    // as the splash end (see feedback_rdpq_free_needs_drain memory).
    rspq_wait();
    sprite_free(disc_sprite);

    // ---- sweep sprite: load + re-index -----------------------------------

    sprite_t* sweep_sprite = sprite_load("rom:/splash/sweep.sprite");
    assertf(sweep_sprite, "splash: sprite_load(rom:/splash/sweep.sprite) failed");

    uint16_t* sweep_pal   = sprite_get_palette(sweep_sprite);
    const int sweep_pal_n = sprite_get_palette_used_colors(sweep_sprite);
    const int orig_bg     = find_bg_slot(sweep_pal, sweep_pal_n);
    assertf(orig_bg >= 0,
        "splash: no transparent slot in sweep.sprite TLUT (expected alpha=0 bg)");

    surface_t sweep_pix  = sprite_get_pixels(sweep_sprite);
    uint8_t*  sweep_buf  = (uint8_t*)sweep_pix.buffer;
    const int n_stripes  = reindex_pixels(
        sweep_buf, sweep_pix.width, sweep_pix.height, sweep_pix.stride,
        (uint8_t)orig_bg);
    // Publish the re-indexed pixels to RDRAM so rdpq_tex_blit's DMA reads
    // the new indices. CI8 stride matches width (200), and surface_make
    // returns 16-byte-aligned buffers, so the whole range is cache-line
    // aligned.
    data_cache_hit_writeback(sweep_buf,
                             (size_t)sweep_pix.height * sweep_pix.stride);

    // ---- fade sprite: load (IA4, no TLUT needed) -------------------------

    sprite_t* fade_sprite = sprite_load("rom:/splash/fade.sprite");
    assertf(fade_sprite, "splash: sprite_load(rom:/splash/fade.sprite) failed");
    surface_t fade_pix    = sprite_get_pixels(fade_sprite);

    // ---- splash phase boundaries -----------------------------------------
    //
    // Background fade-in (black → white) brackets the existing animation in
    // front, and a fade-out (white → black) brackets it at the end. Both
    // edge phases skip sprite rendering — the cleared framebuffer is all
    // that's on screen.

    const int sweep_steps      = n_stripes + SWEEP_LEN - 1;
    const int phase_fadein_bg_end = SPLASH_FADEIN_FRAMES;
    const int phase_sweep_end  = phase_fadein_bg_end + sweep_steps * FRAMES_PER_STEP;
    const int phase_gap_end    = phase_sweep_end + SWEEP_TO_FADE_GAP;
    const int phase_fadein_end = phase_gap_end + FADEIN_FRAMES;
    const int phase_hold_end   = phase_fadein_end + FINAL_HOLD;
    const int phase_fadeout_end = phase_hold_end + FADEOUT_FRAMES;
    const int total_frames     = phase_fadeout_end + SPLASH_FADEOUT_FRAMES;

    const int sweep_tlut_count = SLOT_STRIPE_BASE + n_stripes;

    alignas(16) uint16_t scratch_tlut[SCRATCH_TLUT_ENTRIES];

    const size_t sweep_wb_bytes =
        cacheline_round_up((size_t)sweep_tlut_count * sizeof(uint16_t));

    // Combiner used in the fade phase: result.rgb = LERP(PRIM, 1, TEX0_I),
    // result.alpha = TEX0_A. The TEX0_A pipe drives rdpq_mode_alphacompare
    // so fully-transparent IA4 texels (the area around the wordmark) are
    // killed and the cleared white shows through.
    const rdpq_combiner_t fade_combiner =
        RDPQ_COMBINER1((1, PRIM, TEX0, PRIM), (0, 0, 0, TEX0));

    for (int sf = 0; sf < total_frames; ++sf)
    {
        surface_t* fb = display_get();

        // Held START skips the rest of the intro. We still render this
        // frame and release the framebuffer through rdpq_detach_show
        // before breaking — otherwise display_get's bookkeeping leaves
        // the buffer claimed and the boot menu's next display_get blocks.
        // Same convention boot_menu's prompt_for_setup uses.
        input.poll();
        const bool skip = input.is_pressed(Input::START);

        // Start the jingle exactly when the fade phase begins (after the
        // brief blank-white pause that follows the sweep).
        if (sf == phase_gap_end)
            wav64_play(&splash_wav, 0);

        // Drain any empty AI buffers so the mixer keeps the stream flowing.
        // Once wav64_play has run, mixer_poll emits the jingle's samples;
        // before that it fills the buffers with silence.
        while (audio_can_write())
        {
            short* buf = audio_write_begin();
            mixer_poll(buf, audio_get_buffer_length());
            audio_write_end();
        }

        // Pick the per-frame clear colour. White through the whole wordmark
        // animation; lerped to black on either side for the bracket fades.
        color_t clear;
        if (sf < phase_fadein_bg_end)
        {
            const uint8_t v =
                (uint8_t)(sf * 255 / SPLASH_FADEIN_FRAMES);
            clear = RGBA32(v, v, v, 0xFF);
        }
        else if (sf < phase_fadeout_end)
        {
            clear = clear_white;
        }
        else
        {
            const int rel = sf - phase_fadeout_end;
            const uint8_t v =
                (uint8_t)(255 - rel * 255 / SPLASH_FADEOUT_FRAMES);
            clear = RGBA32(v, v, v, 0xFF);
        }

        rdpq_attach(fb, NULL);
        rdpq_clear(clear);

        if (sf >= phase_fadein_bg_end && sf < phase_fadeout_end)
        {
            rdpq_set_mode_standard();
            rdpq_mode_alphacompare(1);   // kill texels whose 1-bit alpha is 0

            if (sf < phase_sweep_end)
            {
                const int rel = sf - phase_fadein_bg_end;
                rdpq_mode_tlut(TLUT_RGBA16);
                compute_sweep_palette(rel, n_stripes, scratch_tlut);
                data_cache_hit_writeback(scratch_tlut, sweep_wb_bytes);
                rdpq_tex_upload_tlut(scratch_tlut, 0, sweep_tlut_count);
                rdpq_tex_blit(&sweep_pix, SPRITE_X, SPRITE_Y, NULL);
            }
            else if (sf < phase_gap_end)
            {
                // Blank-white pause between the sweep finishing and the fade
                // starting — the cleared framebuffer is all we need to show.
            }
            else
            {
                color_t prim;
                if (sf < phase_fadein_end)
                    prim = fadein_color(sf - phase_gap_end);
                else if (sf < phase_hold_end)
                    prim = rgb_to_color(FADE_COLORS[FADE_KEYFRAMES - 1]);
                else
                    prim = fadeout_color(sf - phase_hold_end);

                rdpq_mode_combiner(fade_combiner);
                rdpq_set_prim_color(prim);
                rdpq_tex_blit(&fade_pix, SPRITE_X, SPRITE_Y, NULL);
            }
        }

        rdpq_detach_show();

        input.frame_done();
        if (skip) break;
    }

    // rdpq_detach_show is async (see feedback_rdpq_free_needs_drain memory):
    // sprite_free before the last frame's RDP work completes would hand the
    // palette pages back to the heap while the RDP is still DMA-ing them.
    rspq_wait();
    sprite_free(fade_sprite);
    sprite_free(sweep_sprite);

    // Hand the audio + mixer back to the system clean so Audio::init() can
    // claim them.
    wav64_close(&splash_wav);
    mixer_close();
    audio_close();
}

}}
