#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <algorithm>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

class Session;

class RoomManager {
public:
    void join(const std::string& room_id, const std::shared_ptr<Session>& session);
    void leave(const std::string& room_id, const std::shared_ptr<Session>& session);
    void broadcast(const std::string& room_id, const std::string& message);

private:
    using SessionRef = std::weak_ptr<Session>;
    std::unordered_map<std::string, std::vector<SessionRef>> rooms_;
    std::mutex mutex_;
};

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, RoomManager& room_manager, std::size_t id)
        : ws_(std::move(socket)), room_manager_(room_manager), id_(id) {}

    void start() {
        ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));

        auto self = shared_from_this();
        ws_.async_accept([self](beast::error_code ec) {
            if (ec) {
                std::cerr << "WebSocket 握手失败: " << ec.message() << std::endl;
                return;
            }
            self->queue_message("欢迎进入游戏服务器，请先发送房间号。\n");
            self->do_read();
        });
    }

    void queue_message(const std::string& message) {
        bool writing = !write_queue_.empty();
        write_queue_.push_back(message);

        if (!writing) {
            do_write();
        }
    }

    std::size_t id() const { return id_; }

private:
    void do_read() {
        auto self = shared_from_this();
        ws_.async_read(buffer_, [self](beast::error_code ec, std::size_t) {
            if (ec == websocket::error::closed) {
                self->on_disconnect();
                return;
            }
            if (ec) {
                std::cerr << "读取失败: " << ec.message() << std::endl;
                self->on_disconnect();
                return;
            }

            std::string msg = beast::buffers_to_string(self->buffer_.data());
            self->buffer_.consume(self->buffer_.size());
            self->on_message(msg);
            self->do_read();
        });
    }

    void on_message(std::string msg) {
        while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) {
            msg.pop_back();
        }

        if (msg.empty()) {
            queue_message("消息不能为空。\n");
            return;
        }

        if (room_id_.empty()) {
            room_id_ = msg;
            room_manager_.join(room_id_, shared_from_this());

            std::string joined = "你已加入房间[" + room_id_ + "]，现在可以聊天。\n";
            queue_message(joined);

            std::string entered = "[系统] 玩家" + std::to_string(id_) + " 进入房间。\n";
            room_manager_.broadcast(room_id_, entered);
            std::cout << "玩家" << id_ << " 加入房间: " << room_id_ << std::endl;
            return;
        }

        std::string chat = "[房间" + room_id_ + "] 玩家" + std::to_string(id_) + ": " + msg + "\n";
        room_manager_.broadcast(room_id_, chat);
    }

    void do_write() {
        auto self = shared_from_this();
        ws_.text(true);
        ws_.async_write(asio::buffer(write_queue_.front()), [self](beast::error_code ec, std::size_t) {
            if (ec) {
                std::cerr << "写入失败: " << ec.message() << std::endl;
                self->on_disconnect();
                return;
            }

            self->write_queue_.pop_front();
            if (!self->write_queue_.empty()) {
                self->do_write();
            }
        });
    }

    void on_disconnect() {
        if (!room_id_.empty()) {
            std::string left = "[系统] 玩家" + std::to_string(id_) + " 离开房间。\n";
            room_manager_.leave(room_id_, shared_from_this());
            room_manager_.broadcast(room_id_, left);
            room_id_.clear();
        }
    }

    websocket::stream<tcp::socket> ws_;
    beast::flat_buffer buffer_;
    std::deque<std::string> write_queue_;
    RoomManager& room_manager_;
    std::string room_id_;
    std::size_t id_;
};

void RoomManager::join(const std::string& room_id, const std::shared_ptr<Session>& session) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& members = rooms_[room_id];
    members.erase(
        std::remove_if(members.begin(), members.end(),
                       [](const SessionRef& weak) { return weak.expired(); }),
        members.end());
    members.push_back(session);
}

void RoomManager::leave(const std::string& room_id, const std::shared_ptr<Session>& session) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = rooms_.find(room_id);
    if (it == rooms_.end()) {
        return;
    }

    auto& members = it->second;
    members.erase(
        std::remove_if(members.begin(), members.end(), [&session](const SessionRef& weak) {
            auto shared = weak.lock();
            return !shared || shared == session;
        }),
        members.end());

    if (members.empty()) {
        rooms_.erase(it);
    }
}

void RoomManager::broadcast(const std::string& room_id, const std::string& message) {
    std::vector<std::shared_ptr<Session>> live_members;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = rooms_.find(room_id);
        if (it == rooms_.end()) {
            return;
        }

        auto& members = it->second;
        members.erase(
            std::remove_if(members.begin(), members.end(),
                           [&live_members](const SessionRef& weak) {
                               auto shared = weak.lock();
                               if (!shared) {
                                   return true;
                               }
                               live_members.push_back(shared);
                               return false;
                           }),
            members.end());

        if (members.empty()) {
            rooms_.erase(it);
        }
    }

    for (auto& session : live_members) {
        session->queue_message(message);
    }
}

class Server {
public:
    Server(asio::io_context& io_context, unsigned short port)
        : acceptor_(io_context, tcp::endpoint(tcp::v4(), port)) {
        do_accept();
    }

private:
    void do_accept() {
        acceptor_.async_accept([this](beast::error_code ec, tcp::socket socket) {
            if (!ec) {
                std::make_shared<Session>(std::move(socket), room_manager_, next_session_id_++)->start();
            } else {
                std::cerr << "接受连接失败: " << ec.message() << std::endl;
            }
            do_accept();
        });
    }

    tcp::acceptor acceptor_;
    RoomManager room_manager_;
    std::size_t next_session_id_ = 1;
};

int main() {
    try {
        asio::io_context io_context;
        Server server(io_context, 8888);

        std::cout << "WebSocket 游戏服务器启动在端口 8888..." << std::endl;
        io_context.run();
    } catch (const std::exception& e) {
        std::cerr << "异常: " << e.what() << std::endl;
    }

    return 0;
}
