/***************************************************************************
    General C++ Helper Functions

    Copyright Chris White.
    See license.txt for more details.

    N64 port: dropped <sstream> in favour of snprintf. std::stringstream
    pulls in the same locale/<ios> machinery that <iostream>/<fstream> do,
    so removing it from this tiny helper file saves a chunk of .text.
***************************************************************************/

#include <cstdio>
#include "utils.hpp"

std::string Utils::to_string(int i)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d", i);
    return std::string(buf);
}

std::string Utils::to_string(char c)
{
    return std::string(1, c);
}

std::string Utils::to_hex_string(int i)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%x", i);
    return std::string(buf);
}

uint32_t Utils::from_hex_string(std::string s)
{
    return (uint32_t) std::strtoul(s.c_str(), nullptr, 16);
}
