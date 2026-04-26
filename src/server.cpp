#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <sys/resource.h>
#include <unistd.h>

#include "models/message.hpp"
#include "models/frame.hpp"
#include "models/protocol.hpp"
#include "models/room.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

class Session;

class RoomManager {
public:
    explicit RoomManager(asio::io_context& io_context) : io_context_(io_context) {}

    void join(const std::string& room_id, const std::shared_ptr<Session>& session, int user_id);
    void leave(const std::string& room_id, const std::shared_ptr<Session>& session, std::uint64_t player_id);
    void broadcast_with_frame(const std::string& room_id,
                              std::uint32_t message_id,
                              ProtocolType protocol_type,
                              const std::vector<std::uint8_t>& payload);
    void enqueue_operation(const std::string& room_id,
                           std::uint32_t message_id,
                           std::uint64_t user_id,
                           InfoType info_type,
                           const std::vector<std::uint8_t>& payload);
    std::vector<Frame> get_frames_after(const std::string& room_id, std::uint32_t frame_id, std::uint32_t count) const;
    std::string server_info() const;

private:
    void start_room_broadcast(const std::string& room_id);
    void tick_room_broadcast(const std::string& room_id);

    asio::io_context& io_context_;
    std::unordered_map<std::string, std::unordered_set<std::shared_ptr<Session>>> sessions_by_room_;
    std::unordered_map<std::string, Room> room_states_;
    std::unordered_map<std::string, std::shared_ptr<asio::steady_timer>> room_timers_;
};

namespace {
double read_process_rss_mb() {
    std::ifstream status_file("/proc/self/status");
    std::string line;
    while (std::getline(status_file, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            std::istringstream iss(line);
            std::string key;
            double kb = 0.0;
            std::string unit;
            iss >> key >> kb >> unit;
            return kb / 1024.0;
        }
    }
    return 0.0;
}
} // namespace

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, std::shared_ptr<RoomManager> room_manager)
        : ws_(std::move(socket)), room_manager_(std::move(room_manager)), session_id_(next_session_id_++) {}

    void start() {
        ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));

        auto self = shared_from_this();
        ws_.async_accept([self](beast::error_code ec) {
            if (ec) {
                std::cerr << "WebSocket 握手失败: " << ec.message() << std::endl;
                return;
            }

            self->room_id_ = "1";
            self->joined_room_ = true;
            self->room_manager_->join(self->room_id_, self, self->session_id_);
            self->do_read();
        });
    }

    void deliver(const std::vector<std::uint8_t>& message) {
        auto self = shared_from_this();
        asio::post(ws_.get_executor(), [self, message] {
            bool writing = !self->outgoing_messages_.empty();
            self->outgoing_messages_.push_back(message);
            if (!writing) {
                self->do_write();
            }
        });
    }

private:
    void do_read() {
        auto self = shared_from_this();
        ws_.async_read(buffer_, [self](beast::error_code ec, std::size_t) {
            if (ec == websocket::error::closed) {
                self->leave_room();
                return;
            }

            if (ec) {
                std::cerr << "读取失败: " << ec.message() << std::endl;
                self->leave_room();
                return;
            }

            const auto data = self->buffer_.data();
            std::vector<std::uint8_t> bytes(asio::buffers_begin(data), asio::buffers_end(data));
            self->buffer_.consume(self->buffer_.size());

            if (bytes.size() < 6) {
                self->deliver(protocol::encode_chat(0, "消息长度不足，至少需要6字节"));
                self->do_read();
                return;
            }

            const std::uint32_t message_id = protocol::read_u32(bytes, 0);
            const auto protocol_type = static_cast<ProtocolType>(protocol::read_u16(bytes, 4));

            const std::vector<std::uint8_t> body(bytes.begin() + 6, bytes.end());
            if (protocol_type == ProtocolType::SystemInfo) {
                self->deliver(protocol::encode_system_info(message_id, self->room_manager_->server_info()));
            } else if (protocol_type == ProtocolType::SendInfo) {
                const auto [info_type, payload] = protocol::decode_send_info_body(body);
                self->room_manager_->enqueue_operation(
                    self->room_id_, message_id, self->session_id_, info_type, payload);
            } else if (protocol_type == ProtocolType::ReplayFrames) {
                const auto [start_frame_id, count] = protocol::decode_replay_request_body(body);
                const auto frames = self->room_manager_->get_frames_after(self->room_id_, start_frame_id, count);
                self->deliver(protocol::encode_replay_response(message_id, frames));
            }

            self->do_read();
        });
    }

    void do_write() {
        auto self = shared_from_this();
        ws_.binary(true);
        ws_.async_write(asio::buffer(outgoing_messages_.front()), [self](beast::error_code ec, std::size_t) {
            if (ec) {
                std::cerr << "写入失败: " << ec.message() << std::endl;
                self->leave_room();
                return;
            }

            self->outgoing_messages_.pop_front();
            if (!self->outgoing_messages_.empty()) {
                self->do_write();
            }
        });
    }

    void leave_room() {
        if (joined_room_) {
            room_manager_->leave(room_id_, shared_from_this(), session_id_);
            joined_room_ = false;
        }
    }

    websocket::stream<tcp::socket> ws_;
    beast::flat_buffer buffer_;
    std::deque<std::vector<std::uint8_t>> outgoing_messages_;
    std::shared_ptr<RoomManager> room_manager_;

    std::string room_id_;
    bool joined_room_ = false;
    int session_id_;

    inline static std::atomic<int> next_session_id_{1};
};

void RoomManager::join(const std::string& room_id, const std::shared_ptr<Session>& session, int user_id) {
    sessions_by_room_[room_id].insert(session);

    auto& room = room_states_[room_id];
    room.id = room_id;
    if (room.current_frame.frame_id == 0) {
        room.current_frame.frame_id = 1;
    }
    room.players.push_back(user_id);

    if (room_timers_.find(room_id) == room_timers_.end()) {
        start_room_broadcast(room_id);
    }
}

void RoomManager::leave(const std::string& room_id, const std::shared_ptr<Session>& session, std::uint64_t player_id) {
    auto session_it = sessions_by_room_.find(room_id);
    if (session_it == sessions_by_room_.end()) {
        return;
    }

    session_it->second.erase(session);

    auto room_it = room_states_.find(room_id);
    if (room_it != room_states_.end()) {
        auto& players = room_it->second.players;
        players.erase(std::remove_if(players.begin(), players.end(), [player_id](int user_id) {
                          return user_id == static_cast<int>(player_id);
                      }),
                      players.end());
    }

    if (session_it->second.empty()) {
        sessions_by_room_.erase(session_it);
        room_states_.erase(room_id);
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
    auto session_it = sessions_by_room_.find(room_id);
    if (session_it == sessions_by_room_.end()) {
        return;
    }

    const auto encoded = protocol::encode(message_id, protocol_type, payload);

    for (const auto& session : session_it->second) {
        session->deliver(encoded);
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
    operations.erase(std::remove_if(operations.begin(),
                                    operations.end(),
                                    [info_type, user_id](const FrameOperation& op) {
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

void RoomManager::start_room_broadcast(const std::string& room_id) {
    auto timer = std::make_shared<asio::steady_timer>(io_context_);
    room_timers_[room_id] = timer;
    tick_room_broadcast(room_id);
}

void RoomManager::tick_room_broadcast(const std::string& room_id) {
    auto timer_it = room_timers_.find(room_id);
    if (timer_it == room_timers_.end()) {
        return;
    }

    auto timer = timer_it->second;
    timer->expires_after(std::chrono::seconds(3));
    timer->async_wait([this, room_id](const beast::error_code& ec) {
        if (ec) {
            return;
        }

        auto room_it = room_states_.find(room_id);
        if (room_it == room_states_.end()) {
            return;
        }

        auto& room = room_it->second;
        const Frame completed_frame = room.current_frame;
        broadcast_with_frame(room_id, 0, ProtocolType::ReplayFrames, protocol::serialize_frames({completed_frame}));

        room.received_messages.push_back(completed_frame);
        room.current_frame.frame_id += 1;
        room.current_frame.operations.clear();
        tick_room_broadcast(room_id);
    });
}

std::string RoomManager::server_info() const {
    std::size_t total_players = 0;
    for (const auto& [room_id, sessions] : sessions_by_room_) {
        (void) room_id;
        total_players += sessions.size();
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
        << "- 当前房间数量: " << sessions_by_room_.size() << "\n"
        << "- 内存占用(RSS): " << read_process_rss_mb() << " MB\n"
        << "- CPU用户态时间: " << cpu_user_seconds << " s\n"
        << "- CPU内核态时间: " << cpu_system_seconds << " s\n"
        << "- 系统CPU核心数: " << cpu_cores;
    return oss.str();
}

class Server {
public:
    Server(asio::io_context& io_context, unsigned short port)
        : acceptor_(io_context, tcp::endpoint(tcp::v4(), port)), room_manager_(std::make_shared<RoomManager>(io_context)) {
        do_accept();
    }

private:
    void do_accept() {
        acceptor_.async_accept([this](beast::error_code ec, tcp::socket socket) {
            if (!ec) {
                std::make_shared<Session>(std::move(socket), room_manager_)->start();
            } else {
                std::cerr << "接受连接失败: " << ec.message() << std::endl;
            }
            do_accept();
        });
    }

    tcp::acceptor acceptor_;
    std::shared_ptr<RoomManager> room_manager_;
};

int main() {
    try {
        asio::io_context io_context;
        Server server(io_context, 8888);

        std::cout << "WebSocket 服务器启动在端口 8888..." << std::endl;
        io_context.run();
    } catch (const std::exception& e) {
        std::cerr << "异常: " << e.what() << std::endl;
    }

    return 0;
}
