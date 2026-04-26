#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <algorithm>
#include <atomic>
#include <deque>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "models/message.hpp"
#include "models/player.hpp"
#include "models/room.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

class Session;

class RoomManager {
public:
    void join(const std::string& room_id, const std::shared_ptr<Session>& session, const Player& player);
    void leave(const std::string& room_id, const std::shared_ptr<Session>& session, std::uint64_t player_id);
    void broadcast(const std::string& room_id, const std::string& sender_name, const std::string& content);

private:
    std::unordered_map<std::string, std::unordered_set<std::shared_ptr<Session>>> sessions_by_room_;
    std::unordered_map<std::string, Room> room_states_;
};

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, std::shared_ptr<RoomManager> room_manager)
        : ws_(std::move(socket)), room_manager_(std::move(room_manager)), session_id_(next_session_id_++) {
        player_.id = session_id_;
        player_.level = 1;
        player_.name = "玩家" + std::to_string(session_id_);
    }

    void start() {
        ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));

        auto self = shared_from_this();
        ws_.async_accept([self](beast::error_code ec) {
            if (ec) {
                std::cerr << "WebSocket 握手失败: " << ec.message() << std::endl;
                return;
            }

            self->deliver("欢迎连接游戏服务器，请先发送房间ID进入房间。\n"
                          "示例：1001");
            self->do_read();
        });
    }

    void deliver(const std::string& message) {
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

            std::string msg = beast::buffers_to_string(self->buffer_.data());
            self->buffer_.consume(self->buffer_.size());

            if (!self->joined_room_) {
                if (msg.empty()) {
                    self->deliver("房间ID不能为空，请重新输入。\n示例：1001");
                } else {
                    self->room_id_ = msg;
                    self->joined_room_ = true;
                    self->room_manager_->join(self->room_id_, self, self->player_);

                    self->deliver("成功进入房间 " + self->room_id_ +
                                  "，你的身份是 " + self->player_.name +
                                  "，现在你发送的消息会广播给房间内所有玩家。");
                }
            } else {
                self->room_manager_->broadcast(self->room_id_, self->player_.name, msg);
            }

            self->do_read();
        });
    }

    void do_write() {
        auto self = shared_from_this();
        ws_.text(true);
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
            room_manager_->leave(room_id_, shared_from_this(), player_.id);
            joined_room_ = false;
        }
    }

    websocket::stream<tcp::socket> ws_;
    beast::flat_buffer buffer_;
    std::deque<std::string> outgoing_messages_;
    std::shared_ptr<RoomManager> room_manager_;

    std::string room_id_;
    bool joined_room_ = false;
    std::uint64_t session_id_;
    Player player_;

    inline static std::atomic<std::uint64_t> next_session_id_{1};
};

void RoomManager::join(const std::string& room_id, const std::shared_ptr<Session>& session, const Player& player) {
    sessions_by_room_[room_id].insert(session);

    auto& room = room_states_[room_id];
    room.id = room_id;
    room.players.push_back(player);

    broadcast(room_id, "系统", "有玩家进入房间 " + room_id);
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
        players.erase(std::remove_if(players.begin(), players.end(), [player_id](const Player& player) {
                          return player.id == player_id;
                      }),
                      players.end());
    }

    if (session_it->second.empty()) {
        sessions_by_room_.erase(session_it);
        room_states_.erase(room_id);
        return;
    }

    broadcast(room_id, "系统", "有玩家离开房间 " + room_id);
}

void RoomManager::broadcast(const std::string& room_id, const std::string& sender_name, const std::string& content) {
    auto session_it = sessions_by_room_.find(room_id);
    if (session_it == sessions_by_room_.end()) {
        return;
    }

    auto room_it = room_states_.find(room_id);
    if (room_it != room_states_.end()) {
        room_it->second.received_messages.push_back(Message{MessageType::Chat, Message::now(), content});
    }

    const std::string message = "[" + Message::now() + "] " + sender_name + ": " + content;
    for (const auto& session : session_it->second) {
        session->deliver(message);
    }
}

class Server {
public:
    Server(asio::io_context& io_context, unsigned short port)
        : acceptor_(io_context, tcp::endpoint(tcp::v4(), port)), room_manager_(std::make_shared<RoomManager>()) {
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
