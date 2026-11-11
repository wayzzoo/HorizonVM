#include "platform/tegra210.h"

#include <cstdint>

namespace hvm::tegra {
    constexpr std::uint64_t iram_base = T210_IRAM_BASE;
    constexpr std::uint64_t dram_base = T210_DRAM_BASE;


    static_assert(iram_base == 0x40000000);
    static_assert(dram_base == 0x80000000);
}