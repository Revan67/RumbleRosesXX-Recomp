#include <cmath>
#include <cstdint>
#include <immintrin.h>

extern "C" float roundevenf(float value)
{
    return std::nearbyint(value);
}

extern "C" __attribute__((target("f16c"))) uint16_t __truncsfhf2(float value)
{
    return static_cast<uint16_t>(_cvtss_sh(value, 0));
}
