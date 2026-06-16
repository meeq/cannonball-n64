####################################################################
# rsp_tile_cache.S — RSP overlay for the Cannonball N64 port.
#
# Future home of the BG/FG tile-layer cache rebuilder (currently done on
# CPU at ~28 ms per layer, stuttering frames at scene transitions; the
# plan is to move that work here and run it in parallel with CPU work).
#
# Phase 1 (this file): scaffolding. The overlay registers, accepts a
# TestPing command that echoes a payload XORed with 0xDEADBEEF plus a
# CAFEBABE sentinel back to a CPU-supplied RDRAM address via SP DMA, and
# returns to the queue dispatcher. End-to-end CPU→RSP→DMA→CPU wiring is
# proven before any real cache-rebuild logic lands.
#
# Phase 1 command:
#   Cmd 0 — TestPing:
#     a0 = status_phys_addr  (RDRAM, 8-byte aligned). RSP writes 8 bytes
#                            here on completion.
#     a1 = magic_payload     RSP echoes (a1 XOR 0xDEADBEEF) into
#                            status[0], CAFEBABE sentinel into status[1].
#     a2 = unused
####################################################################

#include <rsp_queue.inc>

.set noreorder
.set at

####################################################################
# Data segment — overlay header, command table.
####################################################################
.data

RSPQ_BeginOverlayHeader
    # 12 bytes = 3 words = (cmd|arg0) + arg1 + arg2.
    RSPQ_DefineCommand TileCache_TestPing, 12   # cmd 0
    RSPQ_DefineCommand TileCache_FillCache, 12  # cmd 1
RSPQ_EndOverlayHeader

# No persistent state between calls in Phase 1.
RSPQ_BeginSavedState
    .quad 0
RSPQ_EndSavedState

####################################################################
# BSS — DMEM scratch.
####################################################################
.bss

.align 4
DMEM_SCRATCH: .space 16

# 2 KiB scratch of zeros that the FillCache loop DMAs out repeatedly.
# Filled once on the first call (zero-init isn't guaranteed across RSP
# overlay loads — be defensive). Stays at zero forever after.
.align 4
ZERO_CHUNK:   .space 2048

# 8-byte status word DMA'd to RDRAM when FillCache completes.
.align 4
DONE_FLAG:    .space 8

####################################################################
# Code segment.
####################################################################
.text

####################################################################
# TileCache_TestPing — Phase 1 scaffolding.
#
# Inputs:
#   a0 = status_phys (RDRAM ptr, 8-byte aligned for DMA)
#   a1 = magic_payload
#   a2 = unused
#
# Writes 8 bytes at status_phys: word 0 = (a1 ^ 0xDEADBEEF), word 1 =
# 0xCAFEBABE. Both via a single SP DMA-out from DMEM_SCRATCH.
####################################################################
    .func TileCache_TestPing
TileCache_TestPing:
    lui     t0, 0xDEAD
    ori     t0, t0, 0xBEEF
    xor     t1, a1, t0
    sw      t1, %lo(DMEM_SCRATCH) + 0(zero)

    lui     t2, 0xCAFE
    ori     t2, t2, 0xBABE
    sw      t2, %lo(DMEM_SCRATCH) + 4(zero)

    move    s0, a0
    li      s4, %lo(DMEM_SCRATCH)
    jal     DMAOut
    li      t0, DMA_SIZE(8, 1)

    j       RSPQ_Loop
    nop
    .endfunc

####################################################################
# TileCache_FillCache — chunked SP-DMA fill of a contiguous RDRAM range.
#
# Inputs:
#   a0 = cache_phys   (RDRAM ptr, 8-byte aligned)
#   a1 = total_bytes  (multiple of 2048)
#   a2 = status_phys  (RDRAM ptr; RSP writes 0xDEADC0DE here on completion)
#
# Strategy:
#   1. Prime ZERO_CHUNK with all zeros (defensive — BSS init may not
#      guarantee zero on overlay swap).
#   2. Loop: DMA ZERO_CHUNK (2 KiB) → cache_phys at increasing offset
#      until total_bytes have been written.
#   3. Write 0xDEADC0DE to status_phys.
#
# Assert codes (high half of $at on RSP halt):
#   0xFC10 — cache_phys (a0) zero on entry
#   0xFC11 — total_bytes (a1) zero on entry
#   0xFC12 — status_phys (a2) zero on entry
#   0xFC13 — total_bytes not a multiple of 2048
#   0xFC14 — total_bytes exceeds 1 MiB (sanity bound)
####################################################################
    .func TileCache_FillCache
TileCache_FillCache:
    # Tripwires on the arguments.
    assert_ne a0, zero, 0xFC10
    assert_ne a1, zero, 0xFC11
    assert_ne a2, zero, 0xFC12

    # Mask a0's top byte (cmd routing bits) so we have a clean RDRAM dest.
    # SP DMA ignores them in practice, but masking lets the asserts
    # compare expected addresses against actual without surprise.
    li      t9, 0xFFFFFF
    and     s1, a0, t9                  # s1 = cache_phys cursor
    and     s2, a1, t9                  # s2 = bytes_remaining
    and     s3, a2, t9                  # s3 = status_phys

    # Sanity: total_bytes must be a positive multiple of 2 KiB and ≤ 1 MiB.
    andi    t0, s2, 0x7FF
    assert_eq t0, zero, 0xFC13
    li      t1, 0x100000
    assert_le s2, t1, 0xFC14

    # Prime ZERO_CHUNK with zeros. 256 iterations × sw zero × 2 = 2048 B.
    li      t0, %lo(ZERO_CHUNK)
    li      t1, 2048
prime_loop:
    sw      zero, 0(t0)
    sw      zero, 4(t0)
    addi    t1, t1, -8
    bgtz    t1, prime_loop
    addi    t0, t0, 8

    # DMA 2 KiB chunks of zeros until done.
fill_loop:
    blez    s2, fill_done
    nop

    move    s0, s1                      # RDRAM dest
    li      s4, %lo(ZERO_CHUNK)         # DMEM source
    jal     DMAOut
    li      t0, DMA_SIZE(2048, 1)

    addi    s1, s1, 2048
    j       fill_loop
    addi    s2, s2, -2048

fill_done:
    # Stage DEADC0DE in DONE_FLAG, DMA 8 bytes out to status_phys.
    lui     t0, 0xDEAD
    ori     t0, t0, 0xC0DE
    sw      t0, %lo(DONE_FLAG) + 0(zero)
    sw      zero, %lo(DONE_FLAG) + 4(zero)

    move    s0, s3                      # RDRAM dest
    li      s4, %lo(DONE_FLAG)
    jal     DMAOut
    li      t0, DMA_SIZE(8, 1)

    j       RSPQ_Loop
    nop
    .endfunc
