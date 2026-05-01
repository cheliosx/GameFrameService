#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/uuid/detail/md5.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <csignal>

#include "models/protocol.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using namespace std::chrono;

// ==================== 配置结构 ====================
struct BenchmarkConfig {
    std::string host = "127.0.0.1";
    std::string port = "8888";
    std::string redis_host = "127.0.0.1";
    int redis_port = 6379;
    std::string redis_password = "123456";

    // 房间配置
    int rooms_count = 100;           // 房间数量
    int players_per_room = 10;       // 每房间玩家数
    std::string room_secret = "123456"; // 房间密钥

    // 测试行为
    int fps = 2;                     // 服务器帧率
    int duration_seconds = 60;       // 测试持续时间
    int message_interval_ms = 500;   // 玩家发送消息间隔
    bool auto_start_game = true;     // 入房后自动开始游戏

    // 连接配置
    int connect_batch_size = 100;    // 每批连接数
    int connect_interval_ms = 100;   // 批次间隔
    int io_threads = 4;              // IO线程数
};

// ==================== 统计指标 ====================
struct LatencyStats {
    std::vector<double> latencies;
    std::mutex mutex;

    void add(double ms) {
        std::lock_guard<std::mutex> lock(mutex);
        latencies.push_back(ms);
    }

    void report() {
        std::lock_guard<std::mutex> lock(mutex);
        if (latencies.empty()) return;

        std::sort(latencies.begin(), latencies.end());
        double sum = std::accumulate(latencies.begin(), latencies.end(), 0.0);
        double avg = sum / latencies.size();
        double p50 = latencies[latencies.size() * 0.5];
        double p95 = latencies[latencies.size() * 0.95];
        double p99 = latencies[latencies.size() * 0.99];

        std::cout << "\n========== 延迟统计 (ms) ==========" << std::endl;
        std::cout << "样本数: " << latencies.size() << std::endl;
        std::cout << "平均: " << std::fixed << std::setprecision(2) << avg << std::endl;
        std::cout << "P50:  " << p50 << std::endl;
        std::cout << "P95:  " << p95 << std::endl;
        std::cout << "P99:  " << p99 << std::endl;
        std::cout << "最小: " << latencies.front() << std::endl;
        std::cout << "最大: " << latencies.back() << std::endl;
    }
};

struct BenchmarkStats {
    std::atomic<int> connected{0};           // 成功连接数
    std::atomic<int> connect_failed{0};      // 连接失败数
    std::atomic<int> auth_failed{0};         // 鉴权失败数
    std::atomic<int> messages_sent{0};       // 发送消息数
    std::atomic<int> messages_received{0};   // 接收消息数
    std::atomic<int> frames_received{0};     // 接收帧数
    std::atomic<int> errors{0};              // 错误数

    LatencyStats latency;

    void report() {
        std::cout << "\n========== 基准测试统计 ==========" << std::endl;
        std::cout << "成功连接: " << connected.load() << std::endl;
        std::cout << "连接失败: " << connect_failed.load() << std::endl;
        std::cout << "鉴权失败: " << auth_failed.load() << std::endl;
        std::cout << "发送消息: " << messages_sent.load() << std::endl;
        std::cout << "接收消息: " << messages_received.load() << std::endl;
        std::cout << "接收帧数: " << frames_received.load() << std::endl;
        std::cout << "错误数:   " << errors.load() << std::endl;
        latency.report();
    }
};

// ==================== 全局状态 ====================
std::atomic<bool> g_running{true};
std::unique_ptr<BenchmarkStats> g_stats;

// ==================== 工具函数 ====================
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

// ==================== 模拟玩家 ====================
class SimulatedPlayer : public std::enable_shared_from_this<SimulatedPlayer> {
public:
    SimulatedPlayer(asio::io_context& io_context,
                    const std::string& room_id,
                    std::uint64_t user_id,
                    const BenchmarkConfig& config)
        : ws_(io_context),
          room_id_(room_id),
          user_id_(user_id),
          config_(config),
          rng_(std::random_device{}()),
          dist_x_(0.0f, 100.0f),
          dist_y_(0.0f, 100.0f) {}

    void start(std::function<void()> on_complete) {
        on_complete_ = on_complete;
        do_connect();
    }

    void stop() {
        stopped_ = true;
        beast::error_code ec;
        ws_.close(websocket::close_code::normal, ec);
    }

private:
    void do_connect() {
        tcp::resolver resolver(ws_.get_executor());
        auto endpoints = resolver.resolve(config_.host, config_.port);

        asio::async_connect(ws_.next_layer(), endpoints,
            [self = shared_from_this()](beast::error_code ec, tcp::endpoint) {
                if (ec) {
                    g_stats->connect_failed++;
                    if (self->on_complete_) self->on_complete_();
                    return;
                }
                self->do_handshake();
            });
    }

    void do_handshake() {
        ws_.async_handshake(config_.host + ":" + config_.port, "/",
            [self = shared_from_this()](beast::error_code ec) {
                if (ec) {
                    g_stats->connect_failed++;
                    if (self->on_complete_) self->on_complete_();
                    return;
                }
                self->ws_.binary(true);
                self->do_read_challenge();
            });
    }

    void do_read_challenge() {
        auto buffer = std::make_shared<beast::flat_buffer>();
        ws_.async_read(*buffer,
            [self = shared_from_this(), buffer](beast::error_code ec, std::size_t) {
                if (ec) {
                    g_stats->errors++;
                    if (self->on_complete_) self->on_complete_();
                    return;
                }

                auto data = buffer->data();
                std::vector<std::uint8_t> bytes(asio::buffers_begin(data), asio::buffers_end(data));
                auto msg = protocol::decode(bytes);

                if (msg.protocol_type != ProtocolType::JoinRoomChallenge) {
                    g_stats->errors++;
                    if (self->on_complete_) self->on_complete_();
                    return;
                }

                std::uint64_t timestamp_ms = protocol::decode_join_room_challenge_body(msg.body);
                self->do_auth(timestamp_ms);
            });
    }

    void do_auth(std::uint64_t timestamp_ms) {
        std::string md5 = md5_hex(std::to_string(timestamp_ms) + config_.room_secret);
        auto auth_msg = protocol::encode_join_room_auth(1, room_id_, md5);

        ws_.async_write(asio::buffer(auth_msg),
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (ec) {
                    g_stats->auth_failed++;
                    if (self->on_complete_) self->on_complete_();
                    return;
                }
                self->do_read_auth_result();
            });
    }

    void do_read_auth_result() {
        auto buffer = std::make_shared<beast::flat_buffer>();
        ws_.async_read(*buffer,
            [self = shared_from_this(), buffer](beast::error_code ec, std::size_t) {
                if (ec) {
                    g_stats->auth_failed++;
                    if (self->on_complete_) self->on_complete_();
                    return;
                }

                auto data = buffer->data();
                std::vector<std::uint8_t> bytes(asio::buffers_begin(data), asio::buffers_end(data));
                auto msg = protocol::decode(bytes);

                if (msg.protocol_type != ProtocolType::SystemInfo) {
                    g_stats->auth_failed++;
                    if (self->on_complete_) self->on_complete_();
                    return;
                }

                g_stats->connected++;
                self->in_room_ = true;

                if (self->config_.auto_start_game) {
                    self->start_game();
                }

                self->start_reader();
                self->start_sender();

                if (self->on_complete_) self->on_complete_();
            });
    }

    void start_game() {
        auto msg = protocol::encode_game_start(2);
        ws_.write(asio::buffer(msg));
    }

    void start_reader() {
        do_read();
    }

    void do_read() {
        if (stopped_) return;

        auto buffer = std::make_shared<beast::flat_buffer>();
        ws_.async_read(*buffer,
            [self = shared_from_this(), buffer](beast::error_code ec, std::size_t bytes_transferred) {
                if (ec || self->stopped_) {
                    if (!self->stopped_) g_stats->errors++;
                    return;
                }

                auto data = buffer->data();
                std::vector<std::uint8_t> bytes(asio::buffers_begin(data), asio::buffers_end(data));

                try {
                    auto msg = protocol::decode(bytes);

                    if (msg.protocol_type == ProtocolType::ReplayFrames) {
                        g_stats->frames_received++;

                        // 计算延迟
                        auto now = steady_clock::now();
                        auto it = self->pending_messages_.find(msg.message_id);
                        if (it != self->pending_messages_.end()) {
                            auto latency = duration_cast<microseconds>(now - it->second).count() / 1000.0;
                            g_stats->latency.add(latency);
                            self->pending_messages_.erase(it);
                        }
                    } else if (msg.protocol_type == ProtocolType::SystemInfo) {
                        g_stats->messages_received++;
                    }
                } catch (...) {
                    g_stats->errors++;
                }

                self->do_read();
            });
    }

    void start_sender() {
        sender_thread_ = std::thread([self = shared_from_this()]() {
            while (!self->stopped_ && g_running.load()) {
                self->send_random_message();
                std::this_thread::sleep_for(milliseconds(self->config_.message_interval_ms));
            }
        });
    }

    void send_random_message() {
        if (!in_room_ || stopped_) return;

        std::uint32_t msg_id = next_message_id_++;
        std::vector<std::uint8_t> msg;

        // 随机发送位置或聊天
        if (dist_bool_(rng_)) {
            float x = dist_x_(rng_);
            float y = dist_y_(rng_);
            msg = protocol::encode_position(msg_id, x, y);
        } else {
            std::string chat = "msg_" + std::to_string(msg_id);
            msg = protocol::encode_chat(msg_id, chat);
        }

        pending_messages_[msg_id] = steady_clock::now();

        beast::error_code ec;
        ws_.write(asio::buffer(msg), ec);
        if (!ec) {
            g_stats->messages_sent++;
        }
    }

    websocket::stream<tcp::socket> ws_;
    std::string room_id_;
    std::uint64_t user_id_;
    BenchmarkConfig config_;

    std::atomic<bool> stopped_{false};
    std::atomic<bool> in_room_{false};
    std::function<void()> on_complete_;

    std::thread sender_thread_;
    std::uint32_t next_message_id_ = 3;
    std::unordered_map<std::uint32_t, steady_clock::time_point> pending_messages_;

    std::mt19937 rng_;
    std::uniform_real_distribution<float> dist_x_;
    std::uniform_real_distribution<float> dist_y_;
    std::bernoulli_distribution dist_bool_{0.5};
};

// ==================== 基准测试运行器 ====================
class BenchmarkRunner {
public:
    BenchmarkRunner(const BenchmarkConfig& config)
        : config_(config),
          work_guard_(asio::make_work_guard(io_context_)) {}

    void run() {
        std::cout << "========== 游戏服务器压力测试 ==========" << std::endl;
        std::cout << "目标房间数: " << config_.rooms_count << std::endl;
        std::cout << "每房间玩家: " << config_.players_per_room << std::endl;
        std::cout << "总连接数: " << config_.rooms_count * config_.players_per_room << std::endl;
        std::cout << "测试时长: " << config_.duration_seconds << "秒" << std::endl;
        std::cout << "=======================================" << std::endl;

        // 启动IO线程
        std::vector<std::thread> io_threads;
        for (int i = 0; i < config_.io_threads; ++i) {
            io_threads.emplace_back([this]() { io_context_.run(); });
        }

        // 分批创建连接
        auto start_time = steady_clock::now();
        std::atomic<int> completed{0};
        int total_players = config_.rooms_count * config_.players_per_room;

        for (int room_idx = 0; room_idx < config_.rooms_count; ++room_idx) {
            std::string room_id = std::to_string(room_idx + 1);

            for (int player_idx = 0; player_idx < config_.players_per_room; ++player_idx) {
                std::uint64_t user_id = room_idx * 1000 + player_idx + 1;

                auto player = std::make_shared<SimulatedPlayer>(
                    io_context_, room_id, user_id, config_);

                players_.push_back(player);

                player->start([&completed]() { completed++; });

                // 批次控制
                if ((room_idx * config_.players_per_room + player_idx + 1) % config_.connect_batch_size == 0) {
                    std::this_thread::sleep_for(milliseconds(config_.connect_interval_ms));
                }
            }
        }

        // 等待所有连接完成
        std::cout << "正在建立连接..." << std::endl;
        while (completed.load() < total_players && g_running.load()) {
            std::this_thread::sleep_for(milliseconds(100));
        }

        auto connect_time = duration_cast<seconds>(steady_clock::now() - start_time).count();
        std::cout << "连接完成，耗时: " << connect_time << "秒" << std::endl;
        std::cout << "成功: " << g_stats->connected.load()
                  << ", 失败: " << g_stats->connect_failed.load()
                  << ", 鉴权失败: " << g_stats->auth_failed.load() << std::endl;

        // 运行测试
        std::cout << "\n开始压力测试..." << std::endl;
        for (int i = 0; i < config_.duration_seconds && g_running.load(); ++i) {
            std::this_thread::sleep_for(seconds(1));

            if ((i + 1) % 10 == 0) {
                std::cout << "已运行 " << (i + 1) << "秒, "
                          << "帧接收: " << g_stats->frames_received.load()
                          << ", 消息发送: " << g_stats->messages_sent.load()
                          << ", 错误: " << g_stats->errors.load() << std::endl;
            }
        }

        // 停止测试
        std::cout << "\n正在停止测试..." << std::endl;
        g_running.store(false);

        for (auto& player : players_) {
            player->stop();
        }

        io_context_.stop();
        for (auto& t : io_threads) {
            if (t.joinable()) t.join();
        }

        // 输出报告
        g_stats->report();
    }

private:
    BenchmarkConfig config_;
    asio::io_context io_context_;
    asio::executor_work_guard<asio::io_context::executor_type> work_guard_;
    std::vector<std::shared_ptr<SimulatedPlayer>> players_;
};

// ==================== 命令行解析 ====================
BenchmarkConfig parse_args(int argc, char** argv) {
    BenchmarkConfig config;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) config.host = argv[++i];
        else if (arg == "--port" && i + 1 < argc) config.port = argv[++i];
        else if (arg == "--rooms" && i + 1 < argc) config.rooms_count = std::atoi(argv[++i]);
        else if (arg == "--players" && i + 1 < argc) config.players_per_room = std::atoi(argv[++i]);
        else if (arg == "--duration" && i + 1 < argc) config.duration_seconds = std::atoi(argv[++i]);
        else if (arg == "--interval" && i + 1 < argc) config.message_interval_ms = std::atoi(argv[++i]);
        else if (arg == "--batch" && i + 1 < argc) config.connect_batch_size = std::atoi(argv[++i]);
        else if (arg == "--io-threads" && i + 1 < argc) config.io_threads = std::atoi(argv[++i]);
        else if (arg == "--secret" && i + 1 < argc) config.room_secret = argv[++i];
        else if (arg == "--help") {
            std::cout << "用法: " << argv[0] << " [选项]\n"
                      << "选项:\n"
                      << "  --host <ip>         服务器地址 (默认: 127.0.0.1)\n"
                      << "  --port <port>       服务器端口 (默认: 8888)\n"
                      << "  --rooms <n>         房间数量 (默认: 100)\n"
                      << "  --players <n>       每房间玩家数 (默认: 10)\n"
                      << "  --duration <sec>    测试时长秒数 (默认: 60)\n"
                      << "  --interval <ms>     消息发送间隔 (默认: 500)\n"
                      << "  --batch <n>         每批连接数 (默认: 100)\n"
                      << "  --io-threads <n>    IO线程数 (默认: 4)\n"
                      << "  --secret <str>      房间密钥 (默认: 123456)\n"
                      << "  --help              显示帮助\n";
            std::exit(0);
        }
    }

    return config;
}

void signal_handler(int) {
    std::cout << "\n收到中断信号，正在停止..." << std::endl;
    g_running.store(false);
}

// ==================== 主函数 ====================
int main(int argc, char** argv) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    g_stats = std::make_unique<BenchmarkStats>();
    auto config = parse_args(argc, argv);

    try {
        BenchmarkRunner runner(config);
        runner.run();
    } catch (const std::exception& e) {
        std::cerr << "错误: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
