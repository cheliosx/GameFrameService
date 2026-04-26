#pragma once

#include "message.hpp"
#include "player.hpp"

#include <string>
#include <vector>

struct Room {
    std::string id;
    std::vector<Player> players;
    std::vector<Message> received_messages;
};
