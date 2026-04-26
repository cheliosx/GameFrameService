#pragma once

#include "frame.hpp"
#include "player.hpp"

#include <cstdint>
#include <string>
#include <vector>

struct Room {
    std::string id;
    Frame current_frame;
    std::vector<Player> players;
    std::vector<Frame> received_messages;
};
