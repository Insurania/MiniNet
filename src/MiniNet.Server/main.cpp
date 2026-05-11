#include "mininet/ping.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    std::uint16_t port = 40000;
    if (argc >= 2) {
        port = static_cast<std::uint16_t>(std::stoi(argv[1]));
    }

    try {
        mininet::PingServer server(port);
        std::cout << "MiniNet server listening on 127.0.0.1:" << server.port() << '\n';

        while (true) {
            const auto result = server.handle_next(std::chrono::milliseconds(1000));
            if (!result.received) {
                continue;
            }

            if (result.responded) {
                std::cout << "Pong sequence=" << *result.sequence << '\n';
            } else {
                std::cout << "Dropped packet reason=" << mininet::to_string(result.reason) << '\n';
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "Server error: " << error.what() << '\n';
        return 1;
    }
}
