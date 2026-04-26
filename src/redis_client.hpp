#pragma once

#include <boost/asio.hpp>

#include <mutex>
#include <string>
#include <vector>

class RedisClient {
public:
    RedisClient(std::string host, unsigned short port, std::string password);

    std::string get_room_secret(const std::string& room_id);

private:
    void connect();
    void ensure_connected();
    void write_command(const std::vector<std::string>& args);
    std::string read_line();
    void expect_simple_ok();
    std::string read_bulk_string();
    std::string get_room_secret_impl(const std::string& room_id);

    std::string host_;
    unsigned short port_;
    std::string password_;

    boost::asio::io_context io_context_;
    boost::asio::ip::tcp::resolver resolver_;
    boost::asio::ip::tcp::socket socket_;
    boost::asio::streambuf buffer_;
    std::mutex mutex_;
};
