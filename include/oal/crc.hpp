#pragma once

#include <cstddef>
#include <cstdint>

namespace oal {

class Crc32 {
public:
    void update(const void* data, std::size_t length);
    std::uint32_t value() const { return state_ ^ 0xFFFFFFFFU; }
    void reset() { state_ = 0xFFFFFFFFU; }

private:
    std::uint32_t state_ = 0xFFFFFFFFU;
};

std::uint32_t crc32(const void* data, std::size_t length);

}  // namespace oal
