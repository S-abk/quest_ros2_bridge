#pragma once
/**
 * WebSocket server stub — uWebSockets, port 9090.
 * Runs in a dedicated std::thread.
 */

#include <atomic>
#include <thread>

struct us_listen_socket_t;  // Forward-declare in global namespace (uSockets type)

namespace ws {

class Server {
public:
    Server();
    ~Server();

    void start(int port = 9090);
    void stop();
    bool is_running() const { return running_.load(); }
    bool has_client() const { return client_connected_.load(); }

private:
    void run(int port);

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> client_connected_{false};

    // uWebSockets listen socket for shutdown
    ::us_listen_socket_t* listen_socket_ = nullptr;
};

}  // namespace ws
