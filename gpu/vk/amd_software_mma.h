#pragma once

namespace amd_software_mma {

constexpr bool use_broadcast_fallback(bool amd_proprietary, bool software_mma) noexcept {
    return amd_proprietary && software_mma;
}

}  // namespace amd_software_mma
