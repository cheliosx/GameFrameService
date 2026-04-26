#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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

struct FrameOperation {
    std::uint32_t message_id = 0;
    std::uint64_t user_id = 0;
    InfoType info_type = InfoType::Chat;
    std::vector<std::uint8_t> payload;
};

struct Frame {
    std::uint32_t frame_id = 1;
    std::vector<FrameOperation> operations;
};

class Session;

struct Player {
    std::uint64_t user_id = 0;
    std::shared_ptr<Session> session;
};

struct Room {
    std::string id;
    std::string secret;
    bool game_started = false;
    Frame current_frame;
    std::vector<Player> players;
    std::vector<Frame> received_messages;
};
