/***************************************************************************
    Data Types.

    The Boost library is only used to enforce data type size at compile
    time.

    If you're sure the sizes are correct, it can be removed for your port.

    Copyright Chris White.
    See license.txt for more details.
***************************************************************************/

#pragma once

/** C99 Standard Naming */
#if defined(_MSC_VER)
    typedef signed char int8_t;
    typedef signed short int16_t;
    typedef signed int int32_t;
    typedef signed long long int64_t;

    typedef unsigned char uint8_t;
    typedef unsigned short uint16_t;
    typedef unsigned int uint32_t;
    typedef unsigned long long uint64_t;
#else
    #include <stdint.h>
#endif

#if defined(WITH_BOOST)
    #include <boost/static_assert.hpp>
    #define CB_STATIC_ASSERT(expr, msg) BOOST_STATIC_ASSERT_MSG(expr, msg)
#else
    #define CB_STATIC_ASSERT(expr, msg) static_assert(expr, msg)
#endif

/* Report typedef errors */
CB_STATIC_ASSERT(sizeof(int8_t)   == 1, "int8_t is not of the correct size" );
CB_STATIC_ASSERT(sizeof(int16_t)  == 2, "int16_t is not of the correct size");
CB_STATIC_ASSERT(sizeof(int32_t)  == 4, "int32_t is not of the correct size");
CB_STATIC_ASSERT(sizeof(int64_t)  == 8, "int64_t is not of the correct size");

CB_STATIC_ASSERT(sizeof(uint8_t)  == 1, "int8_t is not of the correct size" );
CB_STATIC_ASSERT(sizeof(uint16_t) == 2, "int16_t is not of the correct size");
CB_STATIC_ASSERT(sizeof(uint32_t) == 4, "int32_t is not of the correct size");
CB_STATIC_ASSERT(sizeof(uint64_t) == 8, "int64_t is not of the correct size");
