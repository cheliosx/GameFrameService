#include <boost/asio.hpp>
#include <boost/beast/core.hpp>

#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

#include "redis_client.hpp"
#include "room_manager.hpp"
#include "session.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
using tcp = asio::ip::tcp;

struct ServerConfig {
    unsigned short port = 8888;
    std::uint32_t fps = 1;
    std::string redis_host = "127.0.0.1";
    unsigned short redis_port = 6379;
    std::string redis_password = "123456";
    std::size_t num_shards = 4;
};

namespace {
std::string trim_copy(const std::string& input) {
    const auto start = input.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }
    const auto end = input.find_last_not_of(" \t\r\n");
    return input.substr(start, end - start + 1);
}

void parse_config_line(ServerConfig& cfg, const std::string& line) {
    const auto clean = trim_copy(line);
    if (clean.empty() || clean[0] == '#') {
        return;
    }

    std::istringstream iss(clean);
    std::string key;
    std::string value;
    iss >> key;
    iss >> value;
    if (key.empty() || value.empty()) {
        return;
    }

    if (key == "port") {
        cfg.port = static_cast<unsigned short>(std::stoul(value));
    } else if (key == "fps") {
        cfg.fps = static_cast<std::uint32_t>(std::stoul(value));
    } else if (key == "redis_host") {
        cfg.redis_host = value;
    } else if (key == "redis_port") {
        cfg.redis_port = static_cast<unsigned short>(std::stoul(value));
    } else if (key == "redis_password") {
        cfg.redis_password = value;
    } else if (key == "num_shards") {
        cfg.num_shards = static_cast<std::size_t>(std::stoul(value));
    }
}

ServerConfig parse_config_file(const std::string& path) {
    ServerConfig cfg;
    std::ifstream conf(path);
    if (!conf.is_open()) {
        throw std::runtime_error("无法打开配置文件: " + path);
    }

    std::string line;
    while (std::getline(conf, line)) {
        parse_config_line(cfg, line);
    }
    return cfg;
}

ServerConfig parse_args(int argc, char** argv) {
    std::string config_path;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        }
    }

    if (config_path.empty()) {
        return ServerConfig{};
    }
    return parse_config_file(config_path);
}
} // namespace

class Server {
public:
    Server(asio::io_context& io_context, const ServerConfig& cfg)
        : acceptor_(io_context, tcp::endpoint(tcp::v4(), cfg.port)),
          redis_client_(std::make_shared<RedisClient>(cfg.redis_host, cfg.redis_port, cfg.redis_password)),
          room_manager_(std::make_shared<RoomManager>(cfg.fps, redis_client_, cfg.num_shards)) {
        do_accept();
    }

private:
    void do_accept() {
        acceptor_.async_accept([this](beast::error_code ec, tcp::socket socket) {
            if (!ec) {
                std::make_shared<Session>(std::move(socket), room_manager_)->start();
            }
            do_accept();
        });
    }

    tcp::acceptor acceptor_;
    std::shared_ptr<RedisClient> redis_client_;
    std::shared_ptr<RoomManager> room_manager_;
};

int main(int argc, char** argv) {
    try {
        const auto cfg = parse_args(argc, argv);

        asio::io_context io_context;
        Server server(io_context, cfg);

        std::cout << "WebSocket 服务器启动: port=" << cfg.port << ", fps=" << cfg.fps
                  << ", redis=" << cfg.redis_host << ':' << cfg.redis_port
                  << ", shards=" << cfg.num_shards << std::endl;
        io_context.run();
    } catch (const std::exception& e) {
        std::cerr << "异常: " << e.what() << std::endl;
    }

    return 0;
}
