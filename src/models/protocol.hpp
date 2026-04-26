#pragma once

#include "frame.hpp"
#include "message.hpp"

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace protocol {

struct DecodedMessage {
    std::uint32_t message_id = 0;
    MessageType message_type = MessageType::Chat;
    std::vector<std::uint8_t> body;
};

inline void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

inline void append_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

inline void append_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
}

inline std::uint32_t read_u32(const std::vector<std::uint8_t>& data, std::size_t offset) {
    return (static_cast<std::uint32_t>(data[offset]) << 24) |
           (static_cast<std::uint32_t>(data[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(data[offset + 2]) << 8) |
           static_cast<std::uint32_t>(data[offset + 3]);
}

inline std::uint16_t read_u16(const std::vector<std::uint8_t>& data, std::size_t offset) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[offset]) << 8) |
                                      static_cast<std::uint16_t>(data[offset + 1]));
}

inline std::uint64_t read_u64(const std::vector<std::uint8_t>& data, std::size_t offset) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<std::uint64_t>(data[offset + static_cast<std::size_t>(i)]);
    }
    return value;
}

inline void append_float(std::vector<std::uint8_t>& out, float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(float));
    append_u32(out, bits);
}

inline float read_float(const std::vector<std::uint8_t>& data, std::size_t offset) {
    const std::uint32_t bits = read_u32(data, offset);
    float value = 0.0F;
    std::memcpy(&value, &bits, sizeof(float));
    return value;
}

inline DecodedMessage decode(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 6) {
        throw std::runtime_error("消息长度不足，至少需要6字节头部");
    }

    DecodedMessage decoded;
    decoded.message_id = read_u32(bytes, 0);
    decoded.message_type = static_cast<MessageType>(read_u16(bytes, 4));
    decoded.body.assign(bytes.begin() + 6, bytes.end());
    return decoded;
}

inline std::vector<std::uint8_t> encode(std::uint32_t message_id,
                                        MessageType type,
                                        const std::vector<std::uint8_t>& body) {
    std::vector<std::uint8_t> result;
    result.reserve(6 + body.size());
    append_u32(result, message_id);
    append_u16(result, static_cast<std::uint16_t>(type));
    result.insert(result.end(), body.begin(), body.end());
    return result;
}

inline std::vector<std::uint8_t> encode_chat(std::uint32_t message_id, const std::string& text) {
    return encode(message_id, MessageType::Chat, std::vector<std::uint8_t>(text.begin(), text.end()));
}

inline std::vector<std::uint8_t> encode_position(std::uint32_t message_id, float x, float y) {
    std::vector<std::uint8_t> body;
    body.reserve(8);
    append_float(body, x);
    append_float(body, y);
    return encode(message_id, MessageType::SetPosition, body);
}

inline std::string decode_chat_body(const std::vector<std::uint8_t>& body) {
    return std::string(body.begin(), body.end());
}

inline std::pair<float, float> decode_position_body(const std::vector<std::uint8_t>& body) {
    if (body.size() != 8) {
        throw std::runtime_error("位置消息体长度必须为8字节");
    }
    return {read_float(body, 0), read_float(body, 4)};
}

inline std::vector<std::uint8_t> serialize_frame(const Frame& frame) {
    std::vector<std::uint8_t> out;
    out.reserve(16);
    append_u32(out, frame.frame_id);
    append_u32(out, static_cast<std::uint32_t>(frame.operations.size()));
    for (const auto& op : frame.operations) {
        append_u32(out, op.message_id);
        append_u64(out, op.user_id);
        append_u16(out, static_cast<std::uint16_t>(op.message_type));
        append_u32(out, static_cast<std::uint32_t>(op.payload.size()));
        out.insert(out.end(), op.payload.begin(), op.payload.end());
    }
    return out;
}

inline Frame deserialize_frame(const std::vector<std::uint8_t>& body) {
    if (body.size() < 8) {
        throw std::runtime_error("帧数据长度不足");
    }

    std::size_t offset = 0;
    Frame frame;
    frame.frame_id = read_u32(body, offset);
    offset += 4;
    const std::uint32_t op_count = read_u32(body, offset);
    offset += 4;

    frame.operations.reserve(op_count);
    for (std::uint32_t i = 0; i < op_count; ++i) {
        if (offset + 18 > body.size()) {
            throw std::runtime_error("帧操作头长度不足");
        }

        FrameOperation op;
        op.message_id = read_u32(body, offset);
        offset += 4;
        op.user_id = read_u64(body, offset);
        offset += 8;
        op.message_type = static_cast<MessageType>(read_u16(body, offset));
        offset += 2;
        const std::uint32_t payload_len = read_u32(body, offset);
        offset += 4;
        if (offset + payload_len > body.size()) {
            throw std::runtime_error("帧操作payload长度非法");
        }
        op.payload.assign(body.begin() + static_cast<std::ptrdiff_t>(offset),
                          body.begin() + static_cast<std::ptrdiff_t>(offset + payload_len));
        offset += payload_len;
        frame.operations.push_back(std::move(op));
    }

    return frame;
}

} // namespace protocol
