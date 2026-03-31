#pragma once
/**
 * WebSocket server — uWebSockets, port 9090.
 * Runs in a dedicated std::thread.
 */

#include "protocol.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <cstdint>

struct us_listen_socket_t;  // Forward-declare in global namespace (uSockets type)

namespace uWS {
    class Loop;
    template<bool, bool, class> class WebSocket;
}

namespace ws {

class Server {
public:
    Server();
    ~Server();

    void start(int port = 9090);
    void stop();
    bool is_running() const { return running_.load(); }
    bool has_client() const { return client_connected_.load(); }

    /**
     * Queue a CONTROLLER_STATE message to send to the connected client.
     * payload must be exactly CONTROLLER_STATE_SIZE (64) bytes.
     * Thread-safe — called from the render thread.
     */
    void send_controller_state(const uint8_t* payload);

    /**
     * Queue a HAND_STATE message to send to the connected client.
     * payload must be exactly HAND_STATE_SIZE (729) bytes.
     * Thread-safe — called from the render thread.
     */
    void send_hand_state(const uint8_t* payload);

    /**
     * Haptic intensities written by the WS recv handler, read by the render thread.
     */
    std::atomic<float> haptic_left{0.0f};
    std::atomic<float> haptic_right{0.0f};
    std::atomic<float> haptic_left_duration{0.0f};
    std::atomic<float> haptic_right_duration{0.0f};

private:
    void run(int port);

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> client_connected_{false};

    // uWebSockets listen socket for shutdown
    ::us_listen_socket_t* listen_socket_ = nullptr;

    // Event loop handle for cross-thread defer()
    uWS::Loop* loop_ = nullptr;

    // Active WebSocket connection (only accessed from WS thread + defer callbacks)
    uWS::WebSocket<false, true, bool>* active_ws_ = nullptr;

    // Controller state staging buffer (written from render thread, read from WS thread)
    std::mutex controller_mutex_;
    uint8_t controller_buf_[protocol::CONTROLLER_STATE_SIZE] = {};
    std::atomic<bool> controller_dirty_{false};
};

}  // namespace ws
