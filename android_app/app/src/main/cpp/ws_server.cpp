/**
 * WebSocket server — accepts connections, sends controller state,
 * receives haptic commands and camera frames.
 */

#include "ws_server.h"
#include "protocol.h"
#include "camera_renderer.h"

#include <libusockets.h>  // us_listen_socket_t, us_listen_socket_close
#include <App.h>          // uWebSockets
#include <Loop.h>         // uWS::Loop::defer
#include <android/log.h>

#include <cstring>
#include <memory>
#include <vector>

#define LOG_TAG "QuestBridge_WS"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace ws {

Server::Server() = default;

Server::~Server() {
    stop();
}

void Server::start(int port) {
    if (running_.load()) return;
    stop_requested_.store(false);
    thread_ = std::thread(&Server::run, this, port);
}

void Server::stop() {
    if (!running_.load()) return;
    stop_requested_.store(true);
    if (listen_socket_) {
        us_listen_socket_close(0, listen_socket_);
        listen_socket_ = nullptr;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

void Server::send_controller_state(const uint8_t* payload) {
    std::lock_guard<std::mutex> lock(controller_mutex_);
    std::memcpy(controller_buf_, payload, protocol::CONTROLLER_STATE_SIZE);
    controller_dirty_.store(true);

    // Defer the actual send to the WS event loop thread
    if (loop_) {
        loop_->defer([this]() {
            if (!active_ws_ || !controller_dirty_.load()) return;
            std::lock_guard<std::mutex> lock(controller_mutex_);
            controller_dirty_.store(false);

            // Build the full message: header + payload
            uint8_t msg[protocol::HEADER_SIZE + protocol::CONTROLLER_STATE_SIZE];
            protocol::write_header(msg, protocol::MsgType::CONTROLLER_STATE,
                                   protocol::CONTROLLER_STATE_SIZE);
            std::memcpy(msg + protocol::HEADER_SIZE, controller_buf_,
                        protocol::CONTROLLER_STATE_SIZE);

            active_ws_->send(
                std::string_view(reinterpret_cast<char*>(msg), sizeof(msg)),
                uWS::BINARY);
        });
    }
}

void Server::send_hand_state(const uint8_t* payload) {
    if (!loop_ || !active_ws_) return;

    // Copy payload into a vector for the deferred callback
    auto buf = std::make_shared<std::vector<uint8_t>>(
        protocol::HEADER_SIZE + protocol::HAND_STATE_SIZE);
    protocol::write_header(buf->data(), protocol::MsgType::HAND_STATE,
                           protocol::HAND_STATE_SIZE);
    std::memcpy(buf->data() + protocol::HEADER_SIZE, payload,
                protocol::HAND_STATE_SIZE);

    loop_->defer([this, buf]() {
        if (!active_ws_) return;
        active_ws_->send(
            std::string_view(reinterpret_cast<char*>(buf->data()), buf->size()),
            uWS::BINARY);
    });
}

void Server::run(int port) {
    running_.store(true);
    LOGI("WebSocket server starting on port %d", port);

    uWS::App app;

    app.ws<bool>("/*", {
        .compression = uWS::DISABLED,
        .maxPayloadLength = 64 * 1024,
        .idleTimeout = 0,
        .open = [this](auto* ws) {
            LOGI("Client connected");
            client_connected_.store(true);
            active_ws_ = ws;
        },
        .message = [this](auto* ws, std::string_view message, uWS::OpCode opCode) {
            if (opCode != uWS::BINARY || message.size() < protocol::HEADER_SIZE) return;

            protocol::MsgType type;
            uint32_t payload_len;
            protocol::read_header(
                reinterpret_cast<const uint8_t*>(message.data()), type, payload_len);

            const uint8_t* payload =
                reinterpret_cast<const uint8_t*>(message.data()) + protocol::HEADER_SIZE;

            if (type == protocol::MsgType::HAPTIC_CMD &&
                payload_len == protocol::HAPTIC_CMD_SIZE) {
                uint8_t hand = payload[0];
                float intensity = protocol::be_to_float(payload + 1);
                float duration = protocol::be_to_float(payload + 5);
                if (hand == 0) {
                    haptic_left.store(intensity);
                    haptic_left_duration.store(duration);
                } else {
                    haptic_right.store(intensity);
                    haptic_right_duration.store(duration);
                }
            }
            if (type == protocol::MsgType::CAMERA_FRAME && payload_len > 0) {
                camera_renderer::update_frame(payload, payload_len);
            }
        },
        .close = [this](auto* ws, int code, std::string_view message) {
            LOGI("Client disconnected (code=%d)", code);
            client_connected_.store(false);
            active_ws_ = nullptr;
        }
    });

    app.listen(port, [this, port](::us_listen_socket_t* ls) {
        if (ls) {
            listen_socket_ = ls;
            LOGI("Listening on port %d", port);
        } else {
            LOGW("Failed to listen on port %d", port);
        }
    });

    // Store the event loop handle for cross-thread defer()
    loop_ = uWS::Loop::get();

    app.run();

    // app.run() has returned
    loop_ = nullptr;
    active_ws_ = nullptr;
    running_.store(false);
    listen_socket_ = nullptr;
    LOGI("WebSocket server stopped");
}

}  // namespace ws
