#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <atomic>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "models/protocol.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

void print_frame(const Frame& frame) {
    std::cout << "\n[帧广播] frame_id=" << frame.frame_id << ", op_count=" << frame.operations.size() << std::endl;
    for (const auto& op : frame.operations) {
        std::cout << "  - op mid=" << op.message_id << ", type=" << static_cast<std::uint16_t>(op.message_type);
        if (op.message_type == MessageType::Chat) {
            std::cout << ", chat=" << protocol::decode_chat_body(op.payload);
        } else if (op.message_type == MessageType::SetPosition) {
            const auto [x, y] = protocol::decode_position_body(op.payload);
            std::cout << ", pos=(" << x << ", " << y << ")";
        } else if (op.message_type == MessageType::SystemInfo) {
            std::cout << ", system_info=" << protocol::decode_chat_body(op.payload);
        }
        std::cout << std::endl;
    }
    std::cout << "-------------------------" << std::endl;
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

        asio::io_context io_context;

        const std::string host = "127.0.0.1";
        const std::string port = "8888";

        tcp::resolver resolver(io_context);
        auto endpoints = resolver.resolve(host, port);

        websocket::stream<tcp::socket> ws(io_context);
        asio::connect(ws.next_layer(), endpoints);
        ws.handshake(host + ":" + port, "/");

        ws.binary(true);

        std::atomic<bool> running{true};
        std::mutex output_mutex;
        std::uint32_t next_message_id = 1;

        std::thread receiver([&ws, &running, &output_mutex] {
            try {
                while (running.load()) {
                    beast::flat_buffer incoming;
                    ws.read(incoming);

                    const auto data = incoming.data();
                    std::vector<std::uint8_t> bytes(asio::buffers_begin(data), asio::buffers_end(data));
                    const auto decoded = protocol::decode(bytes);

                    std::lock_guard<std::mutex> lock(output_mutex);
                    if (decoded.message_type == MessageType::SystemInfo) {
                        std::cout << "\n[系统信息][mid=" << decoded.message_id << "] "
                                  << protocol::decode_chat_body(decoded.body)
                                  << "\n-------------------------" << std::endl;
                    } else if (decoded.message_type == MessageType::FrameData) {
                        const auto frame = protocol::deserialize_frame(decoded.body);
                        print_frame(frame);
                    } else if (decoded.message_type == MessageType::Chat) {
                        std::cout << "\n[收到][mid=" << decoded.message_id << "] "
                                  << protocol::decode_chat_body(decoded.body)
                                  << "\n-------------------------" << std::endl;
                    } else if (decoded.message_type == MessageType::SetPosition) {
                        const auto [x, y] = protocol::decode_position_body(decoded.body);
                        std::cout << "\n[收到位置][mid=" << decoded.message_id << "] x=" << x << ", y=" << y
                                  << "\n-------------------------" << std::endl;
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
            std::cout << "✅ 角色[" << role_id << "]已连接并进入房间 1，输入聊天消息（输入 exit 退出）\n" << std::endl;
        }

        std::string input_msg;
        while (running.load()) {
            {
                std::lock_guard<std::mutex> lock(output_mutex);
                std::cout << "请输入聊天消息: ";
            }
            if (!std::getline(std::cin, input_msg)) {
                break;
            }

            if (input_msg == "exit") {
                std::lock_guard<std::mutex> lock(output_mutex);
                std::cout << "👋 退出程序..." << std::endl;
                break;
            }

            if (input_msg.empty()) {
                std::lock_guard<std::mutex> lock(output_mutex);
                std::cout << "⚠️  消息不能为空，请重新输入" << std::endl;
                continue;
            }

            const std::string payload = "[角色" + role_id + "] " + input_msg;
            const auto encoded = protocol::encode_chat(next_message_id++, payload);
            ws.write(asio::buffer(encoded));
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
