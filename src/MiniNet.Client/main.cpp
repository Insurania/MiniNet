#include "mininet/ping.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    mininet::UdpEndpoint server{"127.0.0.1", 40000};
    if (argc >= 2) {
        server.address = argv[1];
    }
    if (argc >= 3) {
        server.port = static_cast<std::uint16_t>(std::stoi(argv[2]));
    }

    try {
        mininet::PingClient client;

        for (int i = 0; i < 5; ++i) {
            const auto result = client.ping(server, std::chrono::milliseconds(1000));
            if (result.received) {
                std::cout << "Received Pong sequence=" << result.sequence
                          << " rtt_ms=" << result.rtt.count() << '\n';
            } else {
                std::cout << "Ping timeout or invalid Pong\n";
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "Client error: " << error.what() << '\n';
        return 1;
    }
}
