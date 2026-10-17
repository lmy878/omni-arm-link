#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace oal {

constexpr std::uint32_t kMagic = 0x314C414FU;
constexpr std::uint8_t kProtocolVersion = 1U;

constexpr std::size_t kQuaternionCount = 5;
constexpr std::size_t kLaserCount = 15;
constexpr std::size_t kStateCount = 50;
constexpr std::size_t kAxisCount = 4;
constexpr std::size_t kParamCount = 4;
constexpr std::size_t kTelemetryPayloadBytes =
    (kQuaternionCount * 4 + kLaserCount + kStateCount) * sizeof(float);

enum class Stream : std::uint8_t {
    Telemetry = 0x01,
    Debug = 0x02,
    TestDown = 0x7E,
    Control = 0x80,
    TestUp = 0xFE,
};

constexpr std::uint8_t stream_id(Stream s) { return static_cast<std::uint8_t>(s); }

enum FrameFlags : std::uint16_t {
    kFlagNone = 0x0000,
    kFlagEstop = 0x0001,
    kFlagHeartbeat = 0x0002,
    kFlagAck = 0x0004,
    kFlagCritical = 0x0008,
};

enum class ControlMode : std::uint8_t {
    Idle = 0,
    Manual = 1,
    Autonomous = 2,
    Calibration = 3,
};

enum ButtonBits : std::uint16_t {
    kButtonArm = 1U << 0,
    kButtonDisarm = 1U << 1,
    kButtonEstop = 1U << 2,
    kButtonMode = 1U << 3,
    kButtonCalibrate = 1U << 4,
};

enum class DebugLevel : std::uint8_t {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
};

struct FrameHeader {
    std::uint32_t magic = kMagic;
    std::uint8_t version = kProtocolVersion;
    std::uint8_t stream = 0;
    std::uint16_t flags = kFlagNone;
    std::uint32_t seq = 0;
    std::uint64_t timestamp_ns = 0;
    std::uint16_t payload_len = 0;
    std::uint16_t reserved = 0;
    std::uint32_t payload_crc = 0;
};

constexpr std::size_t kHeaderSize = 28;

struct ControlPayload {
    std::array<float, kAxisCount> axes{};
    std::uint16_t buttons = 0;
    std::uint8_t mode = static_cast<std::uint8_t>(ControlMode::Idle);
    std::uint8_t flags = 0;
    std::array<float, kParamCount> params{};
};

constexpr std::size_t kControlPayloadBytes =
    kAxisCount * sizeof(float) + sizeof(std::uint16_t) + 2 * sizeof(std::uint8_t) +
    kParamCount * sizeof(float);

struct TelemetryPayload {
    std::array<std::array<float, 4>, kQuaternionCount> quaternions{};
    std::array<float, kLaserCount> lasers{};
    std::array<float, kStateCount> state{};
};

constexpr std::size_t kDebugMaxBytes = 256;
constexpr std::size_t kDebugHeaderBytes = 2;

struct DebugPayload {
    DebugLevel level = DebugLevel::Info;
    std::uint8_t length = 0;
    std::array<char, kDebugMaxBytes> text{};
};

}  // namespace oal
