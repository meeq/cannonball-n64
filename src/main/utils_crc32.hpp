#pragma once

#include "stdint.hpp"
#include <cstddef>

// CRC-32 (IEEE 802.3, polynomial 0xEDB88320), seed-and-finalize identical to
// boost::crc_32_type. Replaces the boost dependency for the N64 build (and is
// safe to use on PC too).
class Crc32
{
public:
    Crc32() : state(0xFFFFFFFFu) {}

    void process_bytes(const void* data, std::size_t len);

    uint32_t checksum() const { return state ^ 0xFFFFFFFFu; }

private:
    uint32_t state;
};
