#include "redis_client.hpp"

#include <sstream>
#include <stdexcept>

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

RedisClient::RedisClient(std::string host, unsigned short port, std::string password)
    : host_(std::move(host)),
      port_(port),
      password_(std::move(password)),
      resolver_(io_context_),
      socket_(io_context_) {
    connect();
}

void RedisClient::connect() {
    if (socket_.is_open()) {
        boost::system::error_code ec;
        socket_.close(ec);
    }

    auto endpoints = resolver_.resolve(host_, std::to_string(port_));
    asio::connect(socket_, endpoints);

    if (!password_.empty()) {
        write_command({"AUTH", password_});
        expect_simple_ok();
    }
}

void RedisClient::ensure_connected() {
    if (!socket_.is_open()) {
        connect();
    }
}

void RedisClient::write_command(const std::vector<std::string>& args) {
    std::ostringstream cmd;
    cmd << '*' << args.size() << "\r\n";
    for (const auto& arg : args) {
        cmd << '$' << arg.size() << "\r\n" << arg << "\r\n";
    }
    const auto text = cmd.str();
    asio::write(socket_, asio::buffer(text));
}

std::string RedisClient::read_line() {
    asio::read_until(socket_, buffer_, "\r\n");
    std::istream is(&buffer_);
    std::string line;
    std::getline(is, line);
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

void RedisClient::expect_simple_ok() {
    const auto line = read_line();
    if (line.empty() || line[0] != '+') {
        throw std::runtime_error("Redis AUTH返回异常: " + line);
    }
}

std::string RedisClient::read_bulk_string() {
    const auto header = read_line();
    if (header.empty() || header[0] != '$') {
        throw std::runtime_error("Redis GET返回异常: " + header);
    }

    const int len = std::stoi(header.substr(1));
    if (len < 0) {
        throw std::runtime_error("房间密钥不存在");
    }

    const std::size_t need = static_cast<std::size_t>(len + 2);
    if (buffer_.size() < need) {
        asio::read(socket_, buffer_, asio::transfer_exactly(need - buffer_.size()));
    }

    std::string payload(static_cast<std::size_t>(len), '\0');
    std::istream is(&buffer_);
    is.read(payload.data(), len);

    char crlf[2] = {0, 0};
    is.read(crlf, 2);
    if (crlf[0] != '\r' || crlf[1] != '\n') {
        throw std::runtime_error("Redis bulk string 结尾非法");
    }

    return payload;
}

std::string RedisClient::get_room_secret_impl(const std::string& room_id) {
    ensure_connected();
    write_command({"GET", "room:secret:" + room_id});
    return read_bulk_string();
}

std::string RedisClient::get_room_secret(const std::string& room_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        return get_room_secret_impl(room_id);
    } catch (const std::exception&) {
        connect();
        return get_room_secret_impl(room_id);
    }
}
