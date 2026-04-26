#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

class RoomManager;

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(boost::asio::ip::tcp::socket socket, std::shared_ptr<RoomManager> room_manager);

    void start();
    void deliver(const std::vector<std::uint8_t>& message);

private:
    void do_read();
    void do_write();
    void leave_room();
    void handle_join_auth(std::uint32_t message_id, const std::vector<std::uint8_t>& body);

    boost::beast::websocket::stream<boost::asio::ip::tcp::socket> ws_;
    boost::beast::flat_buffer buffer_;
    std::deque<std::vector<std::uint8_t>> outgoing_messages_;
    std::shared_ptr<RoomManager> room_manager_;

    std::string room_id_;
    bool joined_room_ = false;
    std::uint64_t challenge_ts_ms_ = 0;
    int session_id_;

    inline static std::atomic<int> next_session_id_{1};
};
