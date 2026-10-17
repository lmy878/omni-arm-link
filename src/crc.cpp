#include "oal/crc.hpp"

namespace oal {
namespace {

struct Crc32Table {
    std::uint32_t entries[256];

    constexpr Crc32Table() : entries{} {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1U) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
            }
            entries[i] = c;
        }
    }
};

constexpr Crc32Table kTable{};

}  // namespace

void Crc32::update(const void* data, std::size_t length) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::uint32_t c = state_;
    for (std::size_t i = 0; i < length; ++i) {
        c = kTable.entries[(c ^ p[i]) & 0xFFU] ^ (c >> 8);
    }
    state_ = c;
}

std::uint32_t crc32(const void* data, std::size_t length) {
    Crc32 crc;
    crc.update(data, length);
    return crc.value();
}

}  // namespace oal
