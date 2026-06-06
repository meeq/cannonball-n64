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

.set noreorder
.set at

####################################################################
# Data segment — overlay header only.
####################################################################
.data

RSPQ_BeginOverlayHeader
    # 12 bytes = 3 words: (cmd|arg0) + arg1 + arg2 (descriptor base + state base).
    RSPQ_DefineCommand HWRoadRDP_BuildRuns, 12
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
