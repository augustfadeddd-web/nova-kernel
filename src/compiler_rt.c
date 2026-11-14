/*
 * compiler_rt.c — the handful of helpers clang/gcc expect from libgcc.
 *
 * The kernel links without libgcc, and a few DOOM routines (FixedDiv in
 * m_fixed.c, FixedMul's 64-bit shift) need 64-bit division and shifting.
 * Everything here is written with 32-bit-friendly operations so no helper
 * ever calls another one.
 */
#include <stddef.h>
#include <stdint.h>

/* Binary long division: 64 shift/subtract steps.  The remainder invariant
 * (rem < divisor) guarantees the shifted remainder fits after subtraction,
 * even when the shift carries out of the top bit. */
static uint64_t divide(uint64_t dividend, uint64_t divisor, uint64_t *remainder)
{
    uint64_t quotient = 0;
    uint64_t rem = 0;
    int bit;

    for (bit = 63; bit >= 0; --bit) {
        uint64_t carried = rem >> 63;
        rem = (rem << 1) | ((dividend >> bit) & 1u);
        if (carried || rem >= divisor) {
            rem -= divisor;
            quotient |= (uint64_t)1 << bit;
        }
    }
    if (remainder) *remainder = rem;
    return quotient;
}

uint64_t __udivmoddi4(uint64_t dividend, uint64_t divisor, uint64_t *remainder)
{
    if (!divisor) {
        if (remainder) *remainder = 0;
        return 0;
    }
    return divide(dividend, divisor, remainder);
}

uint64_t __udivdi3(uint64_t dividend, uint64_t divisor)
{
    return __udivmoddi4(dividend, divisor, NULL);
}

uint64_t __umoddi3(uint64_t dividend, uint64_t divisor)
{
    uint64_t remainder = 0;
    __udivmoddi4(dividend, divisor, &remainder);
    return remainder;
}

int64_t __divdi3(int64_t dividend, int64_t divisor)
{
    int negative = (dividend < 0) != (divisor < 0);
    uint64_t magnitude = divide((uint64_t)(dividend < 0 ? -dividend : dividend),
                                (uint64_t)(divisor < 0 ? -divisor : divisor),
                                NULL);
    return negative ? -(int64_t)magnitude : (int64_t)magnitude;
}

int64_t __moddi3(int64_t dividend, int64_t divisor)
{
    uint64_t remainder = 0;
    int negative = dividend < 0;
    divide((uint64_t)(dividend < 0 ? -dividend : dividend),
           (uint64_t)(divisor < 0 ? -divisor : divisor), &remainder);
    return negative ? -(int64_t)remainder : (int64_t)remainder;
}

int64_t __ashldi3(int64_t value, int count)
{
    uint64_t bits = (uint64_t)value;
    uint32_t low = (uint32_t)bits;
    uint32_t high = (uint32_t)(bits >> 32);
    if (count <= 0) return value;
    if (count >= 64) return 0;
    if (count >= 32) {
        high = low << (count - 32);
        low = 0;
    } else {
        high = (high << count) | (low >> (32 - count));
        low <<= count;
    }
    return (int64_t)(((uint64_t)high << 32) | low);
}

int64_t __ashrdi3(int64_t value, int count)
{
    uint64_t bits = (uint64_t)value;
    uint32_t low = (uint32_t)bits;
    uint32_t high = (uint32_t)(bits >> 32);
    if (count <= 0) return value;
    if (count >= 64) return value < 0 ? -1 : 0;
    if (count >= 32) {
        low = high;
        high = (uint32_t)((int32_t)high >> (count - 32));
    } else {
        low = (low >> count) | (high << (32 - count));
        high = (uint32_t)((int32_t)high >> count);
    }
    return (int64_t)(((uint64_t)high << 32) | low);
}

uint64_t __lshrdi3(uint64_t value, int count)
{
    uint32_t low = (uint32_t)value;
    uint32_t high = (uint32_t)(value >> 32);
    if (count <= 0) return value;
    if (count >= 64) return 0;
    if (count >= 32) {
        low = high >> (count - 32);
        high = 0;
    } else {
        low = (low >> count) | (high << (32 - count));
        high >>= count;
    }
    return ((uint64_t)high << 32) | low;
}
