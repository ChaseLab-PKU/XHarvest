#ifndef __SGX_NVMEV_COMMON_H
#define __SGX_NVMEV_COMMON_H

#include <cstdio>

inline uint64_t rdtsc()
{
    uint32_t hi, lo;
    __asm__ __volatile__("" : : : "memory");
    __asm__ __volatile__("rdtsc"
                         : "=a"(lo), "=d"(hi));
    __asm__ __volatile__("" : : : "memory");

    return (uint64_t(hi) << 32) | uint64_t(lo);
}

#endif // SGX_COMMON_H