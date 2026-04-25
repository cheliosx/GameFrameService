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
        asio::io_context io_context;

        const std::string host = "127.0.0.1";
        const std::string port = "8888";

        tcp::resolver resolver(io_context);
        auto endpoints = resolver.resolve(host, port);

        websocket::stream<tcp::socket> ws(io_context);
        asio::connect(ws.next_layer(), endpoints);
        ws.handshake(host + ":" + port, "/");

        std::cout << "✅ 已连接 WebSocket 游戏服务器。\n" << std::endl;

        beast::flat_buffer buffer;
        ws.read(buffer);
        std::cout << beast::buffers_to_string(buffer.data());
        buffer.consume(buffer.size());

        std::string room_id;
        std::cout << "请输入房间号: ";
        std::getline(std::cin, room_id);
        ws.write(asio::buffer(room_id));

        ws.read(buffer);
        std::cout << beast::buffers_to_string(buffer.data());
        buffer.consume(buffer.size());

        std::string input_msg;
        while (true) {
            std::cout << "请输入消息(输入 exit 退出): ";
            std::getline(std::cin, input_msg);

            if (input_msg == "exit") {
                std::cout << "👋 退出程序..." << std::endl;
                break;
            }

            if (input_msg.empty()) {
                std::cout << "⚠️  消息不能为空，请重新输入" << std::endl;
                continue;
            }

            ws.write(asio::buffer(input_msg));
            ws.read(buffer);

            std::cout << beast::buffers_to_string(buffer.data())
                      << "-------------------------" << std::endl;
            buffer.consume(buffer.size());
        }

        ws.close(websocket::close_code::normal);
    } catch (const std::exception& e) {
        std::cerr << "❌ 异常: " << e.what() << std::endl;
    }

    return 0;
}
