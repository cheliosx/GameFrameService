#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <atomic>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

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

        // 连接后默认进入房间 1
        ws.write(asio::buffer(std::string("mid=0;1")));

        std::atomic<bool> running{true};
        std::mutex output_mutex;
        std::uint64_t next_message_id = 1;

        std::thread receiver([&ws, &running, &output_mutex] {
            try {
                while (running.load()) {
                    beast::flat_buffer incoming;
                    ws.read(incoming);

                    std::lock_guard<std::mutex> lock(output_mutex);
                    std::cout << "\n服务器广播: " << beast::buffers_to_string(incoming.data())
                              << "\n-------------------------" << std::endl;
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
            std::cout << "✅ 角色[" << role_id << "]已连接并进入房间 1，输入消息发送（输入 exit 退出）\n"
                      << std::endl;
        }

        std::string input_msg;
        while (running.load()) {
            {
                std::lock_guard<std::mutex> lock(output_mutex);
                std::cout << "请输入消息: ";
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
            const std::string encoded_payload = "mid=" + std::to_string(next_message_id++) + ";" + payload;
            ws.write(asio::buffer(encoded_payload));
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
