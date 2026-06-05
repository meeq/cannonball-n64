####################################################################
# rsp_hwroad_rdp.S — RSP overlay that packs the hwroad foreground CI4
# mask used by the RDP path (see src/main/n64/hwroad_rdp.cpp).
#
# Mirrors the CPU build's segmented pack: for each scanline that is
# kind=DRAW, decide which road's slot LUT applies per pixel and pack
# two 4-bit indices into one byte of the per-row CI4 mask.
#
# Companion to rsp_hwroad.S, which paints the full RGBA5551 scratch.
# That overlay turned out 45% slower than the CPU rasteriser because
# every line did a synchronous DMA round-trip. This overlay handles a
# much smaller workload (160 B mask out vs 640 B pixels) and lets the
# CPU continue with other prepare_frame work while the RSP runs.
#
# Command 0 — HWRoadRDP_BuildMasks:
#   a0 = num_scanlines (low 16 bits)         — cmd word top byte stripped
#   a1 = per-row descriptor base in RDRAM    — see DescriptorRDP struct
#   a2 = frame state base in RDRAM           — slot LUTs + merged_idx + ctrl
#
# Per-row descriptor layout (32 bytes, see DescriptorRDP in hwroad_rdp_rsp.cpp):
#   0x00  u8   kind          0=SKIP, 1=OOB_ONLY, 2=DRAW
#   0x01  u8   ctrl_case     0..3
#   0x02  u16  span_start
#   0x04  u16  span_end
#   0x06  u16  s0s
#   0x08  u16  s0e
#   0x0a  u16  s1s
#   0x0c  u16  s1e
#   0x10  u32  src0_phys     RDRAM phys of road0 src row + t0_offset
#   0x14  u32  src1_phys     RDRAM phys of road1 src row + t1_offset
#   0x18  u32  mask_phys     RDRAM phys of mask[y][0]
#   0x1c  u32  _pad
#
# Frame state layout (128 bytes total, padded):
#   0x00  u8   ctrl
#   0x01  u8   oob_pair      (oob_slot << 4) | oob_slot
#   0x02  u8[6] _pad
#   0x08  u8[8]  slot8r0     merged_idx[(p<<3)|3] for p in 0..7
#   0x10  u8[8]  slot8r1     merged_idx[(3<<3)|p] for p in 0..7
#   0x18  u8[64] merged_idx  full table, keyed by (p0<<3)|p1
#   0x58  u8[40] _pad
#
# Per-row protocol (sync DMA, scalar pack):
#   1. DMA descriptor (32 B) → DESC.
#   2. If kind != DRAW, advance.
#   3. Pre-fill MASK in DMEM with oob_pair (covers ctrl=1/2 gap regions
#      and the odd-pixel tail of any span).
#   4. DMA src0/src1 (336 B each, covers max span 320 + alignment slop).
#      Skip the unused src on single-road controls.
#   5. Pack pairs into MASK via the per-ctrl inner loop:
#        ctrl=0  -> slot8r0[src0[i - s0s]]                       (single road 0)
#        ctrl=3  -> slot8r1[src1[i - s1s]]                       (single road 1)
#        ctrl=1/2-> walk full span; bounds-checked src reads → 3
#                   on OOB; slot via merged_idx[(p0<<3)|p1]      (both roads)
#   6. DMA byte_count bytes of MASK out to mask_phys.
####################################################################

#include <rsp_queue.inc>

.set noreorder
.set at

####################################################################
# Data segment — overlay header.
####################################################################
.data

RSPQ_BeginOverlayHeader
    # 12 bytes = 3 words: (cmd|arg0) + arg1 + arg2 (descriptor base + state base).
    RSPQ_DefineCommand HWRoadRDP_BuildMasks, 12
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
FRAME_STATE: .space 128         # ctrl + slot8r0 + slot8r1 + merged_idx
.align 4
# SP DMA ignores the low 3 bits of the RDRAM source addr. We DMA up to
# 336 bytes (= 320 max span + alignment slop, rounded to multiple of 8).
# SRC0/SRC1 are sized to 528 to give worst-case slack and keep .bss
# aligned the same way as rsp_hwroad.S.
SRC0:        .space 528
.align 4
SRC1:        .space 528
.align 4
SRC0_BASE:   .space 4           # %lo(SRC0) + (src0_phys & 7); stashed after DMA
.align 4
SRC1_BASE:   .space 4           # %lo(SRC1) + (src1_phys & 7)
.align 4
MASK:        .space 192         # CI4 mask row (160 used; pad to 192 for align)

####################################################################
# Code segment.
####################################################################
.text

# Descriptor field offsets.
#define DESC_KIND       0x00
#define DESC_CTRL       0x01
#define DESC_SS         0x02
#define DESC_SE         0x04
#define DESC_S0S        0x06
#define DESC_S0E        0x08
#define DESC_S1S        0x0a
#define DESC_S1E        0x0c
#define DESC_SRC0_PHYS  0x10
#define DESC_SRC1_PHYS  0x14
#define DESC_MASK_PHYS  0x18
#define DESC_SIZE       32

# Frame state field offsets.
#define FS_CTRL         0x00
#define FS_OOB          0x01
#define FS_SLOT8R0      0x08
#define FS_SLOT8R1      0x10
#define FS_MERGED       0x18

# DMA sizing. Constant 336 B covers (max span 320) + (max misalign 7)
# rounded up to multiple of 8. Mask out is variable: byte_count - 1
# at run time.
#define SRC_DMA_BYTES   336

############################################################
# HWRoadRDP_BuildMasks: pack the per-row CI4 mask for all DRAW lines.
############################################################
    .func HWRoadRDP_BuildMasks
HWRoadRDP_BuildMasks:
    # Stash inputs in callee-saved regs. DMA macros clobber t0..t3, s0, s4.
    # a0's top byte carries the rspq cmd word; mask it.
    andi    s3, a0, 0xFFFF              # rows remaining
    move    s1, a1                       # desc base RDRAM ptr
    move    s2, a2                       # state base RDRAM ptr

    # --- DMA frame state once for the whole queue invocation. -------------
    move    s0, s2
    li      s4, %lo(FRAME_STATE)
    jal     DMAIn
    li      t0, DMA_SIZE(128, 1)

scanline_loop:
    blez    s3, frame_done
    nop

    # --- DMA the per-row descriptor. --------------------------------------
    move    s0, s1
    li      s4, %lo(DESC)
    jal     DMAIn
    li      t0, DMA_SIZE(DESC_SIZE, 1)

    # --- kind != DRAW (2)? advance with no further work. -----------------
    lbu     t0, %lo(DESC) + DESC_KIND
    li      t1, 2
    bne     t0, t1, advance
    nop

    # --- Pre-fill MASK with the frame's oob_pair word. -------------------
    # 160 bytes of mask space; pre-fill the full 192 to cover any span.
    # This handles ctrl=1/2 gap regions (between non-overlapping s0/s1
    # segments) and the low nibble of an odd-pixel tail in any ctrl.
    lbu     t0, %lo(FRAME_STATE) + FS_OOB         # oob_pair
    sll     t1, t0, 8
    or      t1, t1, t0                             # 0x0000 PP PP
    sll     t2, t1, 16
    or      t1, t1, t2                             # 0x PP PP PP PP
    li      t2, %lo(MASK)
    li      t3, %lo(MASK) + 192
.fill_loop:
    sw      t1, 0(t2)
    addiu   t2, t2, 4
    bne     t2, t3, .fill_loop
    nop

    # --- Determine which srcs we need. ctrl 0/3 are single-road. ---------
    lbu     s5, %lo(DESC) + DESC_CTRL              # ctrl_case (cached)

    # src0 is used by ctrl 0, 1, 2 (i.e. ctrl != 3).
    li      t0, 3
    beq     s5, t0, .src0_skip
    nop
    lw      s0, %lo(DESC) + DESC_SRC0_PHYS
    li      s4, %lo(SRC0)
    jal     DMAIn
    li      t0, DMA_SIZE(SRC_DMA_BYTES, 1)
    sw      s4, %lo(SRC0_BASE)                     # s4 = aligned-byte ptr
.src0_skip:

    # src1 is used by ctrl 1, 2, 3 (i.e. ctrl != 0).
    beqz    s5, .src1_skip
    nop
    lw      s0, %lo(DESC) + DESC_SRC1_PHYS
    li      s4, %lo(SRC1)
    jal     DMAIn
    li      t0, DMA_SIZE(SRC_DMA_BYTES, 1)
    sw      s4, %lo(SRC1_BASE)
.src1_skip:

    # --- Dispatch to per-ctrl pack. --------------------------------------
    # ctrl 0 -> pack_case0; 1 -> pack_case12 (with merged_idx using 1's row);
    # 2 -> pack_case12 (using 2's row); 3 -> pack_case3.
    # The merged_idx LUT in FRAME_STATE was built by the CPU for this
    # frame's ctrl, so ctrl 1 and 2 share the same inner loop.
    beqz    s5, pack_case0
    nop
    li      t0, 3
    beq     s5, t0, pack_case3
    nop
    # fall through to ctrl 1/2
    j       pack_case12
    nop

pack_done:
    # --- DMA the populated mask back to mask_phys. -----------------------
    # byte_count = (span_end - span_start + 1) >> 1
    lhu     t1, %lo(DESC) + DESC_SS
    lhu     t2, %lo(DESC) + DESC_SE
    subu    t1, t2, t1
    addiu   t1, t1, 1
    srl     t1, t1, 1                              # byte_count
    lw      s0, %lo(DESC) + DESC_MASK_PHYS
    li      s4, %lo(MASK)
    jal     DMAOut
    addiu   t0, t1, -1                             # DMA size encoding

advance:
    addiu   s1, s1, DESC_SIZE
    j       scanline_loop
    addiu   s3, s3, -1

frame_done:
    j       RSPQ_Loop
    nop
    .endfunc

############################################################
# pack_case0 — ctrl=0, single road 0.
#
# Iterates pixel pairs over [s0s, s0e). Reads src0[k], src0[k+1] from
# DMEM, looks up slot8r0[], packs (slot_a << 4) | slot_b into one
# mask byte. Span guaranteed >= 1 (we'd have taken OOB_ONLY otherwise),
# but may be odd → tail pixel writes only the high nibble (low nibble
# stays oob from the pre-fill).
############################################################
    .func pack_case0
pack_case0:
    # span = s0e - s0s. mask dest = MASK (offset 0; span_start == s0s).
    lhu     t0, %lo(DESC) + DESC_S0S
    lhu     t1, %lo(DESC) + DESC_S0E
    subu    t0, t1, t0                              # span (pixels)

    li      t2, %lo(MASK)                            # dst byte ptr
    lw      t3, %lo(SRC0_BASE)                       # src byte ptr (1st valid px)
    li      t4, %lo(FRAME_STATE) + FS_SLOT8R0        # slot8r0[0]

    srl     t5, t0, 1                                # whole pair count
    andi    t6, t0, 1                                # odd tail flag

    blez    t5, .case0_tail
    nop
.case0_pair_loop:
    lbu     t7, 0(t3)                                # p_a = src0[k]
    lbu     t8, 1(t3)                                # p_b = src0[k+1]
    addu    t7, t4, t7
    addu    t8, t4, t8
    lbu     t7, 0(t7)                                # slot_a
    lbu     t8, 0(t8)                                # slot_b
    sll     t7, t7, 4
    or      t9, t7, t8
    sb      t9, 0(t2)
    addiu   t3, t3, 2
    addiu   t2, t2, 1
    addiu   t5, t5, -1
    bgtz    t5, .case0_pair_loop
    nop

.case0_tail:
    beqz    t6, .case0_done
    nop
    lbu     t7, 0(t3)                                # last pixel
    addu    t7, t4, t7
    lbu     t7, 0(t7)
    sll     t7, t7, 4
    lbu     t8, 0(t2)                                # current byte (low nibble = oob)
    andi    t8, t8, 0x0F
    or      t7, t7, t8
    sb      t7, 0(t2)
.case0_done:
    j       pack_done
    nop
    .endfunc

############################################################
# pack_case3 — ctrl=3, single road 1. Same shape as pack_case0 but
# walks SRC1 and looks up slot8r1.
############################################################
    .func pack_case3
pack_case3:
    lhu     t0, %lo(DESC) + DESC_S1S
    lhu     t1, %lo(DESC) + DESC_S1E
    subu    t0, t1, t0                               # span

    li      t2, %lo(MASK)
    lw      t3, %lo(SRC1_BASE)
    li      t4, %lo(FRAME_STATE) + FS_SLOT8R1

    srl     t5, t0, 1
    andi    t6, t0, 1

    blez    t5, .case3_tail
    nop
.case3_pair_loop:
    lbu     t7, 0(t3)
    lbu     t8, 1(t3)
    addu    t7, t4, t7
    addu    t8, t4, t8
    lbu     t7, 0(t7)
    lbu     t8, 0(t8)
    sll     t7, t7, 4
    or      t9, t7, t8
    sb      t9, 0(t2)
    addiu   t3, t3, 2
    addiu   t2, t2, 1
    addiu   t5, t5, -1
    bgtz    t5, .case3_pair_loop
    nop

.case3_tail:
    beqz    t6, .case3_done
    nop
    lbu     t7, 0(t3)
    addu    t7, t4, t7
    lbu     t7, 0(t7)
    sll     t7, t7, 4
    lbu     t8, 0(t2)
    andi    t8, t8, 0x0F
    or      t7, t7, t8
    sb      t7, 0(t2)
.case3_done:
    j       pack_done
    nop
    .endfunc

############################################################
# pack_case12 — ctrl=1 or ctrl=2, both roads.
#
# Walks every pixel position i ∈ [span_start, span_end). For each:
#   p0 = src0[i - s0s]   if  s0s <= i < s0e   else  3
#   p1 = src1[i - s1s]   if  s1s <= i < s1e   else  3
#   slot = merged_idx[(p0 << 3) | p1]
# Then packs two consecutive slots into one mask byte.
#
# The bounds checks are done via unsigned compare on (i - sNs); a
# negative subtraction wraps to a huge unsigned value, which fails
# the sltu against the span width and routes to the p=3 OOB path.
#
# Gap regions (e.g. ctrl=1 with non-overlapping segments) read OOB
# for both srcs → merged_idx[(3<<3)|3] == oob_slot. That matches the
# pre-fill in MASK, so we'd just rewrite oob_pair over itself. Harmless
# and keeps the loop branch-free at the segment-discovery level.
############################################################
    .func pack_case12
pack_case12:
    # Persistent loop registers (live across inner branches):
    #   s6 = src0_base, s7 = src1_base, s8 = merged_idx base
    #   a2 = span0 = s0e - s0s,  a3 = span1 = s1e - s1s
    #   k0 = s0s, k1 = s1s, t9 = span_end
    #   t6 = i (current pixel position), t7 = mask dst byte ptr
    #   ra is preserved (we j to pack_done at exit)
    # gp is reserved by rspq; a0/a1/a2/a3 are free in this inner phase
    # since the rspq command dispatch already consumed them and the only
    # DMA call from here on is pack_done's DMAOut which doesn't touch aN.
    lhu     t0, %lo(DESC) + DESC_S0S
    lhu     t1, %lo(DESC) + DESC_S0E
    lhu     t2, %lo(DESC) + DESC_S1S
    lhu     t3, %lo(DESC) + DESC_S1E
    lhu     t6, %lo(DESC) + DESC_SS                  # i = span_start
    lhu     t9, %lo(DESC) + DESC_SE
    subu    a2, t1, t0                               # span0 = s0e - s0s
    subu    a3, t3, t2                               # span1 = s1e - s1s
    move    k0, t0                                    # s0s
    move    k1, t2                                    # s1s

    lw      s6, %lo(SRC0_BASE)
    lw      s7, %lo(SRC1_BASE)
    li      s8, %lo(FRAME_STATE) + FS_MERGED

    li      t7, %lo(MASK)                             # dst byte ptr

.case12_pair_loop:
    # i + 1 < span_end? at least one full pair remaining
    addiu   t0, t6, 1
    sltu    t1, t0, t9
    beqz    t1, .case12_tail
    nop

    # ---- Pixel i (high nibble) ----
    subu    t2, t6, k0                               # off0 = i - s0s
    sltu    t3, t2, a2                               # in [0, span0)
    beqz    t3, .case12a_p0_oob
    nop
    addu    t4, s6, t2
    j       .case12a_p0_done
    lbu     t2, 0(t4)                                # p0_a = src0[off0]
.case12a_p0_oob:
    li      t2, 3
.case12a_p0_done:

    subu    t3, t6, k1                               # off1
    sltu    t4, t3, a3
    beqz    t4, .case12a_p1_oob
    nop
    addu    t4, s7, t3
    j       .case12a_p1_done
    lbu     t3, 0(t4)                                # p1_a
.case12a_p1_oob:
    li      t3, 3
.case12a_p1_done:

    sll     t2, t2, 3
    or      t2, t2, t3                               # idx_a
    addu    t4, s8, t2
    lbu     t2, 0(t4)                                # slot_a

    # ---- Pixel i+1 (low nibble) ----
    addiu   t6, t6, 1                                # advance i
    subu    t3, t6, k0
    sltu    t4, t3, a2
    beqz    t4, .case12b_p0_oob
    nop
    addu    t4, s6, t3
    j       .case12b_p0_done
    lbu     t3, 0(t4)                                # p0_b
.case12b_p0_oob:
    li      t3, 3
.case12b_p0_done:

    subu    t4, t6, k1
    sltu    t5, t4, a3
    beqz    t5, .case12b_p1_oob
    nop
    addu    t5, s7, t4
    j       .case12b_p1_done
    lbu     t4, 0(t5)                                # p1_b
.case12b_p1_oob:
    li      t4, 3
.case12b_p1_done:

    sll     t3, t3, 3
    or      t3, t3, t4                               # idx_b
    addu    t5, s8, t3
    lbu     t3, 0(t5)                                # slot_b

    # Pack and store.
    sll     t2, t2, 4
    or      t2, t2, t3
    sb      t2, 0(t7)
    addiu   t7, t7, 1
    addiu   t6, t6, 1                                # advance past second px
    j       .case12_pair_loop
    nop

.case12_tail:
    # i < span_end? then one trailing pixel (odd-length span).
    sltu    t0, t6, t9
    beqz    t0, .case12_done
    nop

    subu    t2, t6, k0
    sltu    t3, t2, a2
    beqz    t3, .case12_tail_p0_oob
    nop
    addu    t4, s6, t2
    j       .case12_tail_p0_done
    lbu     t2, 0(t4)
.case12_tail_p0_oob:
    li      t2, 3
.case12_tail_p0_done:

    subu    t3, t6, k1
    sltu    t4, t3, a3
    beqz    t4, .case12_tail_p1_oob
    nop
    addu    t4, s7, t3
    j       .case12_tail_p1_done
    lbu     t3, 0(t4)
.case12_tail_p1_oob:
    li      t3, 3
.case12_tail_p1_done:

    sll     t2, t2, 3
    or      t2, t2, t3
    addu    t4, s8, t2
    lbu     t2, 0(t4)
    sll     t2, t2, 4
    # Low nibble: keep the oob_slot from pre-fill.
    lbu     t3, 0(t7)
    andi    t3, t3, 0x0F
    or      t2, t2, t3
    sb      t2, 0(t7)

.case12_done:
    j       pack_done
    nop
    .endfunc
