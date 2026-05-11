#include "mininet/udp_socket.hpp"

#include <array>
#include <stdexcept>

#ifdef _WIN32
#else
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace mininet {

namespace {

#ifdef _WIN32
class WsaSession {
public:
    WsaSession()
    {
        WSADATA data{};
        const auto result = WSAStartup(MAKEWORD(2, 2), &data);
        if (result != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
    }

    ~WsaSession()
    {
        WSACleanup();
    }
};

WsaSession& wsa_session()
{
    // Windows 使用 socket 前必须先 WSAStartup。
    // 用 static 对象保证进程内只初始化一次，避免每个 UdpSocket 重复初始化。
    static WsaSession session;
    return session;
}

std::runtime_error socket_error(const char* action)
{
    return std::runtime_error(std::string(action) + " failed with WSA error " + std::to_string(WSAGetLastError()));
}
#else
std::runtime_error socket_error(const char* action)
{
    return std::runtime_error(std::string(action) + " failed: " + std::strerror(errno));
}
#endif

sockaddr_in to_sockaddr(const UdpEndpoint& endpoint)
{
    // sockaddr_in 是系统 socket API 使用的 IPv4 地址结构。
    // MiniNet 暂时只支持 IPv4，后续需要 IPv6 时再扩展 endpoint 表示。
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(endpoint.port);

    if (inet_pton(AF_INET, endpoint.address.c_str(), &address.sin_addr) != 1) {
        throw std::invalid_argument("invalid IPv4 address: " + endpoint.address);
    }

    return address;
}

UdpEndpoint from_sockaddr(const sockaddr_in& address)
{
    std::array<char, INET_ADDRSTRLEN> buffer{};
    auto ipv4_address = address.sin_addr;
    const char* converted = inet_ntop(AF_INET, &ipv4_address, buffer.data(), static_cast<socklen_t>(buffer.size()));
    if (converted == nullptr) {
        throw socket_error("inet_ntop");
    }

    return {std::string(converted), ntohs(address.sin_port)};
}

} // namespace

UdpSocket::UdpSocket() = default;

UdpSocket::UdpSocket(NativeSocket socket)
    : socket_(socket)
{
}

UdpSocket::~UdpSocket()
{
    // RAII：UdpSocket 生命周期结束时自动关闭底层系统 socket。
    if (socket_ != kInvalidSocket) {
#ifdef _WIN32
        closesocket(socket_);
#else
        close(socket_);
#endif
    }
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept
    : socket_(other.socket_)
{
    other.socket_ = kInvalidSocket;
}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept
{
    if (this == &other) {
        return *this;
    }

    if (socket_ != kInvalidSocket) {
#ifdef _WIN32
        closesocket(socket_);
#else
        close(socket_);
#endif
    }

    socket_ = other.socket_;
    other.socket_ = kInvalidSocket;
    return *this;
}

UdpSocket UdpSocket::open()
{
#ifdef _WIN32
    (void)wsa_session();
#endif

    const auto socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == kInvalidSocket) {
        throw socket_error("socket");
    }

    return UdpSocket(socket);
}

UdpSocket UdpSocket::bind(std::uint16_t port)
{
    auto udp_socket = open();

    // 当前学习阶段只绑定 127.0.0.1，避免暴露到局域网或公网。
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = htons(port);

    if (::bind(udp_socket.socket_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        throw socket_error("bind");
    }

    return udp_socket;
}

std::uint16_t UdpSocket::local_port() const
{
    sockaddr_in local{};
    socklen_t length = sizeof(local);
    if (::getsockname(socket_, reinterpret_cast<sockaddr*>(&local), &length) != 0) {
        throw socket_error("getsockname");
    }

    return ntohs(local.sin_port);
}

void UdpSocket::send_to(ByteView bytes, const UdpEndpoint& endpoint) const
{
    const auto address = to_sockaddr(endpoint);
    const auto sent = ::sendto(socket_,
                               reinterpret_cast<const char*>(bytes.data()),
                               static_cast<int>(bytes.size()),
                               0,
                               reinterpret_cast<const sockaddr*>(&address),
                               sizeof(address));

    if (sent < 0 || static_cast<std::size_t>(sent) != bytes.size()) {
        throw socket_error("sendto");
    }
}

std::optional<UdpDatagram> UdpSocket::receive_from(std::chrono::milliseconds timeout) const
{
    // select 用来给阻塞式 socket 加超时。
    // 没有它的话，recvfrom 可能一直卡住，测试和 demo 都不好退出。
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(socket_, &read_set);

    timeval wait{};
    wait.tv_sec = static_cast<long>(timeout.count() / 1000);
    wait.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);

    const auto ready = ::select(static_cast<int>(socket_ + 1), &read_set, nullptr, nullptr, &wait);
    if (ready < 0) {
        throw socket_error("select");
    }

    if (ready == 0) {
        return std::nullopt;
    }

    // 1500 接近常见以太网 MTU。当前 Ping/Pong 很小，后续做分片前先限制在小包内。
    std::vector<std::uint8_t> buffer(1500);
    sockaddr_in sender{};
    socklen_t sender_length = sizeof(sender);
    const auto received = ::recvfrom(socket_,
                                     reinterpret_cast<char*>(buffer.data()),
                                     static_cast<int>(buffer.size()),
                                     0,
                                     reinterpret_cast<sockaddr*>(&sender),
                                     &sender_length);

    if (received < 0) {
        throw socket_error("recvfrom");
    }

    buffer.resize(static_cast<std::size_t>(received));
    return UdpDatagram{std::move(buffer), from_sockaddr(sender)};
}

} // namespace mininet
