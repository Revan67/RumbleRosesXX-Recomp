#pragma once
#include <cstdint>
#include <type_traits>

// Initialized before guest execution from the SDK's actual heap configuration.
inline uint32_t rrxx_physical_4k_host_offset = 0;
struct RRXXTranslatedBase {
    uint8_t* raw;
    // Function and kernel ABI boundaries continue to receive the unmodified base.
    operator uint8_t*() const { return raw; }
    template <typename T> requires std::is_integral_v<T>
    uint8_t* operator+(T value) const {
        const auto address = static_cast<uint32_t>(value);
        return raw + uint64_t(address) +
            (address >= 0xE0000000u && address < 0xFFD00000u ? rrxx_physical_4k_host_offset : 0u);
    }
};
