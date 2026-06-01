#include "utils_crc32.hpp"

namespace
{
    struct Table
    {
        uint32_t v[256];
        Table()
        {
            for (uint32_t i = 0; i < 256; ++i)
            {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k)
                    c = (c >> 1) ^ (0xEDB88320u & -(int32_t)(c & 1));
                v[i] = c;
            }
        }
    };
    const Table table;
}

void Crc32::process_bytes(const void* data, std::size_t len)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t s = state;
    while (len--)
        s = table.v[(s ^ *p++) & 0xFFu] ^ (s >> 8);
    state = s;
}
