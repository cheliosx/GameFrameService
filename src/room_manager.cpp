#include "room_manager.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <unistd.h>
#include <sys/resource.h>
#include <boost/beast/core.hpp>
#include <boost/uuid/detail/md5.hpp>

#include "models/protocol.hpp"
#include "session.hpp"
#include "redis_client.hpp"

namespace beast = boost::beast;

namespace {
std::string md5_hex(const std::string& text) {
    boost::uuids::detail::md5 hash;
    boost::uuids::detail::md5::digest_type digest;
    hash.process_bytes(text.data(), text.size());
    hash.get_digest(digest);

    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&digest);
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < sizeof(digest); ++i) {
        oss << std::setw(2) << static_cast<int>(bytes[i]);
    }
    return oss.str();
}
}

RoomManager::RoomManager(boost::asio::io_context& io_context, std::uint32_t fps, std::shared_ptr<RedisClient> redis_client)
    : io_context_(io_context),
      frame_interval_ms_(std::max<std::uint32_t>(1, 1000 / std::max<std::uint32_t>(1, fps))),
      redis_client_(std::move(redis_client)) {}

void RoomManager::join(const std::string& room_id, const std::shared_ptr<Session>& session, int user_id) {
    auto& room = room_states_[room_id];
    room.id = room_id;
    if (room.current_frame.frame_id == 0) {
        room.current_frame.frame_id = 1;
    }
    room.players.push_back(Player{static_cast<std::uint64_t>(user_id), session});
}

bool RoomManager::verify_join_auth(const std::string& room_id, std::uint64_t timestamp_ms, const std::string& client_md5) {
    auto room_it = room_states_.find(room_id);
    if (room_it == room_states_.end()) {
        Room room;
        room.id = room_id;
        room.secret = redis_client_->get_room_secret(room_id);
        room.current_frame.frame_id = 1;
        room_states_[room_id] = std::move(room);
        room_it = room_states_.find(room_id);
    }

    const auto expected = md5_hex(std::to_string(timestamp_ms) + room_it->second.secret);
    return expected == client_md5;
}

void RoomManager::start_game(const std::string& room_id) {
    auto room_it = room_states_.find(room_id);
    if (room_it == room_states_.end() || room_it->second.game_started) {
        return;
    }
    room_it->second.game_started = true;
    if (room_timers_.find(room_id) == room_timers_.end()) {
        start_room_broadcast(room_id);
    }
}

void RoomManager::leave(const std::string& room_id, const std::shared_ptr<Session>& session, std::uint64_t player_id) {
    auto room_it = room_states_.find(room_id);
    if (room_it == room_states_.end()) {
        return;
    }

    auto& players = room_it->second.players;
    players.erase(std::remove_if(players.begin(), players.end(), [&](const Player& player) {
                      return player.user_id == player_id || player.session == session;
                  }),
                  players.end());

    if (players.empty()) {
        room_states_.erase(room_it);
        auto timer_it = room_timers_.find(room_id);
        if (timer_it != room_timers_.end()) {
            timer_it->second->cancel();
            room_timers_.erase(timer_it);
        }
    }
}

void RoomManager::broadcast_with_frame(const std::string& room_id,
                                       std::uint32_t message_id,
                                       ProtocolType protocol_type,
                                       const std::vector<std::uint8_t>& payload) {
    auto room_it = room_states_.find(room_id);
    if (room_it == room_states_.end()) {
        return;
    }

    const auto encoded = protocol::encode(message_id, protocol_type, payload);
    for (const auto& player : room_it->second.players) {
        if (player.session) {
            player.session->deliver(encoded);
        }
    }
}

void RoomManager::enqueue_operation(const std::string& room_id,
                                    std::uint32_t message_id,
                                    std::uint64_t user_id,
                                    InfoType info_type,
                                    const std::vector<std::uint8_t>& payload) {
    const auto room_it = room_states_.find(room_id);
    if (room_it == room_states_.end()) {
        return;
    }

    auto& operations = room_it->second.current_frame.operations;
    operations.erase(std::remove_if(operations.begin(), operations.end(), [info_type, user_id](const FrameOperation& op) {
                         return op.info_type == info_type && op.user_id == user_id;
                     }),
                     operations.end());
    operations.push_back(FrameOperation{message_id, user_id, info_type, payload});
}

std::vector<Frame> RoomManager::get_frames_after(const std::string& room_id,
                                                 std::uint32_t frame_id,
                                                 std::uint32_t count) const {
    std::vector<Frame> result;
    const auto room_it = room_states_.find(room_id);
    if (room_it == room_states_.end() || count == 0) {
        return result;
    }

    for (const auto& frame : room_it->second.received_messages) {
        if (frame.frame_id > frame_id) {
            result.push_back(frame);
            if (result.size() >= count) {
                break;
            }
        }
    }
    return result;
}

std::uint32_t RoomManager::get_current_frame_id(const std::string& room_id) const {
    const auto room_it = room_states_.find(room_id);
    if (room_it == room_states_.end()) {
        return 0;
    }
    return room_it->second.current_frame.frame_id - 1;
}

void RoomManager::start_room_broadcast(const std::string& room_id) {
    auto timer = std::make_shared<boost::asio::steady_timer>(io_context_);
    room_timers_[room_id] = timer;
    tick_room_broadcast(room_id);
}

void RoomManager::tick_room_broadcast(const std::string& room_id) {
    auto timer_it = room_timers_.find(room_id);
    if (timer_it == room_timers_.end()) {
        return;
    }

    auto timer = timer_it->second;
    timer->expires_after(std::chrono::milliseconds(frame_interval_ms_));
    timer->async_wait([this, room_id](const beast::error_code& ec) {
        if (ec) {
            return;
        }

        auto room_it = room_states_.find(room_id);
        if (room_it == room_states_.end() || !room_it->second.game_started) {
            return;
        }

        auto& room = room_it->second;
        const Frame completed_frame = room.current_frame;

        std::vector<Frame> frames;
        if (!completed_frame.operations.empty()) {
            frames.push_back(completed_frame);
        }
        const auto current_frame_id = room.current_frame.frame_id - 1;
        const auto encoded = protocol::encode_replay_response(0, frames, current_frame_id);

        for (const auto& player : room_it->second.players) {
            if (player.session) {
                player.session->deliver(encoded);
            }
        }

        if (!completed_frame.operations.empty()) {
            room.received_messages.push_back(completed_frame);
        }
        room.current_frame.frame_id += 1;
        room.current_frame.operations.clear();
        tick_room_broadcast(room_id);
    });
}

std::string RoomManager::server_info() const {
    std::size_t total_players = 0;
    for (const auto& [room_id, room] : room_states_) {
        (void)room_id;
        total_players += room.players.size();
    }

    struct rusage usage {};
    getrusage(RUSAGE_SELF, &usage);
    const double cpu_user_seconds = static_cast<double>(usage.ru_utime.tv_sec) +
                                    static_cast<double>(usage.ru_utime.tv_usec) / 1000000.0;
    const double cpu_system_seconds = static_cast<double>(usage.ru_stime.tv_sec) +
                                      static_cast<double>(usage.ru_stime.tv_usec) / 1000000.0;
    const long cpu_cores = sysconf(_SC_NPROCESSORS_ONLN);

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "服务器信息\n"
        << "- 当前玩家数量: " << total_players << "\n"
        << "- 当前房间数量: " << room_states_.size() << "\n"
        << "- CPU用户态时间: " << cpu_user_seconds << " s\n"
        << "- CPU内核态时间: " << cpu_system_seconds << " s\n"
        << "- 系统CPU核心数: " << cpu_cores;
    return oss.str();
}
