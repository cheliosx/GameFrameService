#pragma once

#include "frame.hpp"
#include "message.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace protocol {

struct DecodedMessage {
    std::uint32_t message_id = 0;
    ProtocolType protocol_type = ProtocolType::SendInfo;
    std::vector<std::uint8_t> body;
};

inline void append_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

inline void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

inline void append_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
}

inline std::uint16_t read_u16(const std::vector<std::uint8_t>& data, std::size_t offset) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[offset]) << 8) |
                                      static_cast<std::uint16_t>(data[offset + 1]));
}

inline std::uint32_t read_u32(const std::vector<std::uint8_t>& data, std::size_t offset) {
    return (static_cast<std::uint32_t>(data[offset]) << 24) |
           (static_cast<std::uint32_t>(data[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(data[offset + 2]) << 8) |
           static_cast<std::uint32_t>(data[offset + 3]);
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
    decoded.protocol_type = static_cast<ProtocolType>(read_u16(bytes, 4));
    decoded.body.assign(bytes.begin() + 6, bytes.end());
    return decoded;
}

inline std::vector<std::uint8_t> encode(std::uint32_t message_id,
                                        ProtocolType protocol_type,
                                        const std::vector<std::uint8_t>& body) {
    std::vector<std::uint8_t> result;
    result.reserve(6 + body.size());
    append_u32(result, message_id);
    append_u16(result, static_cast<std::uint16_t>(protocol_type));
    result.insert(result.end(), body.begin(), body.end());
    return result;
}

inline std::vector<std::uint8_t> encode_system_info(std::uint32_t message_id, const std::string& text) {
    return encode(message_id, ProtocolType::SystemInfo, std::vector<std::uint8_t>(text.begin(), text.end()));
}

inline std::string decode_system_info_body(const std::vector<std::uint8_t>& body) {
    return std::string(body.begin(), body.end());
}

inline std::vector<std::uint8_t> encode_send_info(std::uint32_t message_id,
                                                  InfoType info_type,
                                                  const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> body;
    body.reserve(2 + payload.size());
    append_u16(body, static_cast<std::uint16_t>(info_type));
    body.insert(body.end(), payload.begin(), payload.end());
    return encode(message_id, ProtocolType::SendInfo, body);
}

inline std::vector<std::uint8_t> encode_chat(std::uint32_t message_id, const std::string& text) {
    return encode_send_info(message_id, InfoType::Chat, std::vector<std::uint8_t>(text.begin(), text.end()));
}

inline std::vector<std::uint8_t> encode_position(std::uint32_t message_id, float x, float y) {
    std::vector<std::uint8_t> payload;
    payload.reserve(8);
    append_float(payload, x);
    append_float(payload, y);
    return encode_send_info(message_id, InfoType::Position, payload);
}

inline std::pair<InfoType, std::vector<std::uint8_t>> decode_send_info_body(const std::vector<std::uint8_t>& body) {
    if (body.size() < 2) {
        throw std::runtime_error("SendInfo消息体至少需要2字节子类型");
    }
    const auto info_type = static_cast<InfoType>(read_u16(body, 0));
    return {info_type, std::vector<std::uint8_t>(body.begin() + 2, body.end())};
}

inline std::string decode_chat_payload(const std::vector<std::uint8_t>& payload) {
    return std::string(payload.begin(), payload.end());
}

inline std::pair<float, float> decode_position_payload(const std::vector<std::uint8_t>& payload) {
    if (payload.size() != 8) {
        throw std::runtime_error("坐标payload长度必须为8字节");
    }
    return {read_float(payload, 0), read_float(payload, 4)};
}

inline std::vector<std::uint8_t> serialize_frame(const Frame& frame) {
    std::vector<std::uint8_t> out;
    append_u32(out, frame.frame_id);
    append_u32(out, static_cast<std::uint32_t>(frame.operations.size()));
    for (const auto& op : frame.operations) {
        append_u32(out, op.message_id);
        append_u64(out, op.user_id);
        append_u16(out, static_cast<std::uint16_t>(op.info_type));
        append_u32(out, static_cast<std::uint32_t>(op.payload.size()));
        out.insert(out.end(), op.payload.begin(), op.payload.end());
    }
    return out;
}

inline Frame deserialize_frame(const std::vector<std::uint8_t>& body) {
    if (body.size() < 8) {
        throw std::runtime_error("帧数据长度不足");
    }

    Frame frame;
    std::size_t offset = 0;
    frame.frame_id = read_u32(body, offset);
    offset += 4;
    const auto op_count = read_u32(body, offset);
    offset += 4;

    for (std::uint32_t i = 0; i < op_count; ++i) {
        if (offset + 18 > body.size()) {
            throw std::runtime_error("帧操作头长度不足");
        }
        FrameOperation op;
        op.message_id = read_u32(body, offset);
        offset += 4;
        op.user_id = read_u64(body, offset);
        offset += 8;
        op.info_type = static_cast<InfoType>(read_u16(body, offset));
        offset += 2;
        const auto payload_len = read_u32(body, offset);
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

inline std::vector<std::uint8_t> serialize_frames(const std::vector<Frame>& frames) {
    std::vector<std::uint8_t> out;
    append_u32(out, static_cast<std::uint32_t>(frames.size()));
    for (const auto& frame : frames) {
        const auto one = serialize_frame(frame);
        append_u32(out, static_cast<std::uint32_t>(one.size()));
        out.insert(out.end(), one.begin(), one.end());
    }
    return out;
}

inline std::vector<std::uint8_t> serialize_frames_for_broadcast(const std::vector<Frame>& frames) {
    return serialize_frames(frames);
}

inline std::vector<Frame> deserialize_frames(const std::vector<std::uint8_t>& body) {
    if (body.empty()) {
        return {};
    }
    if (body.size() < 4) {
        throw std::runtime_error("补帧响应长度不足");
    }
    std::size_t offset = 0;
    const auto count = read_u32(body, offset);
    offset += 4;
    std::vector<Frame> frames;
    frames.reserve(count);

    for (std::uint32_t i = 0; i < count; ++i) {
        if (offset + 4 > body.size()) {
            throw std::runtime_error("补帧数据长度非法");
        }
        const auto frame_size = read_u32(body, offset);
        offset += 4;
        if (offset + frame_size > body.size()) {
            throw std::runtime_error("补帧Frame长度非法");
        }
        std::vector<std::uint8_t> one(body.begin() + static_cast<std::ptrdiff_t>(offset),
                                      body.begin() + static_cast<std::ptrdiff_t>(offset + frame_size));
        offset += frame_size;
        frames.push_back(deserialize_frame(one));
    }
    return frames;
}

inline std::vector<std::uint8_t> encode_replay_request(std::uint32_t message_id,
                                                       std::uint32_t frame_id,
                                                       std::uint32_t count) {
    std::vector<std::uint8_t> body;
    append_u32(body, frame_id);
    append_u32(body, count);
    return encode(message_id, ProtocolType::ReplayFrames, body);
}

inline std::pair<std::uint32_t, std::uint32_t> decode_replay_request_body(const std::vector<std::uint8_t>& body) {
    if (body.size() != 8) {
        throw std::runtime_error("补帧请求体必须为8字节(frame_id + count)");
    }
    return {read_u32(body, 0), read_u32(body, 4)};
}

inline std::vector<std::uint8_t> encode_replay_response(std::uint32_t message_id, const std::vector<Frame>& frames, std::uint32_t current_frame_id) {
    std::vector<std::uint8_t> body;
    append_u32(body, current_frame_id);
    const auto frames_data = serialize_frames(frames);
    body.insert(body.end(), frames_data.begin(), frames_data.end());
    return encode(message_id, ProtocolType::ReplayFrames, body);
}

inline std::vector<std::uint8_t> encode_join_room_challenge(std::uint32_t message_id, std::uint64_t timestamp_ms) {
    std::vector<std::uint8_t> body;
    append_u64(body, timestamp_ms);
    return encode(message_id, ProtocolType::JoinRoomChallenge, body);
}

inline std::uint64_t decode_join_room_challenge_body(const std::vector<std::uint8_t>& body) {
    if (body.size() != 8) {
        throw std::runtime_error("入房挑战消息体必须为8字节时间戳");
    }
    return read_u64(body, 0);
}

inline std::vector<std::uint8_t> encode_join_room_auth(std::uint32_t message_id,
                                                       const std::string& room_id,
                                                       const std::string& md5) {
    std::vector<std::uint8_t> body;
    append_u16(body, static_cast<std::uint16_t>(room_id.size()));
    body.insert(body.end(), room_id.begin(), room_id.end());
    append_u16(body, static_cast<std::uint16_t>(md5.size()));
    body.insert(body.end(), md5.begin(), md5.end());
    return encode(message_id, ProtocolType::JoinRoomAuth, body);
}

inline std::pair<std::string, std::string> decode_join_room_auth_body(const std::vector<std::uint8_t>& body) {
    if (body.size() < 4) {
        throw std::runtime_error("入房鉴权消息体长度不足");
    }

    std::size_t offset = 0;
    const auto room_id_len = read_u16(body, offset);
    offset += 2;
    if (offset + room_id_len + 2 > body.size()) {
        throw std::runtime_error("入房鉴权room_id长度非法");
    }
    std::string room_id(body.begin() + static_cast<std::ptrdiff_t>(offset),
                        body.begin() + static_cast<std::ptrdiff_t>(offset + room_id_len));
    offset += room_id_len;

    const auto md5_len = read_u16(body, offset);
    offset += 2;
    if (offset + md5_len != body.size()) {
        throw std::runtime_error("入房鉴权md5长度非法");
    }
    std::string md5(body.begin() + static_cast<std::ptrdiff_t>(offset),
                    body.begin() + static_cast<std::ptrdiff_t>(offset + md5_len));
    return {room_id, md5};
}

inline std::vector<std::uint8_t> encode_game_start(std::uint32_t message_id) {
    return encode(message_id, ProtocolType::GameStart, {});
}

} // namespace protocol
