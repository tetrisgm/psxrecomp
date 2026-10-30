#ifndef PSX_PGXP_SCALE_H
#define PSX_PGXP_SCALE_H
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
/* Exact scaling of normal binary64 values whose results stay normal. No
 * rounding or floating exception is possible there. Preserve the library's
 * behavior for zeros, subnormals, infinities, NaNs and exponent boundaries. */
static inline double pgxp_scale_pow2(double value, int shift) {
    if constexpr (sizeof(double) == sizeof(uint64_t) &&
                  std::numeric_limits<double>::is_iec559) {
        uint64_t bits;
        std::memcpy(&bits, &value, sizeof bits);
        const unsigned exponent = unsigned((bits >> 52) & 0x7ffu);
        const int target = int(exponent) + shift;
        if (exponent && exponent != 0x7ffu && target > 0 && target < 0x7ff) {
            bits = (bits & ~(UINT64_C(0x7ff) << 52)) | (uint64_t(target) << 52);
            std::memcpy(&value, &bits, sizeof value);
            return value;
        }
    }
    return std::ldexp(value, shift);
}
#endif
