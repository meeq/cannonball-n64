// Static ROM walker for bake-sprites. See walker.cpp for details.
#pragma once

#include <cstdint>
#include <vector>

class Roms;

struct StaticTuple {
    uint16_t bank;
    uint16_t addr;
    int16_t  pitch;
    uint16_t h;   // max derived height in pixels
};

// Walk the OutRun master CPU ROM's sprite/anim descriptor tables and emit a
// deduplicated set of (bank, addr, pitch, max_h) tuples. The caller bakes
// each tuple with both flip variants.
//
// `sprites_words` is the byte-swapped sprite ROM in the same layout the
// runtime uses (8 banks of 0x10000 words each). The walker uses it as a
// strict validator for brute-force pattern matches: a candidate input_addr
// is accepted only if all 5 of its SIZE sub-descriptors point at sprite
// data that EOR-terminates within MAX_WORDS_PER_ROW words.
//
// `japan` selects the descriptor-table address set: the World/RevB table
// (default) or the corresponding Japan-region table whose addresses are
// shifted per src/main/engine/oaddresses.hpp _J variants.
//
// `phase1_only` skips the two brute-force scans that catch data-driven
// sprites the table walk doesn't reach. Used by the Japan-vs-World audit
// to compare just the descriptor sets that the runtime engine actually
// references (phase 1 = table-derived).
void walk_static_addrs(const Roms& r,
                       const std::vector<uint32_t>& sprites_words,
                       std::vector<StaticTuple>& out,
                       bool japan = false,
                       bool phase1_only = false);
