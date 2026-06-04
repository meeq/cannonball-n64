// Minimal 16-bit PCM WAV writer for the audio-render asset pipeline.
// Header is finalized in close() once the total sample count is known.
#pragma once

#include <cstdint>
#include <cstdio>

class WavWriter
{
public:
    WavWriter();
    ~WavWriter();

    bool open(const char* path, uint32_t sample_rate, uint16_t channels);

    // count = number of int16_t values (already interleaved if stereo).
    bool append(const int16_t* samples, size_t count);

    bool close();

private:
    std::FILE* f;
    uint32_t   sample_rate;
    uint16_t   channels;
    uint32_t   data_bytes;
};
