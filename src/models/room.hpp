#pragma once

#include "frame.hpp"

#include <cstdint>
#include <string>
#include <vector>

struct Room {
    std::string id;
    std::string secret;
    bool game_started = false;
    Frame current_frame;
    std::vector<int> players;
    std::vector<Frame> received_messages;
};
