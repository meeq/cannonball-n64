#include "wav_writer.hpp"

#include <cstring>

namespace
{
    void write_u16le(std::FILE* f, uint16_t v)
    {
        uint8_t b[2] = { uint8_t(v & 0xFF), uint8_t((v >> 8) & 0xFF) };
        std::fwrite(b, 1, 2, f);
    }

    void write_u32le(std::FILE* f, uint32_t v)
    {
        uint8_t b[4] = {
            uint8_t( v        & 0xFF),
            uint8_t((v >>  8) & 0xFF),
            uint8_t((v >> 16) & 0xFF),
            uint8_t((v >> 24) & 0xFF),
        };
        std::fwrite(b, 1, 4, f);
    }
}

WavWriter::WavWriter()
    : f(nullptr), sample_rate(0), channels(0), data_bytes(0) {}

WavWriter::~WavWriter() { close(); }

bool WavWriter::open(const char* path, uint32_t rate, uint16_t ch)
{
    close();
    f = std::fopen(path, "wb");
    if (!f) return false;
    sample_rate = rate;
    channels    = ch;
    data_bytes  = 0;

    // RIFF header — sizes patched in close().
    std::fwrite("RIFF", 1, 4, f);
    write_u32le(f, 0);                    // placeholder for chunk size
    std::fwrite("WAVE", 1, 4, f);

    // fmt subchunk
    std::fwrite("fmt ", 1, 4, f);
    write_u32le(f, 16);                   // PCM fmt size
    write_u16le(f, 1);                    // PCM format
    write_u16le(f, channels);
    write_u32le(f, sample_rate);
    write_u32le(f, sample_rate * channels * 2); // byte rate
    write_u16le(f, channels * 2);         // block align
    write_u16le(f, 16);                   // bits per sample

    // data subchunk header — size patched in close().
    std::fwrite("data", 1, 4, f);
    write_u32le(f, 0);                    // placeholder for data size
    return true;
}

bool WavWriter::append(const int16_t* samples, size_t count)
{
    if (!f) return false;
    size_t bytes = count * 2;
    if (std::fwrite(samples, 1, bytes, f) != bytes) return false;
    data_bytes += (uint32_t)bytes;
    return true;
}

bool WavWriter::close()
{
    if (!f) return false;

    // Patch RIFF chunk size (file size - 8) and data subchunk size.
    std::fseek(f, 4, SEEK_SET);
    write_u32le(f, 36 + data_bytes);
    std::fseek(f, 40, SEEK_SET);
    write_u32le(f, data_bytes);

    std::fclose(f);
    f = nullptr;
    return true;
}
