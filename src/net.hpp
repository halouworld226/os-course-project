#pragma once

#include "image.hpp"

#include <cstdint>
#include <string>

class RequestStream {
public:
    virtual ~RequestStream() = default;
    std::string read_line();
    Bytes read_payload();
    void write_line(const std::string& line);
    void write_payload(const Bytes& data);

private:
    virtual std::size_t read(std::uint8_t* bytes, std::size_t size) = 0;
    virtual std::size_t write(const std::uint8_t* bytes, std::size_t size) = 0;
};

class TcpStream : public RequestStream {
public:
    explicit TcpStream(std::uintptr_t handle) : handle_(handle) {}
    ~TcpStream() override;
    TcpStream(const TcpStream&) = delete;
    TcpStream& operator=(const TcpStream&) = delete;
    TcpStream(TcpStream&& other) noexcept;
    TcpStream& operator=(TcpStream&& other) noexcept;

private:
    std::size_t read(std::uint8_t* bytes, std::size_t size) override;
    std::size_t write(const std::uint8_t* bytes, std::size_t size) override;
    std::uintptr_t handle_;
};

class StdioStream : public RequestStream {
public:
    StdioStream();
    bool has_next() const;
    void flush() const;

private:
    std::size_t read(std::uint8_t* bytes, std::size_t size) override;
    std::size_t write(const std::uint8_t* bytes, std::size_t size) override;
};

class TcpListener {
public:
    explicit TcpListener(unsigned requested_port);
    ~TcpListener();
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    unsigned port() const { return port_; }
    TcpStream accept_client() const;

private:
    std::uintptr_t handle_;
    unsigned port_;
};
