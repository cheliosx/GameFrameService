#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <iostream>
#include <string>
#include <vector>

#include "models/protocol.hpp"

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
        ws.binary(true);

        // 测试发送位置消息（x,y 两个 float）
        const auto position_message = protocol::encode_position(1, 12.5F, -3.25F);
        ws.write(asio::buffer(position_message));

        beast::flat_buffer incoming;
        ws.read(incoming);
        const auto data = incoming.data();
        std::vector<std::uint8_t> bytes(asio::buffers_begin(data), asio::buffers_end(data));
        const auto decoded = protocol::decode(bytes);

        if (decoded.message_type == MessageType::FrameData) {
            const auto frame = protocol::deserialize_frame(decoded.body);
            std::cout << "[position test] frame_id=" << frame.frame_id << ", op_count=" << frame.operations.size()
                      << std::endl;
            for (const auto& operation : frame.operations) {
                if (operation.message_type == MessageType::SetPosition) {
                    const auto [x, y] = protocol::decode_position_body(operation.payload);
                    std::cout << "  position op mid=" << operation.message_id << ", user=" << operation.user_id
                              << ", x=" << x << ", y=" << y
                              << std::endl;
                }
            }
        }

        ws.close(websocket::close_code::normal);
    } catch (const std::exception& e) {
        std::cerr << "position test failed: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
