/***************************************************************************
    Fixed-width integer types.
***************************************************************************/

#pragma once

#include <stdint.h>

static_assert(sizeof(int8_t)   == 1, "int8_t is not of the correct size");
static_assert(sizeof(int16_t)  == 2, "int16_t is not of the correct size");
static_assert(sizeof(int32_t)  == 4, "int32_t is not of the correct size");
static_assert(sizeof(int64_t)  == 8, "int64_t is not of the correct size");

static_assert(sizeof(uint8_t)  == 1, "uint8_t is not of the correct size");
static_assert(sizeof(uint16_t) == 2, "uint16_t is not of the correct size");
static_assert(sizeof(uint32_t) == 4, "uint32_t is not of the correct size");
static_assert(sizeof(uint64_t) == 8, "uint64_t is not of the correct size");
