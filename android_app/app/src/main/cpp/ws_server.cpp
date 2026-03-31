/**
 * WebSocket server stub — accepts connections, echoes back binary frames.
 * No protocol handling yet (Commit 1b scaffold).
 */

#include "ws_server.h"
#include "protocol.h"

#include <libusockets.h>  // us_listen_socket_t, us_listen_socket_close
#include <App.h>          // uWebSockets
#include <android/log.h>

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
    // Close the listen socket to break out of the event loop
    if (listen_socket_) {
        us_listen_socket_close(0, listen_socket_);
        listen_socket_ = nullptr;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

void Server::run(int port) {
    running_.store(true);
    LOGI("WebSocket server starting on port %d", port);

    // listen_socket_ is set inside the listen callback (called synchronously
    // before run), so stop() on another thread can close it to break the loop.
    uWS::App()
        .ws<bool>("/*", {
            .compression = uWS::DISABLED,
            .maxPayloadLength = 64 * 1024,
            .open = [this](auto* ws) {
                LOGI("Client connected");
                client_connected_.store(true);
            },
            .message = [](auto* ws, std::string_view message, uWS::OpCode opCode) {
                // Stub: echo back binary frames for now
                if (opCode == uWS::BINARY) {
                    ws->send(message, uWS::BINARY);
                }
            },
            .close = [this](auto* ws, int code, std::string_view message) {
                LOGI("Client disconnected (code=%d)", code);
                client_connected_.store(false);
            }
        })
        .listen(port, [this, port](::us_listen_socket_t* ls) {
            if (ls) {
                listen_socket_ = ls;
                LOGI("Listening on port %d", port);
            } else {
                LOGW("Failed to listen on port %d", port);
            }
        })
        .run();

    // app.run() has returned — we're shutting down
    running_.store(false);
    listen_socket_ = nullptr;
    LOGI("WebSocket server stopped");
}

}  // namespace ws
