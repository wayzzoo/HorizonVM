#include "devices/mmio.h"
#include "platform/tegra210.h"


#include <cstdint>


namespace hvm::devices {


class MmioDispatcher {
    public:
        static bool read32(
            hvm_mmio_t& mmio,
            std::uint64_t address,
            std::uint32_t& value
        ) {
            if (address >= T210_TIMER_BASE &&
                address - T210_TIMER_BASE < T210_TIMER_SIZE) {
                return hvm_timer_read(
                    &mmio.timer,
                    address,
                    &value
                );
            }
            
            return false;
        }
};

}

extern "C" bool hvm_mmio_init(hvm_mmio_t *mmio) {
    if (mmio == nullptr) {
        return false;
    }

    return hvm_timer_init(&mmio->timer);
}

extern "C" bool hvm_mmio_read32(
    hvm_mmio_t *mmio,
    uint64_t address,
    uint32_t *value
) {
    if (mmio == nullptr || value == nullptr) {
        return false;
    }

    return hvm::devices::MmioDispatcher::read32(
        *mmio,
        address,
        *value
    );
}