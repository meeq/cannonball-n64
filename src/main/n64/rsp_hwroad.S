####################################################################
# hwroad_rsp.S — RSP overlay for the Cannonball N64 port.
#
# Offloads hwroad foreground rasterization (the 320x224 per-pixel road
# blit in HWRoad::render_foreground_lores at src/main/hwvideo/hwroad.cpp)
# from the R4300 to the RSP.
#
# Command 0 — HWROAD_CMD_DRAW_FRAME:
#   a0 = num_scanlines (1..224)  — small int, safe to share top byte with cmd_id
#   a1 = descriptor array in RDRAM (224 × 64 bytes, see Descriptor below)
#   a2 = scratch surface base in RDRAM (320×224 RGBA5551, 16-aligned)
#   a3 = unused
#
# NOTE: a0 carries the rspq command word, whose top 8 bits are the overlay
# ID + cmd index. We must mask those off before using a0 as a value (here,
# `andi a0, 0xFFFF` is sufficient since num_scanlines fits in 16 bits).
#
# Per-scanline descriptor (64 bytes, 16-aligned):
#   0x00  u8   y               0..223
#   0x01  u8   control_flags   bits[0:1]=control(0..3), bit7=skip-scanline
#   0x02  u16  hpos0           12-bit
#   0x04  u16  hpos1           12-bit
#   0x06  u16  reserved
#   0x08  u32  src0_rdram_addr ptr to 512-byte row in roads[] (or dummy row)
#   0x0c  u32  src1_rdram_addr ptr to 512-byte row in roads[]
#   0x10  u16[16] color_table  16 colors: 0..7 road0 then 0..7 road1, RGBA5551
#   0x30  u16   reserved (padding to 64 bytes)
#
# DMEM layout (4 KB total, _ovl_data_start anchors the working buffers):
#   0x000  desc        64 bytes — current scanline descriptor
#   0x040  scratch     640 bytes — output row, 320 RGBA5551 px
#   0x2c0  src0        512 bytes — road 0 texture row
#   0x4c0  src1        512 bytes — road 1 texture row
#   0x6c0  merged_lut  128 bytes — 64 × u16 (case 1/2 priority resolution)
#   0x740+ spare
#
# The actual layout is allocated via .bss labels below — these comments are
# documentation. Sizes match.
####################################################################

#include <rsp_queue.inc>

.set noreorder
.set at

####################################################################
# Data segment — overlay header, command table, constants.
####################################################################
.data

RSPQ_BeginOverlayHeader
    # 12 bytes = 3 words = (cmd|arg0) + arg1 + arg2. CPU's rspq_write(ovl,
    # cmd, a, b, c) emits exactly 3 words; declaring 16 here would cause the
    # RSP to also "consume" the first word of the *next* command, corrupting
    # the queue stream and stalling on the next iteration.
    RSPQ_DefineCommand HWRoad_DrawFrame, 12   # cmd 0: 3 words
RSPQ_EndOverlayHeader

# Priority maps from hwroad.cpp:327 — embedded constants. Case 1 uses
# priority_map[0]; case 2 uses priority_map[1].
.align 4
PRIORITY_MAP_0: .byte 0x80, 0x81, 0x81, 0x87, 0x00, 0x00, 0x00, 0x00
PRIORITY_MAP_1: .byte 0x81, 0x81, 0x81, 0x8f, 0x00, 0x00, 0x00, 0x80

# Saved state — empty for this overlay (no persistent registers between
# frames; CPU rebuilds the descriptor array every frame).
RSPQ_BeginSavedState
    .quad 0
RSPQ_EndSavedState

####################################################################
# BSS — DMEM working buffers, addressed via labels.
####################################################################
.bss

.align 4
DESC:        .space 64
.align 4
SCRATCH:     .space 640      # 320 px × 2 bytes (RGBA5551)
.align 4
# SP DMA ignores the low 3 bits of the RDRAM source address (rsp_dma.inc:43).
# The roads[] member of HWRoad sits at offset +12 inside the C++ object so
# every per-scanline src*_phys is 4-mod-8 aligned. To keep the bytes we
# actually want fully covered we pad SRC0/SRC1 to 520+ bytes and DMA 520:
# the DMA fetches from (src_phys & ~7) onward, and the s4 return points at
# the originally-requested byte (stashed below in SRC0_BASE/SRC1_BASE).
SRC0:        .space 528
.align 4
SRC1:        .space 528
.align 4
SRC0_BASE:   .space 4        # %lo(SRC0) + (src0_phys & 7), set after each DMA
SRC1_BASE:   .space 4        # %lo(SRC1) + (src1_phys & 7)
.align 4
MERGED_LUT:  .space 128      # 64 × u16

####################################################################
# Code segment.
####################################################################
.text

# Constants
#define DESC_Y          0x00
#define DESC_CTRL       0x01
#define DESC_HPOS0      0x02
#define DESC_HPOS1      0x04
#define DESC_SRC0_ADDR  0x08
#define DESC_SRC1_ADDR  0x0c
#define DESC_COLOR      0x10
#define DESC_SIZE       64

#define PIXELS_PER_LINE 320
#define BYTES_PER_LINE  640

# CTRL bit 7 = skip-scanline.
#define CTRL_SKIP_MASK  0x80
#define CTRL_CASE_MASK  0x03

############################################################
# HWRoad_DrawFrame: process num_scanlines scanlines.
#
# Inputs (from rspq prefetch):
#   a0 = desc_array_rdram
#   a1 = scratch_rdram_base
#   a2 = num_scanlines
#
# Strategy:
#   for each scanline:
#     1. DMA descriptor (64 B) → DESC
#     2. If skip bit set, advance and continue (no DMA, no write)
#        Otherwise:
#     3. DMA src0 row (512 B) → SRC0
#     4. DMA src1 row (512 B) → SRC1
#     5. Dispatch on control bits → render into SCRATCH
#     6. DMA SCRATCH (640 B) → scratch_rdram + y*640
############################################################
    .func HWRoad_DrawFrame
HWRoad_DrawFrame:
    # Stash inputs into callee-saved regs (s0..s7) so DMA macros which
    # clobber t0..t3, s0, s4 don't lose them.
    # a0's top byte has the rspq cmd word — mask before use.
    andi    s3, a0, 0xFFFF  # num_scanlines
    move    s1, a1          # desc array RDRAM ptr
    move    s2, a2          # scratch RDRAM base
    li      s5, 0           # y index (0..223)

scanline_loop:
    blez    s3, frame_done
    nop

    # --- 1. DMA descriptor in.
    move    s0, s1                  # RDRAM source
    li      s4, %lo(DESC)           # DMEM dest
    jal     DMAIn
    li      t0, DMA_SIZE(DESC_SIZE, 1)

    # --- 2. Skip check.
    lbu     t0, %lo(DESC) + DESC_CTRL
    andi    t1, t0, CTRL_SKIP_MASK
    bnez    t1, advance
    nop

    # --- 3/4. DMA src0, src1 rows in.
    # Request 520 bytes (512 wanted + up to 7 of leading misalignment, rounded
    # to a multiple of 8). DMAIn returns s4 pointing at the first requested
    # byte regardless of (src_phys & 7), which we stash for the render cases.
    lw      s0, %lo(DESC) + DESC_SRC0_ADDR
    li      s4, %lo(SRC0)
    jal     DMAIn
    li      t0, DMA_SIZE(520, 1)
    sw      s4, %lo(SRC0_BASE)

    lw      s0, %lo(DESC) + DESC_SRC1_ADDR
    li      s4, %lo(SRC1)
    jal     DMAIn
    li      t0, DMA_SIZE(520, 1)
    sw      s4, %lo(SRC1_BASE)

    # --- 5. Dispatch.
    # case 0 → render_case0  (single road 0)
    # case 1 → render_case1  (dual road, road 0 priority)
    # case 2 → render_case2  (dual road, road 1 priority)
    # case 3 → render_case3  (single road 1)
    # NB: DMAIn clobbers t2 (sets it to DMA_IN & 0xF == 0x0C on return), so
    # the case index must be (re)derived from DMEM here, not stashed in t2
    # ahead of the DMAs.
    lbu     t0, %lo(DESC) + DESC_CTRL
    andi    t2, t0, CTRL_CASE_MASK
    beq     t2, $0, render_case0
    nop
    li      t3, 1
    beq     t2, t3, render_case1
    nop
    li      t3, 2
    beq     t2, t3, render_case2
    nop
    # fall through to case 3
    j       render_case3
    nop

render_done:
    # --- 6. DMA scratch row out.
    lbu     t0, %lo(DESC) + DESC_Y          # scanline y
    sll     t0, t0, 7                       # y * 128
    sll     t1, t0, 2                       # y * 512
    addu    t0, t0, t1                      # y * 640
    addu    s0, s2, t0                      # RDRAM dest
    li      s4, %lo(SCRATCH)
    jal     DMAOut
    li      t0, DMA_SIZE(BYTES_PER_LINE, 1)

advance:
    addiu   s1, s1, DESC_SIZE               # next descriptor
    addiu   s3, s3, -1                      # remaining--
    j       scanline_loop
    addiu   s5, s5, 1                       # y++ (unused but cheap)

frame_done:
    j       RSPQ_Loop
    nop
    .endfunc

############################################################
# render_case0 — single road 0.
#
# Inner loop (per pixel):
#   pix = (hpos0 < 0x200) ? src0[hpos0] : 3
#   *dst++ = color_table[pix]
#   hpos0 = (hpos0 + 1) & 0xfff
############################################################
    .func render_case0
render_case0:
    lhu     t0, %lo(DESC) + DESC_HPOS0      # hpos0
    li      t1, %lo(SCRATCH)                # dst
    lw      t2, %lo(SRC0_BASE)              # src base (DMA-aligned, see SRC0 note)
    li      t3, %lo(DESC) + DESC_COLOR      # color_table base (road 0 slots 0..7)
    li      t4, PIXELS_PER_LINE             # pixel count
    li      t5, 0x200                       # bounds
    li      t6, 0xfff                       # hpos wrap
.case0_loop:
    sltu    t7, t0, t5                      # t7 = hpos0 < 0x200
    beqz    t7, .case0_oob
    nop
    addu    t8, t2, t0
    j       .case0_lookup
    lbu     t9, 0(t8)                       # pix = src0[hpos0]
.case0_oob:
    li      t9, 3
.case0_lookup:
    sll     t9, t9, 1                       # × 2 bytes
    addu    t9, t3, t9
    lhu     t9, 0(t9)                       # color = color_table[pix]
    sh      t9, 0(t1)
    addiu   t1, t1, 2
    addiu   t0, t0, 1
    and     t0, t0, t6
    addiu   t4, t4, -1
    bgtz    t4, .case0_loop
    nop
    j       render_done
    nop
    .endfunc

############################################################
# render_case3 — single road 1. Same as case 0 but reads SRC1 and
# uses color_table[0x10..] (slots 8..15 in the 16-entry packing).
############################################################
    .func render_case3
render_case3:
    lhu     t0, %lo(DESC) + DESC_HPOS1
    li      t1, %lo(SCRATCH)
    lw      t2, %lo(SRC1_BASE)                # src1 base (DMA-aligned)
    li      t3, %lo(DESC) + DESC_COLOR + 16   # color_table base (road 1)
    li      t4, PIXELS_PER_LINE
    li      t5, 0x200
    li      t6, 0xfff
.case3_loop:
    sltu    t7, t0, t5
    beqz    t7, .case3_oob
    nop
    addu    t8, t2, t0
    j       .case3_lookup
    lbu     t9, 0(t8)
.case3_oob:
    li      t9, 3
.case3_lookup:
    sll     t9, t9, 1
    addu    t9, t3, t9
    lhu     t9, 0(t9)
    sh      t9, 0(t1)
    addiu   t1, t1, 2
    addiu   t0, t0, 1
    and     t0, t0, t6
    addiu   t4, t4, -1
    bgtz    t4, .case3_loop
    nop
    j       render_done
    nop
    .endfunc

############################################################
# render_case1 — both roads, road 0 priority.
#
# Pre-loop: build merged_lut[64].
#   for p0 in 0..7:
#     mask = PRIORITY_MAP_0[p0]
#     c0   = color_table[0x00 + p0]
#     for p1 in 0..7:
#       merged[(p0<<3)|p1] = ((mask >> p1) & 1)
#                            ? color_table[0x10 + p1]   # road 1 wins
#                            : c0                       # road 0 wins
#
# Inner loop (per pixel):
#   pix0 = (hpos0 < 0x200) ? src0[hpos0] : 3
#   pix1 = (hpos1 < 0x200) ? src1[hpos1] : 3
#   *dst++ = merged_lut[(pix0 << 3) | pix1]
#   hpos0++; hpos1++; both wrap at 0xfff
############################################################
    .func render_case1
render_case1:
    # ---- Build merged_lut[64] from PRIORITY_MAP_0 + color_table.
    li      t0, %lo(PRIORITY_MAP_0)
    li      t1, %lo(DESC) + DESC_COLOR          # color_table (road 0)
    li      t2, %lo(DESC) + DESC_COLOR + 16     # color_table (road 1)
    li      t3, %lo(MERGED_LUT)
    li      t4, 8                                # p0 counter
.case1_build_p0:
    lbu     t5, 0(t0)                            # mask = PRIORITY_MAP_0[p0]
    lhu     t6, 0(t1)                            # c0 = ct[0x00 + p0]
    li      t7, 8                                # p1 counter
    move    t8, t2                               # &ct[0x10] pointer for inner
    move    t9, t5                               # mask copy we shift
.case1_build_p1:
    andi    s6, t9, 1                            # bit 0
    bnez    s6, .case1_use_road1
    nop
    j       .case1_store
    move    s7, t6                               # else c0
.case1_use_road1:
    lhu     s7, 0(t8)                            # ct[0x10 + p1]
.case1_store:
    sh      s7, 0(t3)
    addiu   t3, t3, 2
    addiu   t8, t8, 2
    srl     t9, t9, 1
    addiu   t7, t7, -1
    bgtz    t7, .case1_build_p1
    nop
    addiu   t0, t0, 1                            # next priority byte
    addiu   t1, t1, 2                            # next c0 slot
    addiu   t4, t4, -1
    bgtz    t4, .case1_build_p0
    nop

    # ---- Per-pixel loop.
    lhu     t0, %lo(DESC) + DESC_HPOS0           # hpos0
    lhu     t1, %lo(DESC) + DESC_HPOS1           # hpos1
    li      t2, %lo(SCRATCH)                     # dst
    lw      t3, %lo(SRC0_BASE)                   # src0 base (DMA-aligned)
    lw      t4, %lo(SRC1_BASE)                   # src1 base (DMA-aligned)
    li      t5, %lo(MERGED_LUT)                  # lut base
    li      t6, PIXELS_PER_LINE                  # pixel count
    li      t7, 0x200                            # bounds
    li      t8, 0xfff                            # hpos wrap
.case1_loop:
    # pix0
    sltu    s6, t0, t7
    beqz    s6, .case1_p0_oob
    nop
    addu    s7, t3, t0
    j       .case1_p0_done
    lbu     s6, 0(s7)
.case1_p0_oob:
    li      s6, 3
.case1_p0_done:
    # pix1
    sltu    s7, t1, t7
    beqz    s7, .case1_p1_oob
    nop
    addu    s7, t4, t1
    j       .case1_p1_done
    lbu     s7, 0(s7)
.case1_p1_oob:
    li      s7, 3
.case1_p1_done:
    # idx = (pix0 << 3) | pix1
    sll     s6, s6, 3
    or      s6, s6, s7
    sll     s6, s6, 1                            # × 2 (u16)
    addu    s6, t5, s6
    lhu     s6, 0(s6)
    sh      s6, 0(t2)
    addiu   t2, t2, 2
    addiu   t0, t0, 1
    and     t0, t0, t8
    addiu   t1, t1, 1
    and     t1, t1, t8
    addiu   t6, t6, -1
    bgtz    t6, .case1_loop
    nop
    j       render_done
    nop
    .endfunc

############################################################
# render_case2 — both roads, road 1 priority.
#
# Same merged-LUT trick as case 1, but using PRIORITY_MAP_1 and the
# inverse selection (mask bit set → use color_table[0x10 + p1],
# otherwise color_table[0x00 + p0]). Per the source: if priority bit
# set, road 1 wins; otherwise road 0 wins.
############################################################
    .func render_case2
render_case2:
    # ---- Build merged_lut[64] from PRIORITY_MAP_1.
    # merged[(p0<<3)|p1] = (PRIORITY_MAP_1[p0] >> p1) & 1
    #                     ? color_table[0x10 + p1]
    #                     : color_table[0x00 + p0]
    li      t0, %lo(PRIORITY_MAP_1)
    li      t1, %lo(DESC) + DESC_COLOR
    li      t2, %lo(DESC) + DESC_COLOR + 16
    li      t3, %lo(MERGED_LUT)
    li      t4, 8
.case2_build_p0:
    lbu     t5, 0(t0)
    lhu     t6, 0(t1)
    li      t7, 8
    move    t8, t2
    move    t9, t5
.case2_build_p1:
    andi    s6, t9, 1
    bnez    s6, .case2_use_road1
    nop
    j       .case2_store
    move    s7, t6
.case2_use_road1:
    lhu     s7, 0(t8)
.case2_store:
    sh      s7, 0(t3)
    addiu   t3, t3, 2
    addiu   t8, t8, 2
    srl     t9, t9, 1
    addiu   t7, t7, -1
    bgtz    t7, .case2_build_p1
    nop
    addiu   t0, t0, 1
    addiu   t1, t1, 2
    addiu   t4, t4, -1
    bgtz    t4, .case2_build_p0
    nop

    # Per-pixel loop — identical to case 1 (the LUT is the only difference).
    lhu     t0, %lo(DESC) + DESC_HPOS0
    lhu     t1, %lo(DESC) + DESC_HPOS1
    li      t2, %lo(SCRATCH)
    lw      t3, %lo(SRC0_BASE)
    lw      t4, %lo(SRC1_BASE)
    li      t5, %lo(MERGED_LUT)
    li      t6, PIXELS_PER_LINE
    li      t7, 0x200
    li      t8, 0xfff
.case2_loop:
    sltu    s6, t0, t7
    beqz    s6, .case2_p0_oob
    nop
    addu    s7, t3, t0
    j       .case2_p0_done
    lbu     s6, 0(s7)
.case2_p0_oob:
    li      s6, 3
.case2_p0_done:
    sltu    s7, t1, t7
    beqz    s7, .case2_p1_oob
    nop
    addu    s7, t4, t1
    j       .case2_p1_done
    lbu     s7, 0(s7)
.case2_p1_oob:
    li      s7, 3
.case2_p1_done:
    sll     s6, s6, 3
    or      s6, s6, s7
    sll     s6, s6, 1
    addu    s6, t5, s6
    lhu     s6, 0(s6)
    sh      s6, 0(t2)
    addiu   t2, t2, 2
    addiu   t0, t0, 1
    and     t0, t0, t8
    addiu   t1, t1, 1
    and     t1, t1, t8
    addiu   t6, t6, -1
    bgtz    t6, .case2_loop
    nop
    j       render_done
    nop
    .endfunc
