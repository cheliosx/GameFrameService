#pragma once

#include <boost/asio.hpp>

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "models/game_models.hpp"

class RedisClient;
class Session;

class RoomManager {
public:
    RoomManager(std::uint32_t fps, std::shared_ptr<RedisClient> redis_client, std::size_t num_shards = 4);
    ~RoomManager();

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
    void start_broadcast_shard(std::size_t shard_id);
    void tick_broadcast_shard(std::size_t shard_id);
    std::size_t get_shard_id(const std::string& room_id) const;

    std::uint32_t frame_interval_ms_;
    std::size_t num_shards_;
    std::unordered_map<std::string, Room> room_states_;
    mutable std::shared_mutex room_states_mutex_;

    std::vector<std::vector<std::string>> shard_room_ids_;
    std::vector<std::shared_ptr<boost::asio::io_context>> shard_io_contexts_;
    std::vector<std::shared_ptr<boost::asio::steady_timer>> shard_timers_;
    std::vector<std::thread> shard_threads_;

    std::shared_ptr<RedisClient> redis_client_;
};
