#pragma once

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

enum class ProtocolType : std::uint16_t {
    SystemInfo = 1,
    SendInfo = 2,
    ReplayFrames = 3,
    JoinRoomChallenge = 4,
    JoinRoomAuth = 5,
    GameStart = 6
};

enum class InfoType : std::uint16_t {
    Chat = 1,
    Position = 2
};
