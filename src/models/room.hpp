#pragma once

#include "message.hpp"
#include "player.hpp"

#include <cstdint>
#include <string>
#include <vector>

struct Room {
    std::string id;
    std::uint32_t frame_id = 1;
    std::vector<Player> players;
    std::vector<Message> received_messages;
};
