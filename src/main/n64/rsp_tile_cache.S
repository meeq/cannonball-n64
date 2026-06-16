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
