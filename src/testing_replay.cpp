#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
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

        // 发送一条聊天和一条坐标，等待服务器进入下一帧广播周期
        ws.write(asio::buffer(protocol::encode_chat(1, "replay-test-chat")));
        ws.write(asio::buffer(protocol::encode_position(2, 9.5F, 7.25F)));
        std::this_thread::sleep_for(std::chrono::seconds(4));

        // 请求从 frame_id=0 开始的 2 帧数据
        ws.write(asio::buffer(protocol::encode_replay_request(3, 0, 2)));

        beast::flat_buffer incoming;
        ws.read(incoming);
        const auto data = incoming.data();
        std::vector<std::uint8_t> bytes(asio::buffers_begin(data), asio::buffers_end(data));
        const auto decoded = protocol::decode(bytes);

        if (decoded.protocol_type != ProtocolType::ReplayFrames) {
            std::cerr << "replay test failed: response is not ReplayFrames" << std::endl;
            return 1;
        }

        const auto frames = protocol::deserialize_frames(decoded.body);
        if (frames.empty()) {
            std::cerr << "replay test failed: no frames returned" << std::endl;
            return 1;
        }

        std::cout << "[replay test] frame_count=" << frames.size() << std::endl;
        for (const auto& frame : frames) {
            std::cout << "  frame_id=" << frame.frame_id << ", op_count=" << frame.operations.size() << std::endl;
        }

        ws.close(websocket::close_code::normal);
    } catch (const std::exception& e) {
        std::cerr << "replay test failed: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
