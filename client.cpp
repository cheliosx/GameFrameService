#include <boost/asio.hpp>
#include <iostream>
#include <string>
#include <limits>

namespace asio = boost::asio;
using asio::ip::tcp;

int main() {
    try {
        asio::io_context io_context;

        // 连接服务器
        tcp::resolver resolver(io_context);
        auto endpoints = resolver.resolve("127.0.0.1", "8888");

        tcp::socket socket(io_context);
        asio::connect(socket, endpoints);
        std::cout << "✅ 已连接服务器，输入消息发送（输入 exit 退出）\n" << std::endl;

        std::string input_msg;
        while (true) {
            // 提示输入
            std::cout << "请输入消息: ";
            // 读取一行输入
            std::getline(std::cin, input_msg);

            // 如果输入 exit，退出循环，关闭连接
            if (input_msg == "exit") {
                std::cout << "👋 退出程序..." << std::endl;
                break;
            }

            // 空消息不发送
            if (input_msg.empty()) {
                std::cout << "⚠️  消息不能为空，请重新输入" << std::endl;
                continue;
            }

            // 发送消息给服务器
            asio::write(socket, asio::buffer(input_msg + "\n"));

            // 读取服务器回复（最多 1024 字节）
            char reply[1024];
            boost::system::error_code error;
            size_t reply_len = socket.read_some(asio::buffer(reply), error);

            // 对端断开或出错
            if (error == asio::error::eof || error) {
                std::cerr << "❌ 服务器断开连接: " << error.message() << std::endl;
                break;
            }

            // 打印回复
            std::cout << "服务器回复: ";
            std::cout.write(reply, reply_len);
            std::cout << "\n-------------------------" << std::endl;
        }

        // 关闭socket
        socket.close();

    } catch (std::exception& e) {
        std::cerr << "❌ 异常: " << e.what() << std::endl;
    }

    return 0;
}