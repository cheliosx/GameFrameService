#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/uuid/detail/md5.hpp>

#include <atomic>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "models/protocol.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

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

void print_frame(const Frame& frame) {
    std::cout << "  frame_id=" << frame.frame_id << ", op_count=" << frame.operations.size() << std::endl;
    for (const auto& op : frame.operations) {
        std::cout << "    - op mid=" << op.message_id << ", user=" << op.user_id
                  << ", type=" << static_cast<std::uint16_t>(op.info_type);
        if (op.info_type == InfoType::Chat) {
            std::cout << ", chat=" << protocol::decode_chat_payload(op.payload);
        } else if (op.info_type == InfoType::Position) {
            const auto [x, y] = protocol::decode_position_payload(op.payload);
            std::cout << ", pos=(" << x << ", " << y << ")";
        }
        std::cout << std::endl;
    }
}

void print_help() {
    std::cout << "\n========== 命令帮助 ==========" << std::endl;
    std::cout << "1 <内容>     - 发送聊天消息，如: 1 hello" << std::endl;
    std::cout << "2 <x> <y>    - 发送位置，如: 2 10.5 20.3" << std::endl;
    std::cout << "go   - 开始游戏" << std::endl;
    std::cout << "info         - 获取服务器信息" << std::endl;
    std::cout << "help         - 显示此帮助信息" << std::endl;
    std::cout << "exit         - 退出程序" << std::endl;
    std::cout << "============================\n" << std::endl;
}

int main() {
    try {
        std::string role_id;
        std::cout << "请输入角色ID: ";
        std::getline(std::cin, role_id);
        if (role_id.empty()) {
            std::cerr << "❌ 角色ID不能为空" << std::endl;
            return 1;
        }

        const std::string room_id = "1";
        const std::string test_secret = "123456";

        asio::io_context io_context;
        const std::string host = "127.0.0.1";
        const std::string port = "8888";

        tcp::resolver resolver(io_context);
        auto endpoints = resolver.resolve(host, port);

        websocket::stream<tcp::socket> ws(io_context);
        asio::connect(ws.next_layer(), endpoints);
        ws.handshake(host + ":" + port, "/");
        ws.binary(true);

        std::uint32_t next_message_id = 1;

        beast::flat_buffer challenge_buf;
        ws.read(challenge_buf);
        const auto challenge_data = challenge_buf.data();
        std::vector<std::uint8_t> challenge_bytes(asio::buffers_begin(challenge_data), asio::buffers_end(challenge_data));
        const auto challenge_msg = protocol::decode(challenge_bytes);
        if (challenge_msg.protocol_type != ProtocolType::JoinRoomChallenge) {
            std::cerr << "❌ 未收到入房挑战协议" << std::endl;
            return 1;
        }

        const std::uint64_t timestamp_ms = protocol::decode_join_room_challenge_body(challenge_msg.body);
        const std::string md5 = md5_hex(std::to_string(timestamp_ms) + test_secret);
        const auto join_auth = protocol::encode_join_room_auth(next_message_id++, room_id, md5);
        ws.write(asio::buffer(join_auth));

        beast::flat_buffer join_result_buf;
        ws.read(join_result_buf);
        const auto join_data = join_result_buf.data();
        std::vector<std::uint8_t> join_bytes(asio::buffers_begin(join_data), asio::buffers_end(join_data));
        const auto join_result = protocol::decode(join_bytes);
        if (join_result.protocol_type != ProtocolType::SystemInfo) {
            std::cerr << "❌ 入房失败: " << protocol::decode_chat_payload(join_result.body) << std::endl;
            return 1;
        }

        std::cout << "✅ 入房成功: " << protocol::decode_system_info_body(join_result.body) << std::endl;

        std::atomic<bool> running{true};
        std::mutex output_mutex;

        std::thread receiver([&ws, &running, &output_mutex] {
            try {
                while (running.load()) {
                    beast::flat_buffer incoming;
                    ws.read(incoming);

                    const auto data = incoming.data();
                    std::vector<std::uint8_t> bytes(asio::buffers_begin(data), asio::buffers_end(data));
                    const auto decoded = protocol::decode(bytes);

                    std::lock_guard<std::mutex> lock(output_mutex);
                    if (decoded.protocol_type == ProtocolType::SystemInfo) {
                        std::cout << "\n[系统信息] " << protocol::decode_system_info_body(decoded.body) << std::endl;
                    } else if (decoded.protocol_type == ProtocolType::ReplayFrames) {
                        if (decoded.body.size() < 8) {
                            std::cout << "\n⚠️ 补帧数据长度不足" << std::endl;
                            continue;
                        }
                        std::size_t offset = 0;
                        const auto current_frame_id = protocol::read_u32(decoded.body, offset);
                        offset += 8; // Skip count field

                        std::vector<std::uint8_t> frames_data(decoded.body.begin() + offset, decoded.body.end());
                        const auto frames = protocol::deserialize_frames(frames_data);

                        std::cout << "\n[帧数据] current_frame_id=" << current_frame_id << ", count=" << frames.size() << std::endl;
                        for (const auto& frame : frames) {
                            print_frame(frame);
                        }
                    }
                }
            } catch (const std::exception& e) {
                if (running.load()) {
                    std::lock_guard<std::mutex> lock(output_mutex);
                    std::cout << "\n⚠️ 接收消息中断: " << e.what() << std::endl;
                }
                running.store(false);
            }
        });

        {
            std::lock_guard<std::mutex> lock(output_mutex);
            print_help();
            std::cout << "\n✅ 角色[" << role_id << "]已连接并通过鉴权进入房间 1\n" << std::endl;
        }

        std::string input_msg;
        while (running.load()) {
            {
                std::lock_guard<std::mutex> lock(output_mutex);
                std::cout << "> ";
            }
            if (!std::getline(std::cin, input_msg)) {
                break;
            }

            std::istringstream iss(input_msg);
            std::string cmd;
            iss >> cmd;

            if (cmd.empty() || cmd[0] == '#') {
                continue;
            }

            if (cmd == "exit") {
                std::lock_guard<std::mutex> lock(output_mutex);
                std::cout << "👋 退出程序..." << std::endl;
                break;
            }

            if (cmd == "help") {
                std::lock_guard<std::mutex> lock(output_mutex);
                print_help();
                continue;
            }

            if (cmd == "go") {
                const auto start_msg = protocol::encode_game_start(next_message_id++);
                ws.write(asio::buffer(start_msg));
                continue;
            }

            if (cmd == "info") {
                const auto info_msg = protocol::encode_system_info(next_message_id++, "get_server_info");
                ws.write(asio::buffer(info_msg));
                continue;
            }

            if (cmd == "1") {
                std::string content;
                std::getline(iss, content);
                if (content.empty() || content[0] == ' ') {
                    content = content.substr(1);
                }
                if (content.empty()) {
                    std::lock_guard<std::mutex> lock(output_mutex);
                    std::cout << "⚠️  聊天内容不能为空" << std::endl;
                    continue;
                }
                const std::string payload = "[角色" + role_id + "] " + content;
                const auto encoded = protocol::encode_chat(next_message_id++, payload);
                ws.write(asio::buffer(encoded));
                continue;
            }

            if (cmd == "2") {
                std::string pos_str;
                std::getline(iss, pos_str);
                if (pos_str.empty() || pos_str[0] == ' ') {
                    pos_str = pos_str.substr(1);
                }
                float x = 0.0f, y = 0.0f;
                std::istringstream pos_iss(pos_str);
                pos_iss >> x >> y;
                const auto encoded = protocol::encode_position(next_message_id++, x, y);
                ws.write(asio::buffer(encoded));
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(output_mutex);
                std::cout << "⚠️  未知命令: " << cmd << "，输入 help 查看帮助" << std::endl;
            }
        }

        running.store(false);
        beast::error_code ec;
        ws.close(websocket::close_code::normal, ec);

        if (receiver.joinable()) {
            receiver.join();
        }
    } catch (const std::exception& e) {
        std::cerr << "❌ 异常: " << e.what() << std::endl;
    }

    return 0;
}
