#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/uuid/detail/md5.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <sys/resource.h>
#include <unistd.h>

#include "models/frame.hpp"
#include "models/message.hpp"
#include "models/protocol.hpp"
#include "models/room.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

struct ServerConfig {
    unsigned short port = 8888;
    std::uint32_t fps = 1;
    std::string redis_host = "127.0.0.1";
    unsigned short redis_port = 6379;
    std::string redis_password;
};

class RedisClient {
public:
    RedisClient(std::string host, unsigned short port, std::string password)
        : host_(std::move(host)), port_(port), password_(std::move(password)) {}

    std::string get_room_secret(const std::string& room_id) {
        asio::io_context io;
        tcp::resolver resolver(io);
        tcp::socket socket(io);
        asio::connect(socket, resolver.resolve(host_, std::to_string(port_)));

        if (!password_.empty()) {
            write_command(socket, {"AUTH", password_});
            expect_simple_ok(socket);
        }

        write_command(socket, {"GET", "room:secret:" + room_id});
        return read_bulk_string(socket);
    }

private:
    static void write_command(tcp::socket& socket, const std::vector<std::string>& args) {
        std::ostringstream cmd;
        cmd << '*' << args.size() << "\r\n";
        for (const auto& arg : args) {
            cmd << '$' << arg.size() << "\r\n" << arg << "\r\n";
        }
        const auto text = cmd.str();
        asio::write(socket, asio::buffer(text));
    }

    static std::string read_line(tcp::socket& socket) {
        asio::streambuf buf;
        asio::read_until(socket, buf, "\r\n");
        std::istream is(&buf);
        std::string line;
        std::getline(is, line);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        return line;
    }

    static void expect_simple_ok(tcp::socket& socket) {
        const auto line = read_line(socket);
        if (line.empty() || line[0] != '+') {
            throw std::runtime_error("Redis AUTH返回异常: " + line);
        }
    }

    static std::string read_bulk_string(tcp::socket& socket) {
        const auto header = read_line(socket);
        if (header.empty() || header[0] != '$') {
            throw std::runtime_error("Redis GET返回异常: " + header);
        }

        const int len = std::stoi(header.substr(1));
        if (len < 0) {
            throw std::runtime_error("房间密钥不存在");
        }

        std::vector<char> data(static_cast<std::size_t>(len + 2));
        asio::read(socket, asio::buffer(data));
        return std::string(data.begin(), data.begin() + len);
    }

    std::string host_;
    unsigned short port_;
    std::string password_;
};

class Session;

class RoomManager {
public:
    RoomManager(asio::io_context& io_context, std::uint32_t fps)
        : io_context_(io_context), frame_interval_ms_(std::max<std::uint32_t>(1, 1000 / std::max<std::uint32_t>(1, fps))) {}

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
    std::uint32_t frame_interval_ms_;
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
    std::string out = oss.str();
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::uint64_t now_timestamp_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

ServerConfig parse_args(int argc, char** argv) {
    ServerConfig cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next_value = [&](const std::string& name) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error("参数缺少值: " + name);
            }
            return argv[++i];
        };

        if (arg == "--port") {
            cfg.port = static_cast<unsigned short>(std::stoul(next_value(arg)));
        } else if (arg == "--fps") {
            cfg.fps = static_cast<std::uint32_t>(std::stoul(next_value(arg)));
        } else if (arg == "--redis-host") {
            cfg.redis_host = next_value(arg);
        } else if (arg == "--redis-port") {
            cfg.redis_port = static_cast<unsigned short>(std::stoul(next_value(arg)));
        } else if (arg == "--redis-password") {
            cfg.redis_password = next_value(arg);
        }
    }
    return cfg;
}

} // namespace

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, std::shared_ptr<RoomManager> room_manager, std::shared_ptr<RedisClient> redis_client)
        : ws_(std::move(socket)),
          room_manager_(std::move(room_manager)),
          redis_client_(std::move(redis_client)),
          session_id_(next_session_id_++) {}

    void start() {
        ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));

        auto self = shared_from_this();
        ws_.async_accept([self](beast::error_code ec) {
            if (ec) {
                std::cerr << "WebSocket 握手失败: " << ec.message() << std::endl;
                return;
            }

            self->challenge_ts_ms_ = now_timestamp_ms();
            self->deliver(protocol::encode_join_room_challenge(0, self->challenge_ts_ms_));
            self->do_read();
        });
    }

    void deliver(const std::vector<std::uint8_t>& message) {
        auto self = shared_from_this();
        asio::post(ws_.get_executor(), [self, message] {
            const bool writing = !self->outgoing_messages_.empty();
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

            try {
                if (protocol_type == ProtocolType::JoinRoomAuth) {
                    self->handle_join_auth(message_id, body);
                } else if (!self->joined_room_) {
                    self->deliver(protocol::encode_chat(message_id, "请先发送入房鉴权协议"));
                } else if (protocol_type == ProtocolType::SystemInfo) {
                    self->deliver(protocol::encode_system_info(message_id, self->room_manager_->server_info()));
                } else if (protocol_type == ProtocolType::SendInfo) {
                    const auto [info_type, payload] = protocol::decode_send_info_body(body);
                    self->room_manager_->enqueue_operation(self->room_id_, message_id, self->session_id_, info_type, payload);
                } else if (protocol_type == ProtocolType::ReplayFrames) {
                    const auto [start_frame_id, count] = protocol::decode_replay_request_body(body);
                    const auto frames = self->room_manager_->get_frames_after(self->room_id_, start_frame_id, count);
                    self->deliver(protocol::encode_replay_response(message_id, frames));
                }
            } catch (const std::exception& e) {
                self->deliver(protocol::encode_chat(message_id, std::string("协议处理失败: ") + e.what()));
            }

            self->do_read();
        });
    }

    void handle_join_auth(std::uint32_t message_id, const std::vector<std::uint8_t>& body) {
        const auto [room_id, client_md5] = protocol::decode_join_room_auth_body(body);
        const auto secret = redis_client_->get_room_secret(room_id);
        const auto expected = md5_hex(std::to_string(challenge_ts_ms_) + secret);

        if (expected != client_md5) {
            deliver(protocol::encode_chat(message_id, "入房鉴权失败，md5不匹配"));
            return;
        }

        room_id_ = room_id;
        joined_room_ = true;
        room_manager_->join(room_id_, shared_from_this(), session_id_);
        deliver(protocol::encode_system_info(message_id, "入房成功，room_id=" + room_id_));
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
    std::shared_ptr<RedisClient> redis_client_;

    std::string room_id_;
    bool joined_room_ = false;
    std::uint64_t challenge_ts_ms_ = 0;
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
    timer->expires_after(std::chrono::milliseconds(frame_interval_ms_));
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
        (void)room_id;
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
    Server(asio::io_context& io_context, const ServerConfig& cfg)
        : acceptor_(io_context, tcp::endpoint(tcp::v4(), cfg.port)),
          room_manager_(std::make_shared<RoomManager>(io_context, cfg.fps)),
          redis_client_(std::make_shared<RedisClient>(cfg.redis_host, cfg.redis_port, cfg.redis_password)) {
        do_accept();
    }

private:
    void do_accept() {
        acceptor_.async_accept([this](beast::error_code ec, tcp::socket socket) {
            if (!ec) {
                std::make_shared<Session>(std::move(socket), room_manager_, redis_client_)->start();
            } else {
                std::cerr << "接受连接失败: " << ec.message() << std::endl;
            }
            do_accept();
        });
    }

    tcp::acceptor acceptor_;
    std::shared_ptr<RoomManager> room_manager_;
    std::shared_ptr<RedisClient> redis_client_;
};

int main(int argc, char** argv) {
    try {
        const auto cfg = parse_args(argc, argv);

        asio::io_context io_context;
        Server server(io_context, cfg);

        std::cout << "WebSocket 服务器启动: port=" << cfg.port << ", fps=" << cfg.fps
                  << ", redis=" << cfg.redis_host << ':' << cfg.redis_port << std::endl;
        io_context.run();
    } catch (const std::exception& e) {
        std::cerr << "异常: " << e.what() << std::endl;
    }

    return 0;
}
