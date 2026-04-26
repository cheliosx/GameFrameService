#pragma once

#include "message.hpp"

#include <cstdint>
#include <vector>

struct FrameOperation {
    std::uint32_t message_id = 0;
    MessageType message_type = MessageType::Chat;
    std::vector<std::uint8_t> payload;
};

struct Frame {
    std::uint32_t frame_id = 1;
    std::vector<FrameOperation> operations;
};
