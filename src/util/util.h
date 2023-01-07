#pragma once

#include <cstdio>

#define UNUSED(x) (void)(x)

#ifdef LOGGING_ENABLED

#define LOG(...) fprintf(stdout, __VA_ARGS__)
#define LOG_INFO(...) fprintf(stdout, __VA_ARGS__)
#define LOG_ERROR(...) fprintf(stderr, __VA_ARGS__)

#else
#define LOG(...) fprintf(stdout, __VA_ARGS__)
#define LOG_INFO(...)                                                                                        \
    do {                                                                                                     \
    } while (0)
#define LOG_ERROR(...)                                                                                       \
    do {                                                                                                     \
    } while (0)
#endif

template <unsigned N> constexpr bool is_power_of_two() {
    static_assert(N <= 32768, "N must be N <= 32768");
    return (N == 1 || N == 2 || N == 4 || N == 8 || N == 16 || N == 32 || N == 64 || N == 128 || N == 256 ||
            N == 512 || N == 1024 || N == 2048 || N == 4096 || N == 8192 || N == 16384 || N == 32768);
}

template <unsigned N> constexpr unsigned log2() {
    static_assert(is_power_of_two<N>(), "N must be a power of 2");
    unsigned val = N;
    unsigned ret = 0;
    while (val > 1) {
        val >>= 1;
        ret++;
    }

    return ret;
}

// Sanity checks
static_assert(log2<1>() == 0);
static_assert(log2<2>() == 1);
static_assert(log2<4>() == 2);
static_assert(log2<8>() == 3);
static_assert(log2<16>() == 4);
static_assert(log2<1024>() == 10);
static_assert(log2<2048>() == 11);
static_assert(log2<4096>() == 12);
