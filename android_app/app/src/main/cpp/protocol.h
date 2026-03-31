#pragma once
/**
 * Wire protocol definitions for quest_ros2_bridge.
 * Mirrors ros2_bridge/ros2_bridge/protocol.py — keep in sync.
 *
 * All multi-byte fields are big-endian (network byte order).
 * Pose: [x, y, z, qx, qy, qz, qw] — 7 × float32 = 28 bytes.
 */

#include <cstdint>
#include <cstring>  // memcpy
#include <arpa/inet.h>  // htonl, ntohl

namespace protocol {

// Message types
enum class MsgType : uint8_t {
    CONTROLLER_STATE = 0x01,
    HAPTIC_CMD       = 0x02,
    CAMERA_FRAME     = 0x03,
    HAND_STATE       = 0x04,
};

// Size constants
constexpr size_t HEADER_SIZE           = 5;   // 1B type + 4B uint32 length
constexpr size_t POSE_SIZE             = 28;  // 7 × float32
constexpr size_t CONTROLLER_BLOCK_SIZE = 32;  // 4B buttons/triggers + 28B pose
constexpr size_t CONTROLLER_STATE_SIZE = 64;  // 2 × 32B (left + right)
constexpr size_t HAND_JOINT_COUNT      = 26;
constexpr size_t HAND_STATE_SIZE       = 729; // 1B hand + 26 × 28B poses
constexpr size_t HAPTIC_CMD_SIZE       = 9;   // 1B hand + 4B intensity + 4B duration

// ---------- Byte-order helpers (safe, no UB) ----------

/** Write a float32 in big-endian to buf. */
inline void float_to_be(float val, uint8_t* buf) {
    uint32_t tmp;
    std::memcpy(&tmp, &val, 4);
    tmp = htonl(tmp);
    std::memcpy(buf, &tmp, 4);
}

/** Read a big-endian float32 from buf. */
inline float be_to_float(const uint8_t* buf) {
    uint32_t tmp;
    std::memcpy(&tmp, buf, 4);
    tmp = ntohl(tmp);
    float val;
    std::memcpy(&val, &tmp, 4);
    return val;
}

/** Write the 5-byte message header. */
inline void write_header(uint8_t* buf, MsgType type, uint32_t payload_len) {
    buf[0] = static_cast<uint8_t>(type);
    uint32_t len_be = htonl(payload_len);
    std::memcpy(buf + 1, &len_be, 4);
}

/** Read the 5-byte message header. */
inline void read_header(const uint8_t* buf, MsgType& type, uint32_t& payload_len) {
    type = static_cast<MsgType>(buf[0]);
    uint32_t len_be;
    std::memcpy(&len_be, buf + 1, 4);
    payload_len = ntohl(len_be);
}

}  // namespace protocol
