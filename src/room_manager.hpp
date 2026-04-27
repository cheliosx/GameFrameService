#pragma once

#include <boost/asio.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "models/game_models.hpp"

class RedisClient;
class Session;

class RoomManager {
public:
    RoomManager(boost::asio::io_context& io_context, std::uint32_t fps, std::shared_ptr<RedisClient> redis_client);

    void join(const std::string& room_id, const std::shared_ptr<Session>& session, int user_id);
    void leave(const std::string& room_id, const std::shared_ptr<Session>& session, std::uint64_t player_id);
    bool verify_join_auth(const std::string& room_id, std::uint64_t timestamp_ms, const std::string& client_md5);
    void start_game(const std::string& room_id);
    void enqueue_operation(const std::string& room_id,
                           std::uint32_t message_id,
                           std::uint64_t user_id,
                           InfoType info_type,
                           const std::vector<std::uint8_t>& payload);
    std::vector<Frame> get_frames_after(const std::string& room_id, std::uint32_t frame_id, std::uint32_t count) const;
    std::uint32_t get_current_frame_id(const std::string& room_id) const;
    std::string server_info() const;

private:
    void start_room_broadcast(const std::string& room_id);
    void tick_room_broadcast(const std::string& room_id);
    void broadcast_with_frame(const std::string& room_id,
                              std::uint32_t message_id,
                              ProtocolType protocol_type,
                              const std::vector<std::uint8_t>& payload);

    boost::asio::io_context& io_context_;
    std::uint32_t frame_interval_ms_;
    std::unordered_map<std::string, Room> room_states_;
    std::unordered_map<std::string, std::shared_ptr<boost::asio::steady_timer>> room_timers_;
    std::shared_ptr<RedisClient> redis_client_;
};
