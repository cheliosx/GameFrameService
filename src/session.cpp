#include "session.hpp"

#include <boost/asio/post.hpp>

#include <chrono>
#include <stdexcept>

#include "models/protocol.hpp"
#include "room_manager.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;

namespace {
std::uint64_t now_timestamp_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}
}

Session::Session(asio::ip::tcp::socket socket, std::shared_ptr<RoomManager> room_manager)
    : ws_(std::move(socket)), room_manager_(std::move(room_manager)), session_id_(next_session_id_++) {}

void Session::start() {
    ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));

    auto self = shared_from_this();
    ws_.async_accept([self](beast::error_code ec) {
        if (ec) {
            return;
        }

        self->challenge_ts_ms_ = now_timestamp_ms();
        self->deliver(protocol::encode_join_room_challenge(0, self->challenge_ts_ms_));
        self->do_read();
    });
}

void Session::deliver(const std::vector<std::uint8_t>& message) {
    auto self = shared_from_this();
    asio::post(ws_.get_executor(), [self, message] {
        const bool writing = !self->outgoing_messages_.empty();
        self->outgoing_messages_.push_back(message);
        if (!writing) {
            self->do_write();
        }
    });
}

void Session::do_read() {
    auto self = shared_from_this();
    ws_.async_read(buffer_, [self](beast::error_code ec, std::size_t) {
        if (ec == websocket::error::closed) {
            self->leave_room();
            return;
        }
        if (ec) {
            self->leave_room();
            return;
        }

        const auto data = self->buffer_.data();
        std::vector<std::uint8_t> bytes(asio::buffers_begin(data), asio::buffers_end(data));
        self->buffer_.consume(self->buffer_.size());

        if (bytes.size() < 6) {
            self->deliver(protocol::encode_chat(0, "消息长度不足，至少需要6字节"));
            self->do_read();
            return;
        }

        const std::uint32_t message_id = protocol::read_u32(bytes, 0);
        const auto protocol_type = static_cast<ProtocolType>(protocol::read_u16(bytes, 4));
        const std::vector<std::uint8_t> body(bytes.begin() + 6, bytes.end());

        try {
            if (protocol_type == ProtocolType::JoinRoomAuth) {
                self->handle_join_auth(message_id, body);
            } else if (!self->joined_room_) {
                self->deliver(protocol::encode_chat(message_id, "请先发送入房鉴权协议"));
            } else if (protocol_type == ProtocolType::SystemInfo) {
                self->deliver(protocol::encode_system_info(message_id, self->room_manager_->server_info()));
            } else if (protocol_type == ProtocolType::GameStart) {
                self->room_manager_->start_game(self->room_id_);
                self->deliver(protocol::encode_system_info(message_id, "游戏已开始"));
            } else if (protocol_type == ProtocolType::SendInfo) {
                const auto [info_type, payload] = protocol::decode_send_info_body(body);
                self->room_manager_->enqueue_operation(self->room_id_, message_id, self->session_id_, info_type, payload);
            } else if (protocol_type == ProtocolType::ReplayFrames) {
                const auto [start_frame_id, count] = protocol::decode_replay_request_body(body);
                const auto frames = self->room_manager_->get_frames_after(self->room_id_, start_frame_id, count);
                self->deliver(protocol::encode_replay_response(message_id, frames));
            }
        } catch (const std::exception& e) {
            self->deliver(protocol::encode_chat(message_id, std::string("协议处理失败: ") + e.what()));
        }

        self->do_read();
    });
}

void Session::handle_join_auth(std::uint32_t message_id, const std::vector<std::uint8_t>& body) {
    const auto [room_id, client_md5] = protocol::decode_join_room_auth_body(body);
    if (!room_manager_->verify_join_auth(room_id, challenge_ts_ms_, client_md5)) {
        deliver(protocol::encode_chat(message_id, "入房鉴权失败，md5不匹配"));
        return;
    }

    room_id_ = room_id;
    joined_room_ = true;
    room_manager_->join(room_id_, shared_from_this(), session_id_);
    deliver(protocol::encode_system_info(message_id, "入房成功，room_id=" + room_id_));
}

void Session::do_write() {
    auto self = shared_from_this();
    ws_.binary(true);
    ws_.async_write(asio::buffer(outgoing_messages_.front()), [self](beast::error_code ec, std::size_t) {
        if (ec) {
            self->leave_room();
            return;
        }

        self->outgoing_messages_.pop_front();
        if (!self->outgoing_messages_.empty()) {
            self->do_write();
        }
    });
}

void Session::leave_room() {
    if (joined_room_) {
        room_manager_->leave(room_id_, shared_from_this(), session_id_);
        joined_room_ = false;
    }
}
