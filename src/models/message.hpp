#pragma once

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

enum class MessageType : std::uint16_t {
    SystemInfo = 0,
    Chat = 1,
    SetPosition = 2,
    FrameData = 3
};

struct Message {
    MessageType type = MessageType::Chat;
    std::string sent_at;
    std::string content;

    static std::string now() {
        const auto current_time = std::chrono::system_clock::now();
        const auto time_t = std::chrono::system_clock::to_time_t(current_time);
        std::tm local_time{};
#ifdef _WIN32
        localtime_s(&local_time, &time_t);
#else
        localtime_r(&time_t, &local_time);
#endif

        std::ostringstream oss;
        oss << std::put_time(&local_time, "%Y-%m-%d %H:%M:%S");
        return oss.str();
    }
};
