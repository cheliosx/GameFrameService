#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <iostream>
#include <string>

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

        beast::flat_buffer buffer;

        // 第一步默认进入房间 1
        ws.write(asio::buffer(std::string("1")));
        ws.read(buffer);
        std::cout << "服务器回复: " << beast::buffers_to_string(buffer.data())
                  << "\n-------------------------" << std::endl;
        buffer.consume(buffer.size());

        std::cout << "✅ 角色[" << role_id << "]已连接并进入房间 1，输入消息发送（输入 exit 退出）\n"
                  << std::endl;

        std::string input_msg;
        while (true) {
            std::cout << "请输入消息: ";
            std::getline(std::cin, input_msg);

            if (input_msg == "exit") {
                std::cout << "👋 退出程序..." << std::endl;
                break;
            }

            if (input_msg.empty()) {
                std::cout << "⚠️  消息不能为空，请重新输入" << std::endl;
                continue;
            }

            const std::string payload = "[角色" + role_id + "] " + input_msg;
            ws.write(asio::buffer(payload));
            ws.read(buffer);

            std::cout << "服务器回复: " << beast::buffers_to_string(buffer.data())
                      << "\n-------------------------" << std::endl;
            buffer.consume(buffer.size());
        }

        ws.close(websocket::close_code::normal);
    } catch (const std::exception& e) {
        std::cerr << "❌ 异常: " << e.what() << std::endl;
    }

    return 0;
}
