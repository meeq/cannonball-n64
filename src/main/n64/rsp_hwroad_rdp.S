####################################################################
# rsp_hwroad_rdp.S — RSP overlay that emits hwroad foreground Run
# records for the RDP path (see src/main/n64/hwroad_rdp.cpp).
#
# Each per-row Run is {x_end:u16, color5551:u16} = 4 bytes; up to
# MAX_RUNS_PER_ROW = 64 per row. The emit phase (CPU-side, in
# emit_foreground_lores_rdp) walks the run list and emits one
# rdpq_fill_rectangle per visible run.
#
# This file replaces the previous CI4-mask packer. The scan is scalar
# for now — a vectorised lpv/vne/cfc2 inner loop was prototyped but
# hit a correctness issue, and the perf budget for case-0/3 (single-
# road frames) is comfortably met by the scalar path running async
# behind the rest of prepare_frame. The win over the CPU build comes
# from the async dispatch alone, not from per-byte throughput.
#
# Command 0 — HWRoadRDP_BuildRuns:
#   a0 = num_scanlines (low 16 bits)          — cmd word top byte stripped
#   a1 = per-row descriptor base in RDRAM     — DescriptorRDP[]
#   a2 = frame state base in RDRAM            — FrameStateRDP
#
# Per-row descriptor layout (32 bytes, see DescriptorRDP in
# hwroad_rdp_rsp.cpp — keep in sync):
#   0x00  u8   kind          0=SKIP, 1=OOB_ONLY, 2=DRAW
#   0x01  u8   ctrl_case     0..3
#   0x02  u16  span_start
#   0x04  u16  span_end
#   0x06  u16  s0s
#   0x08  u16  s0e
#   0x0a  u16  s1s
#   0x0c  u16  s1e
#   0x0e  u16  c_oob_color   RGBA5551
#   0x10  u32  tlut_phys     RDRAM phys of 16-entry RGBA5551 TLUT (32B)
#   0x14  u32  src0_phys     RDRAM phys of src0 + t0_offset
#   0x18  u32  src1_phys     RDRAM phys of src1 + t1_offset
#   0x1c  u32  runs_phys     RDRAM phys of runs[y][0]
#
# Frame state layout (128 bytes total, padded):
#   0x00  u8   ctrl
#   0x01  u8   oob_pair       (legacy, unused on runs path)
#   0x02  u16  _pad
#   0x04  u32  n_runs_phys    RDRAM phys of u16[MAX_LINES] writeback
#   0x08  u8[8]  slot8r0      merged_idx[(p<<3)|3] for p in 0..7
#   0x10  u8[8]  slot8r1      merged_idx[(3<<3)|p] for p in 0..7
#   0x18  u8[64] merged_idx   full table, keyed by (p0<<3)|p1
#   0x58  u8[40] _pad
#
# Per-row protocol:
#   1. DMA descriptor (32 B) → DESC.
#   2. If kind != DRAW, write n_runs[y]=0 and advance.
#   3. DMA TLUT (32 B) → TLUT_DMEM.
#   4. ctrl=0 → DMA SRC0, vec_scan(SRC0, slot8r0, s0s, s0e-s0s).
#      ctrl=3 → DMA SRC1, vec_scan(SRC1, slot8r1, s1s, s1e-s1s).
#      ctrl=1/2 → DMA both srcs; build sorted piece bounds in BOUNDS;
#                 walk pieces emitting via per-piece scalar scan
#                 (pair / r0-only / r1-only / c_oob).
#   5. Write n_runs[y] into N_RUNS_OUT DMEM table; DMA runs[] out.
#   6. After all rows, bulk-DMA N_RUNS_OUT to frame_state.n_runs_phys.
#
# vec_scan loop shape (case 0/3 — sentinel-terminated extend):
#   outer:
#     b     = src[i]
#     color = tlut[slot_table[b]]
#     when color changes, close runs[n-1] and open runs[n], n++
#     inner extends i while src[i] == b
#     (sentinel src[len]=0xFF stamped at DMA time terminates the run
#      extend without a per-iter length check; outer-loop test then
#      sees i==len and falls through to the trailing close.)
#
# do_case12 loop shape (per piece — bounds-checked extend):
#   For each sorted piece [lo, hi) inside [span_start, span_end):
#     in0 = lo in [s0s, s0e);  in1 = lo in [s1s, s1e)
#     dispatch on (in0, in1):
#       (1,1) pair    : color = tlut[merged_idx[(b0<<3)|b1]]
#                       extend while p0[x]==b0 && p1[x]==b1
#       (1,0) r0_only : color = tlut[slot8r0[b0]]
#                       extend while p0[x]==b0
#       (0,1) r1_only : color = tlut[slot8r1[b1]]
#                       extend while p1[x]==b1
#       (0,0) c_oob   : emit one c_oob run (no scan)
#     piece extends share n / prev_color so adjacent same-colour pieces
#     coalesce into one run, matching the CPU EMIT macro semantics.
#   Final trailing run is closed at span_end after all pieces walked.
####################################################################

#include <rsp_queue.inc>
#include <rdpq_constants.h>

.set noreorder
.set at

####################################################################
# Data segment — overlay header only.
####################################################################
.data

RSPQ_BeginOverlayHeader
    # 12 bytes = 3 words: (cmd|arg0) + arg1 + arg2 (descriptor base + state base).
    RSPQ_DefineCommand HWRoadRDP_BuildRuns,    12
    # HWRoadRDP_EmitCoobFill: emits the per-row OOB-colour strip via direct
    # RDP fill_rectangle writes. arg0 = coob array RDRAM ptr, arg1 = (x_off<<16)
    # | y_off, arg2 = (W<<16) | n_rows.
    RSPQ_DefineCommand HWRoadRDP_EmitCoobFill, 12
    # HWRoadRDP_EmitRuns: emits per-row body fill_rectangles for the run
    # lists produced by BuildRuns. 16 bytes = 4 words (cmd|arg0) + arg1 +
    # arg2 + arg3. arg0 = emit_row RDRAM ptr; arg1 = n_runs RDRAM ptr;
    # arg2 = runs_base RDRAM ptr; arg3 = (x_off<<16) | y_off.
    RSPQ_DefineCommand HWRoadRDP_EmitRuns,     16
RSPQ_EndOverlayHeader

RSPQ_BeginSavedState
    .quad 0
RSPQ_EndSavedState

####################################################################
# BSS — DMEM working buffers.
####################################################################
.bss

.align 4
DESC:        .space 32          # current per-row descriptor
.align 4
FRAME_STATE: .space 128         # ctrl + oob_pair + n_runs_phys + slot8r0/r1 + merged_idx
.align 4
TLUT_DMEM:   .space 32          # per-row 16-entry RGBA5551 TLUT
.align 8
# SP DMA ignores low 3 bits of RDRAM src addr. We DMA SRC_DMA_BYTES
# = 336 bytes (= 320 max span + 7 bytes misalignment slop, rounded
# up to 8). SRC0/SRC1 sized to 528 for worst-case slack and to
# leave headroom for the post-DMA sentinel store. .align 8 so lpv
# can treat the buffer base as 8-aligned.
SRC0:        .space 528
.align 8
SRC1:        .space 528
.align 4
SRC0_BASE:   .space 4           # SRC0 + (src0_phys & 7); stashed after DMA
.align 4
SRC1_BASE:   .space 4           # SRC1 + (src1_phys & 7)
.align 8
# Per-row runs staging. MAX_RUNS_PER_ROW=64 * sizeof(Run)=4 = 256B.
# Written sequentially by the emit code, DMA'd out at row end.
RUNS:        .space 256
.align 8
# Per-frame n_runs table. One u16 per row; bulk DMA'd to RDRAM at
# the end of the command (after the row loop drains).
N_RUNS_OUT:  .space 512         # 256 rows * 2 = 512B (covers MAX_LINES=224 plus slack)
.align 4
# Per-row piece-walk scratch for do_case12. BOUNDS holds up to 6
# sorted halfword bounds (span_start / s0s / s0e / s1s / s1e / span_end);
# NB_SAVE is the count after pruning duplicates and out-of-range entries.
BOUNDS:      .space 16
NB_SAVE:     .space 4

# Per-row c_oob colour table consumed by HWRoadRDP_EmitCoobFill. CPU
# writes one u16 per row (0xFFFF = inactive, else RGBA5551). Sized for
# MAX_LINES+1 entries with the trailing entry stamped 0xFFFF on the CPU
# side so the walk's closing edge falls out naturally; rounded up to
# 8-byte multiple for SP DMA (232 rows * 2 = 464).
#
# Aliased onto N_RUNS_OUT — DMEM is tight and the two arrays never live
# simultaneously: BuildRuns writes N_RUNS_OUT and DMAs it out before
# sync_runs returns; EmitCoobFill is queued strictly after that drain, so
# N_RUNS_OUT's bytes are dead by the time we DMA the coob table in.
COOB_FILL_DMEM = N_RUNS_OUT
.align 8
# RDP command staging for the coob fill command. Mirrors the upstream
# RDPQ_CMD_STAGING from rsp_rdpq.inc but is private to this overlay so
# we don't have to drag in the rest of that file (triangle, modes, …).
# 0xB0 bytes = 22 8-byte slots — enough for many fill+colour pairs
# before we need to flush via hp_RDPQ_Send. Shared by HWRoadRDP_EmitCoobFill
# and HWRoadRDP_EmitRuns (the two RDP-emitting commands).
COOB_STAGING:    .space 0xB0
.align 4
# Current write position inside COOB_STAGING. Reset to COOB_STAGING base
# at the top of the command and advanced 8 bytes per hp_RDPQ_Write8 call.
COOB_STAGING_PTR: .space 4

# Per-row body-emit state for HWRoadRDP_EmitRuns: 4 B/row × MAX_LINES (224)
# = 896 B. Each row: u16 s_start, u16 c_oob. Aliased onto SRC0 — SRC0+SRC1
# are 528+528 = 1056 B contiguous (SRC0 ends 8-aligned so SRC1's .align 8
# is a no-op), comfortably holding the 896 B EmitRow array. SRC0/SRC1 are
# BuildRuns-only and dead by the time EmitRuns runs as a separate command.
EMIT_ROW_DMEM = SRC0

# Per-row runs DMA-in target for HWRoadRDP_EmitRuns. Up to MAX_RUNS_PER_ROW
# (64) × sizeof(Run) (4) = 256 B. Aliased onto RUNS — BuildRuns' per-row
# staging buffer that is fully DMA'd out before the command returns.
RUN_DMEM = RUNS

# Per-row n_runs DMA-in target for HWRoadRDP_EmitRuns. Aliased onto N_RUNS_OUT
# (the BuildRuns u16 writeback table) — by the time EmitRuns runs, BuildRuns
# has finished writing out and EmitCoobFill has consumed its alias of the
# same bytes (COOB_FILL_DMEM), so the slot is dead and free for reuse.
N_RUNS_DMEM = N_RUNS_OUT

# Row count — matches MAX_LINES in hwroad_rdp_internal.hpp / S16_HEIGHT.
#define MAX_LINES 224

####################################################################
# Code segment.
####################################################################
.text

# Descriptor field offsets — keep in sync with hwroad_rdp_rsp.cpp.
#define DESC_KIND        0x00
#define DESC_CTRL        0x01
#define DESC_SS          0x02
#define DESC_SE          0x04
#define DESC_S0S         0x06
#define DESC_S0E         0x08
#define DESC_S1S         0x0a
#define DESC_S1E         0x0c
#define DESC_C_OOB       0x0e
#define DESC_TLUT_PHYS   0x10
#define DESC_SRC0_PHYS   0x14
#define DESC_SRC1_PHYS   0x18
#define DESC_RUNS_PHYS   0x1c
#define DESC_SIZE        32

# Frame state field offsets.
#define FS_CTRL          0x00
#define FS_OOB_PAIR      0x01
#define FS_N_RUNS_PHYS   0x04
#define FS_SLOT8R0       0x08
#define FS_SLOT8R1       0x10
#define FS_MERGED        0x18

# Run-list cap per row. Must match the CPU side in hwroad_rdp_rsp.cpp.
#define MAX_RUNS_PER_ROW 64

# DMA sizing. 336 B = max span (320) + max misalign (7), rounded to
# multiple of 8.
#define SRC_DMA_BYTES    336

############################################################
# HWRoadRDP_BuildRuns: emit runs[] for all DRAW rows, then bulk-DMA
# the n_runs writeback array.
############################################################
HWRoadRDP_BuildRuns:
    # Stash inputs in callee-saved regs. DMA macros clobber t0..t3,
    # s0, s4. a0's top byte carries the rspq cmd word; mask it.
    andi    s3, a0, 0xFFFF              # rows remaining
    move    s1, a1                       # desc base RDRAM ptr
    move    s2, a2                       # state base RDRAM ptr

    # Row index (used to compute N_RUNS_OUT[y] write offset).
    li      k0, 0                        # k0 = current row index

    # --- DMA frame state once for the whole queue invocation. ---
    move    s0, s2
    li      s4, %lo(FRAME_STATE)
    jal     DMAIn
    li      t0, DMA_SIZE(128, 1)

scanline_loop:
    blez    s3, frame_done
    nop

    # --- DMA the per-row descriptor. -------------------------
    move    s0, s1
    li      s4, %lo(DESC)
    jal     DMAIn
    li      t0, DMA_SIZE(DESC_SIZE, 1)

    # --- kind != DRAW (2)? n_runs[y]=0, advance. -------------
    lbu     t0, %lo(DESC) + DESC_KIND
    li      t1, 2
    beq     t0, t1, .draw_row
    nop
    # Non-DRAW: write zero to N_RUNS_OUT[y].
    sll     t2, k0, 1                    # y * 2
    li      t3, %lo(N_RUNS_OUT)
    addu    t2, t2, t3
    sh      zero, 0(t2)
    j       advance
    nop

.draw_row:
    # --- DMA the per-row TLUT (32 bytes). --------------------
    lw      s0, %lo(DESC) + DESC_TLUT_PHYS
    li      s4, %lo(TLUT_DMEM)
    jal     DMAIn
    li      t0, DMA_SIZE(32, 1)

    # --- Dispatch on ctrl_case. ------------------------------
    lbu     s5, %lo(DESC) + DESC_CTRL    # ctrl_case
    beqz    s5, do_case0
    nop
    li      t0, 3
    beq     s5, t0, do_case3
    nop
    # ctrl=1/2 — stub. Falls through to do_case12.
    j       do_case12
    nop

do_case0:
    # DMA src0. DMAIn adjusts s4 to point at the first requested byte
    # (i.e. s4 = %lo(SRC0) + (src0_phys & 7) on return).
    lw      s0, %lo(DESC) + DESC_SRC0_PHYS
    li      s4, %lo(SRC0)
    jal     DMAIn
    li      t0, DMA_SIZE(SRC_DMA_BYTES, 1)
    sw      s4, %lo(SRC0_BASE)
    # Sentinel: SRC0_BASE[len] = 0xFF. Palette indices are 3-bit (0..7), so
    # 0xFF cannot match any in-band byte — the inner-loop run-extender
    # terminates naturally at the sentinel without per-iter bounds checks.
    lhu     t1, %lo(DESC) + DESC_S0E
    lhu     t2, %lo(DESC) + DESC_S0S
    subu    t1, t1, t2                   # len
    addu    t1, s4, t1                   # &SRC0_BASE[len]
    li      t3, 0xFF
    sb      t3, 0(t1)
    # vec_scan(SRC0_BASE, slot8r0, s0s, len)
    move    v0, s4                       # v0 = src_base (DMEM)
    li      v1, %lo(FRAME_STATE) + FS_SLOT8R0
    lhu     a0, %lo(DESC) + DESC_S0S     # a0 = span_start_x
    lhu     a1, %lo(DESC) + DESC_S0E
    subu    a1, a1, a0                   # a1 = len
    j       vec_scan
    nop

do_case3:
    # DMA src1. DMAIn adjusts s4 to first requested byte.
    lw      s0, %lo(DESC) + DESC_SRC1_PHYS
    li      s4, %lo(SRC1)
    jal     DMAIn
    li      t0, DMA_SIZE(SRC_DMA_BYTES, 1)
    sw      s4, %lo(SRC1_BASE)
    lhu     t1, %lo(DESC) + DESC_S1E
    lhu     t2, %lo(DESC) + DESC_S1S
    subu    t1, t1, t2
    addu    t1, s4, t1
    li      t3, 0xFF
    sb      t3, 0(t1)
    move    v0, s4
    li      v1, %lo(FRAME_STATE) + FS_SLOT8R1
    lhu     a0, %lo(DESC) + DESC_S1S
    lhu     a1, %lo(DESC) + DESC_S1E
    subu    a1, a1, a0
    j       vec_scan
    nop

############################################################
# Register plan for do_case12 piece walk:
#   Persistent (live across inner scanners):
#     t1 = s0s          t2 = s0e
#     t3 = s1s          t4 = s1e
#     v0 = SRC0_BASE    v1 = SRC1_BASE
#     s5 = RUNS         s6 = TLUT_DMEM
#     s7 = MERGED       s8 = SLOT8R0  (slot8r1 = s8 + 8)
#     a3 = n_runs       k1 = prev_color
#     k0 = row index    (preserved for emit_done_with_n_in_t3)
#     t8 = piece index
#
#   Per piece (set in walk loop, read by scanner entry):
#     a1 = lo (piece start screen_x)
#     a2 = hi (piece end screen_x)
#
#   Inside inner scanner (free scratch, clobbered each iter):
#     a0 = i (scan position screen_x)
#     t0, t5, t6, t7  — generic scratch
#     t9              — pair_extend uses for b1
#
# All bounds-build + sort scratch (t-regs) is consumed before the
# persistent-state load; the load freshens t1..t4 from DESC.
############################################################

do_case12:
    # ---- DMA both src rows. ----------------------------------
    lw      s0, %lo(DESC) + DESC_SRC0_PHYS
    li      s4, %lo(SRC0)
    jal     DMAIn
    li      t0, DMA_SIZE(SRC_DMA_BYTES, 1)
    sw      s4, %lo(SRC0_BASE)
    lw      s0, %lo(DESC) + DESC_SRC1_PHYS
    li      s4, %lo(SRC1)
    jal     DMAIn
    li      t0, DMA_SIZE(SRC_DMA_BYTES, 1)
    sw      s4, %lo(SRC1_BASE)

    # ---- Build BOUNDS[] ---------------------------------------
    # bounds[0] = span_start; then push s0s/s0e/s1s/s1e iff strictly
    # inside (span_start, span_end); finally bounds[nb++] = span_end.
    # All build-phase scratch uses t-regs; t1..t4 will be reloaded
    # for the persistent piece-walk state after the sort.
    lhu     t5, %lo(DESC) + DESC_SS          # ss
    lhu     t6, %lo(DESC) + DESC_SE          # se
    li      t8, %lo(BOUNDS)
    sh      t5, 0(t8)                        # bounds[0] = ss
    addiu   t8, t8, 2

    lhu     t7, %lo(DESC) + DESC_S0S
    sltu    t0, t5, t7                       # ss < s0s ?
    sltu    t2, t7, t6                       # s0s < se ?
    and     t0, t0, t2
    beqz    t0, .c12_skip_s0s
    nop
    sh      t7, 0(t8)
    addiu   t8, t8, 2
.c12_skip_s0s:

    lhu     t7, %lo(DESC) + DESC_S0E
    sltu    t0, t5, t7
    sltu    t2, t7, t6
    and     t0, t0, t2
    beqz    t0, .c12_skip_s0e
    nop
    sh      t7, 0(t8)
    addiu   t8, t8, 2
.c12_skip_s0e:

    lhu     t7, %lo(DESC) + DESC_S1S
    sltu    t0, t5, t7
    sltu    t2, t7, t6
    and     t0, t0, t2
    beqz    t0, .c12_skip_s1s
    nop
    sh      t7, 0(t8)
    addiu   t8, t8, 2
.c12_skip_s1s:

    lhu     t7, %lo(DESC) + DESC_S1E
    sltu    t0, t5, t7
    sltu    t2, t7, t6
    and     t0, t0, t2
    beqz    t0, .c12_skip_s1e
    nop
    sh      t7, 0(t8)
    addiu   t8, t8, 2
.c12_skip_s1e:

    sh      t6, 0(t8)                        # bounds[nb] = se
    addiu   t8, t8, 2

    # nb = (t8 - BOUNDS) / 2
    li      t0, %lo(BOUNDS)
    subu    t1, t8, t0
    srl     t1, t1, 1                        # t1 = nb
    sw      t1, %lo(NB_SAVE)

    # ---- Insertion sort BOUNDS[0..nb) -----------------------
    li      t2, 1                            # i = 1
.c12_sort_outer:
    sltu    t0, t2, t1
    beqz    t0, .c12_sort_done
    nop
    sll     t3, t2, 1
    li      t8, %lo(BOUNDS)
    addu    t3, t8, t3
    lhu     t4, 0(t3)                        # v = bounds[i]
    move    t5, t2                           # j = i
.c12_sort_inner:
    blez    t5, .c12_sort_place
    nop
    addiu   t6, t5, -1
    sll     t7, t6, 1
    addu    t7, t8, t7
    lhu     t6, 0(t7)                        # bounds[j-1]
    sltu    t0, t4, t6                       # v < bounds[j-1] ?
    beqz    t0, .c12_sort_place
    nop
    sll     t7, t5, 1
    addu    t7, t8, t7
    sh      t6, 0(t7)                        # bounds[j] = bounds[j-1]
    addiu   t5, t5, -1
    j       .c12_sort_inner
    nop
.c12_sort_place:
    sll     t6, t5, 1
    addu    t6, t8, t6
    sh      t4, 0(t6)                        # bounds[j] = v
    addiu   t2, t2, 1
    j       .c12_sort_outer
    nop
.c12_sort_done:

    # ---- Load persistent piece-walk state -------------------
    li      a3, 0                            # n_runs
    li      k1, 0x10000                      # prev_color sentinel
    lhu     t1, %lo(DESC) + DESC_S0S
    lhu     t2, %lo(DESC) + DESC_S0E
    lhu     t3, %lo(DESC) + DESC_S1S
    lhu     t4, %lo(DESC) + DESC_S1E
    lw      v0, %lo(SRC0_BASE)
    lw      v1, %lo(SRC1_BASE)
    li      s5, %lo(RUNS)
    li      s6, %lo(TLUT_DMEM)
    li      s7, %lo(FRAME_STATE) + FS_MERGED
    li      s8, %lo(FRAME_STATE) + FS_SLOT8R0

    li      t8, 0                            # piece index
.c12_piece_loop:
    lw      t6, %lo(NB_SAVE)
    addiu   t0, t6, -1                       # nb - 1
    sltu    t0, t8, t0
    beqz    t0, .c12_pieces_done
    nop

    sll     t6, t8, 1
    li      t7, %lo(BOUNDS)
    addu    t6, t7, t6
    lhu     a1, 0(t6)                        # lo
    lhu     a2, 2(t6)                        # hi

    sltu    t0, a1, a2
    beqz    t0, .c12_piece_next
    nop

    # in0 = (lo >= s0s) && (lo < s0e)
    sltu    t5, a1, t1                       # lo < s0s ?
    xori    t5, t5, 1
    sltu    t6, a1, t2                       # lo < s0e ?
    and     t5, t5, t6                       # t5 = in0

    # in1 = (lo >= s1s) && (lo < s1e)
    sltu    t6, a1, t3
    xori    t6, t6, 1
    sltu    t7, a1, t4
    and     t6, t6, t7                       # t6 = in1

    # Dispatch on (in0, in1).
    bnez    t5, .c12_in0_set
    nop
    bnez    t6, .c12_piece_r1
    nop
    j       .c12_piece_oob
    nop
.c12_in0_set:
    bnez    t6, .c12_piece_pair
    nop
    j       .c12_piece_r0
    nop

############################################################
# .c12_piece_r0 — road 0 only over [lo, hi).
############################################################
.c12_piece_r0:
    move    a0, a1                           # screen_x = lo
.r0_outer:
    sltu    t0, a0, a2
    beqz    t0, .c12_piece_next
    nop
    subu    t5, a0, t1                       # offset = x - s0s
    addu    t5, v0, t5
    lbu     t6, 0(t5)                        # b
    addu    t5, s8, t6                       # &slot8r0[b]
    lbu     t7, 0(t5)                        # slot
    sll     t7, t7, 1
    addu    t5, s6, t7
    lhu     t7, 0(t5)                        # color
    beq     t7, k1, .r0_extend
    nop
    blez    a3, .r0_open
    nop
    addiu   t5, a3, -1
    sll     t5, t5, 2
    addu    t5, s5, t5
    sh      a0, 0(t5)                        # runs[n-1].x_end = screen_x
.r0_open:
    li      t5, MAX_RUNS_PER_ROW
    sltu    t5, a3, t5
    beqz    t5, .r0_set_prev
    nop
    sll     t5, a3, 2
    addu    t5, s5, t5
    sh      t7, 2(t5)                        # runs[n].color
    addiu   a3, a3, 1
.r0_set_prev:
    move    k1, t7
.r0_extend:
    addiu   a0, a0, 1
    sltu    t0, a0, a2
    beqz    t0, .r0_outer
    nop
    subu    t5, a0, t1
    addu    t5, v0, t5
    lbu     t5, 0(t5)
    beq     t5, t6, .r0_extend
    nop
    j       .r0_outer
    nop

############################################################
# .c12_piece_r1 — road 1 only over [lo, hi).
############################################################
.c12_piece_r1:
    move    a0, a1
.r1_outer:
    sltu    t0, a0, a2
    beqz    t0, .c12_piece_next
    nop
    subu    t5, a0, t3                       # offset = x - s1s
    addu    t5, v1, t5
    lbu     t6, 0(t5)                        # b
    addiu   t5, s8, 8                        # &slot8r1
    addu    t5, t5, t6
    lbu     t7, 0(t5)                        # slot
    sll     t7, t7, 1
    addu    t5, s6, t7
    lhu     t7, 0(t5)                        # color
    beq     t7, k1, .r1_extend
    nop
    blez    a3, .r1_open
    nop
    addiu   t5, a3, -1
    sll     t5, t5, 2
    addu    t5, s5, t5
    sh      a0, 0(t5)
.r1_open:
    li      t5, MAX_RUNS_PER_ROW
    sltu    t5, a3, t5
    beqz    t5, .r1_set_prev
    nop
    sll     t5, a3, 2
    addu    t5, s5, t5
    sh      t7, 2(t5)
    addiu   a3, a3, 1
.r1_set_prev:
    move    k1, t7
.r1_extend:
    addiu   a0, a0, 1
    sltu    t0, a0, a2
    beqz    t0, .r1_outer
    nop
    subu    t5, a0, t3
    addu    t5, v1, t5
    lbu     t5, 0(t5)
    beq     t5, t6, .r1_extend
    nop
    j       .r1_outer
    nop

############################################################
# .c12_piece_pair — both roads in [lo, hi). merged_idx lookup.
#   Holds b0 in t6, b1 in t9 across the inner extend loop.
############################################################
.c12_piece_pair:
    move    a0, a1
.pair_outer:
    sltu    t0, a0, a2
    beqz    t0, .c12_piece_next
    nop
    subu    t5, a0, t1
    addu    t5, v0, t5
    lbu     t6, 0(t5)                        # b0
    subu    t5, a0, t3
    addu    t5, v1, t5
    lbu     t9, 0(t5)                        # b1
    sll     t5, t6, 3
    or      t5, t5, t9                        # (b0 << 3) | b1
    addu    t5, s7, t5
    lbu     t5, 0(t5)                        # merged slot
    sll     t5, t5, 1
    addu    t5, s6, t5
    lhu     t7, 0(t5)                        # color
    beq     t7, k1, .pair_extend
    nop
    blez    a3, .pair_open
    nop
    addiu   t5, a3, -1
    sll     t5, t5, 2
    addu    t5, s5, t5
    sh      a0, 0(t5)
.pair_open:
    li      t5, MAX_RUNS_PER_ROW
    sltu    t5, a3, t5
    beqz    t5, .pair_set_prev
    nop
    sll     t5, a3, 2
    addu    t5, s5, t5
    sh      t7, 2(t5)
    addiu   a3, a3, 1
.pair_set_prev:
    move    k1, t7
.pair_extend:
    addiu   a0, a0, 1
    sltu    t0, a0, a2
    beqz    t0, .pair_outer
    nop
    subu    t5, a0, t1
    addu    t5, v0, t5
    lbu     t5, 0(t5)
    bne     t5, t6, .pair_outer              # b0 changed
    nop
    subu    t5, a0, t3
    addu    t5, v1, t5
    lbu     t5, 0(t5)
    beq     t5, t9, .pair_extend             # b1 still matches
    nop
    j       .pair_outer                       # b1 changed
    nop

############################################################
# .c12_piece_oob — entire piece is out-of-bounds for both roads.
#   Emit one c_oob_color run (no scan; coalesces with prior c_oob).
############################################################
.c12_piece_oob:
    lhu     t7, %lo(DESC) + DESC_C_OOB
    beq     t7, k1, .c12_piece_next          # already in a c_oob run
    nop
    blez    a3, .oob_open
    nop
    addiu   t5, a3, -1
    sll     t5, t5, 2
    addu    t5, s5, t5
    sh      a1, 0(t5)                        # runs[n-1].x_end = lo
.oob_open:
    li      t5, MAX_RUNS_PER_ROW
    sltu    t5, a3, t5
    beqz    t5, .oob_set_prev
    nop
    sll     t5, a3, 2
    addu    t5, s5, t5
    sh      t7, 2(t5)
    addiu   a3, a3, 1
.oob_set_prev:
    move    k1, t7
    # Fall through to .c12_piece_next.

.c12_piece_next:
    addiu   t8, t8, 1
    j       .c12_piece_loop
    nop

.c12_pieces_done:
    # Close trailing run at span_end. If no runs were emitted (a3 == 0),
    # skip — emit_done_with_n_in_t3 will see n_runs = 0 and drop the DMA.
    blez    a3, .c12_done
    nop
    lhu     t0, %lo(DESC) + DESC_SE
    addiu   t5, a3, -1
    sll     t5, t5, 2
    addu    t5, s5, t5
    sh      t0, 0(t5)                        # runs[n-1].x_end = span_end
.c12_done:
    move    t3, a3                           # n_runs
    j       emit_done_with_n_in_t3
    nop

############################################################
# vec_scan — scalar find-next-mismatch scan.
#
# In:
#   v0 = src_base (DMEM address; first byte = logical position 0)
#   v1 = slot_table base in DMEM (FRAME_STATE + FS_SLOT8R0 or _R1)
#   a0 = span_start_x  (logical s_start; runs[].x_end is offset by this)
#   a1 = len           (span pixel count; src[len] is sentinel 0xFF)
#
# Out:
#   t3 = n_runs (emitted to RUNS in DMEM)
#
# Then j emit_done_with_n_in_t3.
#
# Reg roster (clobbers t0..t9; ra preserved by caller):
#   v0 = src_base, v1 = slot_table
#   a0 = span_start_x, a1 = len
#   a2 = i (logical scan position)
#   a3 = n (run count)
#   t1 = current b (preserved across the inner loop)
#   k1 = prev_color (16-bit; init to a sentinel > 16-bit so first iter emits)
#   s5 = runs base in DMEM
#   s6 = TLUT_DMEM base
############################################################

vec_scan:
    li      a2, 0                         # i = 0
    li      a3, 0                         # n = 0
    li      k1, 0x10000                   # prev_color = sentinel (>16-bit)
    li      s5, %lo(RUNS)
    li      s6, %lo(TLUT_DMEM)

.vs_outer:
    # If i >= len, scan is done (sentinel pushes us here naturally).
    sltu    t0, a2, a1
    beqz    t0, .vs_done
    nop

    # b = src[i]  (scalar)
    addu    t0, v0, a2
    lbu     t1, 0(t0)                     # t1 = b

    # slot = slot_table[b]
    addu    t0, v1, t1
    lbu     t2, 0(t0)                     # t2 = slot

    # color = tlut[slot]
    sll     t2, t2, 1                     # slot * 2
    addu    t0, s6, t2
    lhu     t3, 0(t0)                     # t3 = color (RGBA5551)

    # Skip the emit when colour didn't change.
    beq     t3, k1, .vs_extend
    nop

    # Close the previous run (if any): runs[n-1].x_end = a0 + a2.
    blez    a3, .vs_open_new
    nop
    addiu   t4, a3, -1
    sll     t4, t4, 2                     # (n-1) * 4
    addu    t4, s5, t4
    addu    t5, a0, a2
    sh      t5, 0(t4)                     # runs[n-1].x_end

.vs_open_new:
    # Open a new run if there's room (cap at MAX_RUNS_PER_ROW). When full
    # we drop the open but still record the colour transition in k1 so the
    # trailing close at .vs_done lands on the right index.
    li      t6, MAX_RUNS_PER_ROW
    sltu    t6, a3, t6
    beqz    t6, .vs_update_prev
    nop
    sll     t4, a3, 2
    addu    t4, s5, t4
    sh      t3, 2(t4)                     # runs[n].color5551 (offset 2)
    addiu   a3, a3, 1
.vs_update_prev:
    move    k1, t3

.vs_extend:
    # Scan forward over the run of equal bytes. The sentinel byte
    # src[len]=0xFF (stamped by do_case0 / do_case3) is guaranteed to
    # break the loop, so no per-iter bounds check is needed.
    addiu   a2, a2, 1
    addu    t0, v0, a2
    lbu     t4, 0(t0)
    beq     t4, t1, .vs_extend
    nop
    j       .vs_outer
    nop

.vs_done:
    # Close the trailing run: runs[n-1].x_end = a0 + len = span_end.
    blez    a3, .vs_done_zero
    nop
    addiu   t4, a3, -1
    sll     t4, t4, 2
    addu    t4, s5, t4
    addu    t5, a0, a1
    sh      t5, 0(t4)
.vs_done_zero:
    move    t3, a3                        # n_runs
    j       emit_done_with_n_in_t3
    nop

############################################################
# emit_done_with_n_in_t3 — common tail: stash n_runs[y], DMA runs out.
#
# In:
#   t3 = n_runs for this row (0..MAX_RUNS_PER_ROW)
#
# Falls through to advance.
############################################################
emit_done_with_n_in_t3:
    # N_RUNS_OUT[y] = t3
    sll     t4, k0, 1
    li      t5, %lo(N_RUNS_OUT)
    addu    t4, t4, t5
    sh      t3, 0(t4)

    # If n_runs == 0, skip the DMA out (nothing to send).
    blez    t3, advance
    nop

    # byte_count = n_runs * 4, rounded up to 8.
    sll     t4, t3, 2                    # n * 4
    addiu   t4, t4, 7
    li      t5, ~7
    and     t4, t4, t5                   # align up to 8

    lw      s0, %lo(DESC) + DESC_RUNS_PHYS
    li      s4, %lo(RUNS)
    addiu   t0, t4, -1                   # DMA size encoding (len-1)
    jal     DMAOut
    nop

advance:
    addiu   s1, s1, DESC_SIZE
    addiu   k0, k0, 1
    j       scanline_loop
    addiu   s3, s3, -1

frame_done:
    # Bulk DMA the n_runs table back to RDRAM.
    # k0 holds total rows processed; rounding up to multiple of 8 for SP DMA.
    sll     t0, k0, 1                    # k0 * 2 bytes
    addiu   t0, t0, 7
    li      t1, ~7
    and     t0, t0, t1                   # align up

    lw      s0, %lo(FRAME_STATE) + FS_N_RUNS_PHYS
    li      s4, %lo(N_RUNS_OUT)
    addiu   t0, t0, -1                   # DMA size encoding
    jal     DMAOut
    nop

    j       RSPQ_Loop
    nop

############################################################
# hp_RDPQ_Write8 — append 8 bytes (a0, a1) to COOB_STAGING.
#
# Lightweight clone of upstream RDPQ_Write8 sized for this overlay
# (the upstream lives in rsp_rdpq.inc; including the whole .inc would
# pull in triangle / mode-API / scissor code we don't use and overflow
# IMEM). Touches only s0; ra is preserved.
############################################################
hp_RDPQ_Write8:
    lw      s0, %lo(COOB_STAGING_PTR)
    sw      a0, 0x00(s0)
    sw      a1, 0x04(s0)
    addi    s0, s0, 8
    jr      ra
    sw      s0, %lo(COOB_STAGING_PTR)

############################################################
# hp_RDPQ_Send — flush COOB_STAGING bytes [s4, s3) to the RDP buffer.
#
# Mirrors upstream RDPQ_Send (rsp_rdpq.inc): DMAs the staged bytes to
# the live RDP buffer, advancing DP_END via RSPQCmd_RdpAppendBuffer.
# When the buffer wraps, swaps RDPQ_DYNAMIC_BUFFERS and re-points DP
# via RSPQCmd_RdpSetBuffer.
#
# In:
#   s4 = DMEM start (= %lo(COOB_STAGING))
#   s3 = DMEM end (= COOB_STAGING_PTR snapshot)
# Out: returns to ra (after the tail-called append/set), with
#      COOB_STAGING_PTR reset by the caller.
############################################################
hp_RDPQ_Send:
    sub     rspq_cmd_size, s3, s4
    beqz    rspq_cmd_size, JrRa          # nothing staged — exit cleanly
    move    ra2, ra

    lw      s0, %lo(RDPQ_CURRENT)
    and     s0, s0, 0xFFFFFF             # mask to RDRAM
    lw      a2, %lo(RDPQ_SENTINEL)
    sub     a2, a2, rspq_cmd_size
    bge     a2, s0, hp_send_do_dma
    li      t5, RSPQCmd_RdpAppendBuffer  # next_func = append

    # Not enough room — switch dynamic buffers. Wait for END_VALID to
    # clear (and any SYNC_FULL to drain) before reusing the other slot.
    lbu     t3, %lo(RDPQ_SYNCFULL_ONGOING)
    ori     t3, t3, DP_STATUS_END_VALID
hp_send_wait:
    mfc0    t0, COP0_DP_STATUS
    and     t0, t0, t3
    bnez    t0, hp_send_wait
    nop

    lw      s0, %lo(RDPQ_DYNAMIC_BUFFERS) + 4
    lw      t1, %lo(RDPQ_DYNAMIC_BUFFERS) + 0
    sw      s0, %lo(RDPQ_DYNAMIC_BUFFERS) + 0
    sw      t1, %lo(RDPQ_DYNAMIC_BUFFERS) + 4
    add     a2, s0, RDPQ_DYNAMIC_BUFFER_SIZE
    move    a1, s0                       # a1 = new DP_START
    li      t5, RSPQCmd_RdpSetBuffer     # next_func = setbuf

hp_send_do_dma:
    add     t0, rspq_cmd_size, -1
    jal     DMAOut
    add     a0, s0, rspq_cmd_size        # a0 = new DP_END

    jr      t5                            # tail to next_func
    move    ra, ra2

############################################################
# HWRoadRDP_EmitCoobFill: emit the per-row OOB-colour strip directly
# to the RDP via fill_rectangles.
#
# Args:
#   a0 = (cmd<<24) | coob_phys (RDRAM ptr to u16 COOB array)
#   a1 = (x_off<<16) | y_off
#   a2 = (W<<16) | n_rows
#
# The caller (emit_foreground_lores_rdp on CPU) must have issued
# rdpq_set_mode_fill() before queueing this command so the RDP is in
# fill mode when our staged commands flush through.
#
# Per row y in [0..n_rows]:
#   - load c = COOB[y]; 0xFFFF means "inactive"
#   - track an "open" rect (run_start, run_color):
#       * if active and matches run_color: extend (no emit)
#       * if active and different colour, or inactive: close run, emit
#         SET_FILL_COLOR + FILL_RECTANGLE for [x_off, y_off+run_start ..
#         x_off+W, y_off+y], then open a new run if active.
#
# Walking the trailing 0xFFFF sentinel at y=n_rows closes the final
# open run without special-casing.
#
# Reg roster:
#   s0  = coob phys (saved before DMA), then scratch
#   s1  = x_off (sign-extended)
#   s2  = y_off (sign-extended)
#   s5  = n_rows (loop bound)
#   s6  = W   — kept out of s3 because hp_RDPQ_Send consumes s3 as its
#              DMEM-end argument; s6 survives both DMAOut and the flush.
#   k0  = current y
#   t6  = run_start (or -1)
#   t7  = run_color (only valid when t6 >= 0). Trashed by hp_RDPQ_Send
#         (it aliases rspq_cmd_size), but always re-set in the open path
#         or left dead in the close-only path before next read.
#   s7  = current row c — held in a callee-saved reg so it survives the
#         mid-loop flush (DMAOut inside hp_RDPQ_Send destroys t-regs).
############################################################
HWRoadRDP_EmitCoobFill:
    # ---- DMA the coob array in. -----------------------------
    # COOB array is MAX_LINES+1 u16 = 464 bytes (CPU rounds and stamps a
    # 0xFFFF sentinel at MAX_LINES so the loop's trailing close lands).
    # Strip the rspq cmd byte from a0 via shift (andi's immediate is 16-bit
    # on MIPS, so 0xFFFFFF can't be encoded directly).
    sll     s0, a0, 8
    srl     s0, s0, 8                    # phys = a0 & 0x00FFFFFF
    li      s4, %lo(COOB_FILL_DMEM)
    jal     DMAIn
    li      t0, DMA_SIZE(464, 1)

    # ---- Stash inputs in callee-saved regs. -----------------
    # x_off / y_off are sign-extended in case the caller hands us a
    # negative offset (none today, but keep the encoding honest).
    sra     s1, a1, 16                   # x_off
    sll     s2, a1, 16
    sra     s2, s2, 16                   # y_off
    sra     s6, a2, 16                   # W (kept in s6 so flush below
                                         #     can use s3 as hp_RDPQ_Send arg)
    andi    s5, a2, 0xFFFF               # n_rows

    # ---- Reset staging pointer. ------------------------------
    li      t0, %lo(COOB_STAGING)
    sw      t0, %lo(COOB_STAGING_PTR)

    # ---- Loop init. -----------------------------------------
    li      t6, -1                       # run_start = -1
    li      t7, 0                        # run_color (unused while -1)
    li      k0, 0                        # y

    addiu   s5, s5, 1                    # walk inclusive of sentinel row

coob_loop:
    sltu    t0, k0, s5
    beqz    t0, coob_done
    nop

    # c = COOB[y]. Land in callee-saved s7 so it survives any mid-loop
    # flush via DMAOut (which destroys t-regs).
    sll     t0, k0, 1
    addiu   t1, t0, %lo(COOB_FILL_DMEM)
    lhu     s7, 0(t1)

    bltz    t6, coob_no_open
    nop

    # Have an open run. If colour matches, extend (skip emit).
    beq     s7, t7, coob_advance
    nop

    # ---- Close current run: emit SYNC_PIPE + SET_FILL_COLOR + FILL_RECTANGLE.
    # SYNC_PIPE (0xE7000000, 0): real RDP needs the previous fill rect's
    # pixel writes drained before a fill-color change, or the new colour
    # races stripe-pixels of the still-in-flight rectangle and we get
    # wrong-colored bands. libdragon's rdpq_set_fill_color auto-syncs
    # via autosync, but this overlay bypasses rdpq.
    lui     a0, 0xE700
    move    a1, zero
    jal     hp_RDPQ_Write8
    nop

    # SET_FILL_COLOR: w0 = 0xF7000000, w1 = (c<<16) | c (16bpp pack)
    lui     a0, 0xF700
    sll     a1, t7, 16
    or      a1, a1, t7
    jal     hp_RDPQ_Write8
    nop

    # FILL_RECTANGLE: coords are 10.2 sub-pixel — multiply pixel inputs
    # by 4 before packing into the 12-bit fields. In fill mode (cycle_type
    # == 2) the encoded w0 also gets (4<<12)|4 subtracted so that an
    # exclusive bottom-right (x1, y1) sample-aligns to the last covered
    # pixel: see __rdpq_fill_rectangle in libdragon/src/rdpq/rdpq_rect.c.
    #   w0 = 0xF6000000 | ((x1*4 & 0xFFF) << 12) | (y1*4 & 0xFFF) - 0x4004
    #   w1 = ((x0*4 & 0xFFF) << 12) | (y0*4 & 0xFFF)
    # Coords (pixel space):
    #   x0 = x_off               x1 = x_off + W
    #   y0 = y_off + run_start   y1 = y_off + y
    add     t8, s2, t6                   # y0 = y_off + run_start  (px)
    add     t9, s2, k0                   # y1 = y_off + y          (px)
    add     v0, s1, s6                   # x1 = x_off + W          (px)

    sll     v0, v0, 2                    # x1 subpixel
    sll     t9, t9, 2                    # y1 subpixel
    andi    t0, v0, 0xFFF
    sll     t0, t0, 12
    andi    t1, t9, 0xFFF
    or      t0, t0, t1
    lui     t1, 0xF600
    or      t0, t0, t1
    addiu   a0, t0, -0x4004              # fill-mode coord adjust

    sll     t2, s1, 2                    # x0 subpixel (t2 = scratch; c is in s7)
    sll     t8, t8, 2                    # y0 subpixel
    andi    t0, t2, 0xFFF
    sll     t0, t0, 12
    andi    t1, t8, 0xFFF
    or      a1, t0, t1
    jal     hp_RDPQ_Write8
    nop

    # ---- Mid-command flush if staging is getting full. -----
    # Each emitted rect uses 16 bytes; threshold (0xA0 = 160) leaves
    # >= 16 bytes headroom so we never overflow between checks. c is in
    # s7 (callee-saved), so DMAOut's t-clobbers below don't touch it.
    lw      t4, %lo(COOB_STAGING_PTR)
    li      t5, %lo(COOB_STAGING) + 0xA0
    sltu    t0, t4, t5                   # 1 if ptr < threshold (skip flush)
    bnez    t0, coob_after_flush
    nop
    li      s4, %lo(COOB_STAGING)
    move    s3, t4                        # s3 = staging end (input to hp_RDPQ_Send)
    sw      s4, %lo(COOB_STAGING_PTR)     # reset write head to base
    jal     hp_RDPQ_Send
    nop
coob_after_flush:

    # ---- Decide what to do with the new row. ---------------
    # If s7 (c) == 0xFFFF (inactive), leave closed; otherwise open a new
    # run at y with c.
    li      t3, 0xFFFF
    beq     s7, t3, coob_close_only
    nop
    move    t6, k0                       # run_start = y
    move    t7, s7                       # run_color = c
    j       coob_advance
    nop
coob_close_only:
    li      t6, -1
    j       coob_advance
    nop

coob_no_open:
    # No open run. If c is active, open. If inactive, fall through.
    li      t3, 0xFFFF
    beq     s7, t3, coob_advance
    nop
    move    t6, k0
    move    t7, s7

coob_advance:
    addiu   k0, k0, 1
    j       coob_loop
    nop

coob_done:
    # Final flush + return to dispatcher.
    li      s4, %lo(COOB_STAGING)
    lw      s3, %lo(COOB_STAGING_PTR)
    li      t0, %lo(COOB_STAGING)
    sw      t0, %lo(COOB_STAGING_PTR)
    jal_and_j hp_RDPQ_Send, RSPQ_Loop

############################################################
# HWRoadRDP_EmitRuns: emit per-row body fill_rectangles for the run
# lists produced by HWRoadRDP_BuildRuns.
#
# Must be queued AFTER HWRoadRDP_EmitCoobFill so the c_oob backstop lands
# first in the RDP command stream; the runs overpaint the body within
# that backstop.
#
# Args:
#   a0 = (cmd<<24) | emit_row_phys   RDRAM ptr to EmitRowRDP[MAX_LINES]
#                                    (4 B/row: u16 s_start, u16 c_oob)
#   a1 = n_runs_phys                 RDRAM ptr to u16[MAX_LINES]
#                                    (written async by BuildRuns DMAOut)
#   a2 = runs_base_phys              RDRAM base of runs_buf
#                                    (per-row stride = MAX_RUNS_PER_ROW*4 = 256)
#   a3 = (x_off<<16) | y_off         packed framebuffer offsets (signed 16-bit)
#
# Per row y in [0..MAX_LINES):
#   n = n_runs[y]; if n == 0, advance.
#   DMA runs (rounded to 8 bytes) into RUN_DMEM.
#   prev_x = emit_row[y].s_start; c_oob = emit_row[y].c_oob.
#   For each i in 0..n:
#     xe    = runs[i].x_end
#     color = runs[i].color5551
#     If color != c_oob && color != 0 && xe > prev_x:
#       SET_FILL_COLOR (skipped if matches the last emitted colour)
#       FILL_RECTANGLE [x_off+prev_x, y_off+y .. x_off+xe, y_off+y+1]
#     prev_x = xe.
#
# Reg roster (across rows + flushes):
#   s1 = runs_base_phys (RDRAM)
#   s2 = x_off (sign-extended)
#   s5 = EMIT_ROW_DMEM base (constant)
#   s6 = last emitted RGBA5551 colour (for SET_FILL_COLOR dedup)
#   s7 = last_color_valid (0 / 1)
#   s8 = current row c_oob (callee-saved; survives Send + DMAOut)
#   k0 = current y
#   k1 = y_off (sign-extended)
#
# Per-row scratch (must be reloaded after DMA / Send):
#   t2 = n_runs (DMAOut destroys t2; reload after Send)
#   v1 = prev_x (callee-saved-style; updated unconditionally per run)
#
# Per-run scratch (clobbered each iteration):
#   v0 = i; t4 = xe; t5 = color; t0/t1/t3/t6/t7/t8 = generic
############################################################
HWRoadRDP_EmitRuns:
    # ---- DMA EmitRow array in (896 B). ----------------------
    sll     s0, a0, 8
    srl     s0, s0, 8                    # phys = a0 & 0x00FFFFFF
    li      s4, %lo(EMIT_ROW_DMEM)
    jal     DMAIn
    li      t0, DMA_SIZE(896, 1)

    # ---- DMA n_runs[] in (448 B). ---------------------------
    move    s0, a1
    li      s4, %lo(N_RUNS_DMEM)
    jal     DMAIn
    li      t0, DMA_SIZE(448, 1)

    # ---- Stash inputs in callee-saved regs. -----------------
    move    s1, a2                       # runs_base_phys
    sra     s2, a3, 16                   # x_off (arithmetic shift = sign-extend)
    sll     k1, a3, 16
    sra     k1, k1, 16                   # y_off
    li      s5, %lo(EMIT_ROW_DMEM)
    li      s6, 0                        # last_color (unused while !valid)
    li      s7, 0                        # last_color_valid = 0

    # ---- Reset staging pointer. -----------------------------
    li      t0, %lo(COOB_STAGING)
    sw      t0, %lo(COOB_STAGING_PTR)

    li      k0, 0                        # y = 0

er_row_loop:
    li      t0, MAX_LINES
    sltu    t0, k0, t0
    beqz    t0, er_done
    nop

    # ---- Load n_runs[y]; skip row if zero. -----------------
    sll     t0, k0, 1
    addiu   t1, t0, %lo(N_RUNS_DMEM)
    lhu     t2, 0(t1)
    beqz    t2, er_row_advance
    nop

    # ---- Load EmitRow[y]: s_start (init prev_x) + c_oob. ----
    sll     t0, k0, 2
    addiu   t1, t0, %lo(EMIT_ROW_DMEM)
    lhu     v1, 0(t1)                    # prev_x = s_start
    lhu     s8, 2(t1)                    # c_oob

    # ---- DMA this row's runs into RUN_DMEM. -----------------
    # runs_phys = runs_base + y * 256 (= MAX_RUNS_PER_ROW * sizeof(Run))
    sll     s0, k0, 8
    addu    s0, s0, s1
    sll     t0, t2, 2                    # n_runs * 4
    addiu   t0, t0, 7
    li      t1, ~7
    and     t0, t0, t1                   # rounded up to 8
    addiu   t0, t0, -1                   # DMA size encoding (len-1)
    li      s4, %lo(RUN_DMEM)
    jal     DMAIn
    nop

    # ---- Reload n_runs (DMAIn clobbered t2). ---------------
    sll     t0, k0, 1
    addiu   t1, t0, %lo(N_RUNS_DMEM)
    lhu     t2, 0(t1)

    li      v0, 0                        # i = 0

er_run_loop:
    sltu    t0, v0, t2
    beqz    t0, er_row_advance
    nop

    sll     t0, v0, 2
    addiu   t3, t0, %lo(RUN_DMEM)
    lhu     t4, 0(t3)                    # x_end
    lhu     t5, 2(t3)                    # color

    # Skip cases that paint nothing: colour matches Phase 2a's coob fill,
    # vestigial transparent CI4 slot (color == 0), or zero-width.
    beq     t5, s8, er_run_advance
    nop
    beqz    t5, er_run_advance
    nop
    sltu    t0, v1, t4
    beqz    t0, er_run_advance
    nop

    # ---- SYNC_PIPE + SET_FILL_COLOR if colour changed. -----------------
    # SYNC_PIPE before SET_FILL_COLOR is mandatory on real hardware when
    # a fill rect is still pixel-writing — without it the new colour
    # races stripes of the previous rect. ares hides the hazard; HW
    # shows it as wrong-colored road stripes. See the coob phase above.
    beqz    s7, .er_emit_set
    nop
    beq     t5, s6, .er_skip_set
    nop
.er_emit_set:
    lui     a0, 0xE700
    move    a1, zero
    jal     hp_RDPQ_Write8
    nop

    lui     a0, 0xF700
    sll     a1, t5, 16
    or      a1, a1, t5
    jal     hp_RDPQ_Write8
    nop
    # Write8 touches only s0; t5 still holds colour for the tracker update.
    move    s6, t5
    li      s7, 1
.er_skip_set:

    # ---- FILL_RECTANGLE [x0,y0]..[x1,y1] (pixel coords). ----
    #   x0 = x_off + prev_x   x1 = x_off + xe
    #   y0 = y_off + y        y1 = y_off + y + 1
    # 10.2 sub-pixel encoding: multiply pixel inputs by 4 before packing
    # into the 12-bit fields. In fill mode (cycle_type == 2) the encoded
    # w0 also gets (4<<12)|4 subtracted so the exclusive bottom-right
    # sample-aligns to the last covered pixel — see __rdpq_fill_rectangle
    # in libdragon/src/rdpq/rdpq_rect.c.
    add     t6, s2, v1                   # x0 (px)
    add     t7, s2, t4                   # x1 (px)
    add     t8, k1, k0                   # y0 (px) = y_off + y
    addiu   t3, t8, 1                    # y1 (px) = y0 + 1

    sll     t7, t7, 2                    # x1 sub-pixel
    sll     t3, t3, 2                    # y1 sub-pixel
    sll     t6, t6, 2                    # x0 sub-pixel
    sll     t8, t8, 2                    # y0 sub-pixel

    andi    t1, t7, 0xFFF
    sll     t1, t1, 12
    andi    t0, t3, 0xFFF
    or      t1, t1, t0
    lui     t0, 0xF600
    or      t1, t1, t0
    addiu   a0, t1, -0x4004              # w0 (fill-mode coord adjust)

    andi    t1, t6, 0xFFF
    sll     t1, t1, 12
    andi    t0, t8, 0xFFF
    or      a1, t1, t0                   # w1

    jal     hp_RDPQ_Write8
    nop

    # ---- Mid-loop flush check. ------------------------------
    # Each visible run stages 16 bytes; threshold 0xA0 leaves ≥ 16 B
    # headroom so we never overflow between checks.
    lw      t0, %lo(COOB_STAGING_PTR)
    li      t1, %lo(COOB_STAGING) + 0xA0
    sltu    t1, t0, t1
    bnez    t1, .er_no_flush
    nop
    li      s4, %lo(COOB_STAGING)
    move    s3, t0
    sw      s4, %lo(COOB_STAGING_PTR)
    jal     hp_RDPQ_Send
    nop
    # Send → DMAOut destroys t2 (per rsp_dma.inc); reload before next iter.
    sll     t0, k0, 1
    addiu   t1, t0, %lo(N_RUNS_DMEM)
    lhu     t2, 0(t1)
.er_no_flush:

er_run_advance:
    move    v1, t4                       # prev_x = xe (unconditional)
    addiu   v0, v0, 1
    j       er_run_loop
    nop

er_row_advance:
    addiu   k0, k0, 1
    j       er_row_loop
    nop

er_done:
    # Final flush + return to dispatcher.
    li      s4, %lo(COOB_STAGING)
    lw      s3, %lo(COOB_STAGING_PTR)
    li      t0, %lo(COOB_STAGING)
    sw      t0, %lo(COOB_STAGING_PTR)
    jal_and_j hp_RDPQ_Send, RSPQ_Loop
