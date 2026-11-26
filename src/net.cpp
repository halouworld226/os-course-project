#include "net.hpp"
#include "framing.hpp"

#include <cerrno>
#include <cstdio>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace {

#ifdef _WIN32
using Socket = SOCKET;
using AddrSize = int;
constexpr auto bad_socket = INVALID_SOCKET;
void close_socket(Socket socket) { closesocket(socket); }
bool interrupted() { return WSAGetLastError() == WSAEINTR; }
bool prevent_inheritance(Socket socket) {
    return SetHandleInformation(reinterpret_cast<HANDLE>(socket), HANDLE_FLAG_INHERIT, 0) != 0;
}
#else
using Socket = int;
using AddrSize = socklen_t;
constexpr auto bad_socket = -1;
void close_socket(Socket socket) { close(socket); }
bool interrupted() { return errno == EINTR; }
bool prevent_inheritance(Socket socket) {
    const auto flags = fcntl(socket, F_GETFD);
    return flags != -1 && fcntl(socket, F_SETFD, flags | FD_CLOEXEC) == 0;
}
#endif

std::size_t receive(Socket socket, std::uint8_t* data, std::size_t count) {
    for (;;) {
        const auto n = recv(socket, reinterpret_cast<char*>(data), static_cast<int>(count), 0);
        if (n >= 0) return static_cast<std::size_t>(n);
        if (!interrupted()) throw std::runtime_error("socket receive failed or timed out");
    }
}

std::size_t transmit(Socket socket, const std::uint8_t* data, std::size_t count) {
    for (;;) {
#ifdef MSG_NOSIGNAL
        constexpr auto flags = MSG_NOSIGNAL;
#else
        constexpr auto flags = 0;
#endif
        const auto n = send(socket, reinterpret_cast<const char*>(data), static_cast<int>(count), flags);
        if (n >= 0) return static_cast<std::size_t>(n);
        if (!interrupted()) throw std::runtime_error("socket send failed or timed out");
    }
}

} // namespace

TcpListener::TcpListener(unsigned requested_port) {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("Winsock startup failed");
#endif
    const auto socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle != bad_socket) {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<unsigned short>(requested_port));
        AddrSize size = sizeof(address);
        if (prevent_inheritance(socket_handle) && bind(socket_handle, reinterpret_cast<const sockaddr*>(&address), size) == 0 &&
            listen(socket_handle, 8) == 0 &&
            getsockname(socket_handle, reinterpret_cast<sockaddr*>(&address), &size) == 0) {
            handle_ = static_cast<std::uintptr_t>(socket_handle);
            port_ = ntohs(address.sin_port);
            return;
        }
        close_socket(socket_handle);
    }
#ifdef _WIN32
    WSACleanup();
#endif
    throw std::runtime_error("cannot listen on the requested loopback port");
}

TcpListener::~TcpListener() {
    close_socket(static_cast<Socket>(handle_));
#ifdef _WIN32
    WSACleanup();
#endif
}

TcpStream TcpListener::accept_client() const {
    Socket client;
    do { client = accept(static_cast<Socket>(handle_), nullptr, nullptr); }
    while (client == bad_socket && interrupted());
    if (client == bad_socket) throw std::runtime_error("cannot accept client");
    if (!prevent_inheritance(client)) {
        close_socket(client);
        throw std::runtime_error("cannot secure accepted socket");
    }
#ifdef _WIN32
    const DWORD timeout = 30000;
    const auto receive_timeout = setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    const auto send_timeout = setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    const timeval timeout{30, 0};
    const auto receive_timeout = setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    const auto send_timeout = setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
    if (receive_timeout != 0 || send_timeout != 0) {
        close_socket(client);
        throw std::runtime_error("cannot configure client socket timeouts");
    }
    return TcpStream(static_cast<std::uintptr_t>(client));
}

TcpStream::TcpStream(TcpStream&& other) noexcept
    : handle_(std::exchange(other.handle_, static_cast<std::uintptr_t>(bad_socket))) {}

TcpStream& TcpStream::operator=(TcpStream&& other) noexcept {
    if (this != &other) {
        if (handle_ != static_cast<std::uintptr_t>(bad_socket)) close_socket(static_cast<Socket>(handle_));
        handle_ = std::exchange(other.handle_, static_cast<std::uintptr_t>(bad_socket));
    }
    return *this;
}

TcpStream::~TcpStream() {
    if (handle_ != static_cast<std::uintptr_t>(bad_socket)) close_socket(static_cast<Socket>(handle_));
}

std::size_t TcpStream::read(std::uint8_t* bytes, std::size_t size) {
    return receive(static_cast<Socket>(handle_), bytes, size);
}

std::size_t TcpStream::write(const std::uint8_t* bytes, std::size_t size) {
    return transmit(static_cast<Socket>(handle_), bytes, size);
}

StdioStream::StdioStream() {
#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) == -1 || _setmode(_fileno(stdout), _O_BINARY) == -1) {
        throw std::runtime_error("cannot set binary standard I/O");
    }
#endif
}

std::size_t StdioStream::read(std::uint8_t* bytes, std::size_t size) { return std::fread(bytes, 1, size, stdin); }

bool StdioStream::has_next() const {
    const auto byte = std::fgetc(stdin);
    if (byte == EOF) return false;
    if (std::ungetc(byte, stdin) == EOF) throw std::runtime_error("cannot inspect session input");
    return true;
}

std::size_t StdioStream::write(const std::uint8_t* bytes, std::size_t size) { return std::fwrite(bytes, 1, size, stdout); }

void StdioStream::flush() const {
    if (std::fflush(stdout) != 0) throw std::runtime_error("cannot flush standard output");
}

std::string RequestStream::read_line() {
    std::string line;
    for (unsigned i = 0; i < 128; ++i) {
        std::uint8_t byte;
        if (!read(&byte, 1)) throw std::runtime_error("request line missing");
        if (byte == '\n') return line;
        line += static_cast<char>(byte);
    }
    throw std::runtime_error("request line too long");
}

Bytes RequestStream::read_payload() {
    Bytes payload;
    if (!read_frame([this](auto* bytes, auto size) { return read(bytes, size); }, payload)) {
        throw std::runtime_error("request body missing");
    }
    return payload;
}

void RequestStream::write_line(const std::string& line) {
    auto send = [this](auto* bytes, auto size) { return write(bytes, size); };
    write_exact(send, reinterpret_cast<const std::uint8_t*>(line.data()), line.size());
}

void RequestStream::write_payload(const Bytes& data) {
    write_frame([this](auto* bytes, auto size) { return write(bytes, size); }, data);
}
