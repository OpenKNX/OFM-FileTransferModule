/**
 * @file        WebConsole.h
 * @brief       Minimal RFC6455 WebSocket client for `ftc -i <ip> con` (interface webconsole transport)
 * @details     Connects to ws://<ip>:80/console (text frames: log lines out / commands in). Host-only, no
 *              external deps: TCP + HTTP upgrade + client-masked frames + ping/pong. The handshake sends a
 *              fixed key and only checks for "101" (Sec-WebSocket-Accept is not verified — we are the only
 *              party). Display + input reuse ConsoleUi; this is just the transport.
 * @date        2026-08-03
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "../core/Discovery.h" // ftc::detail::socket_t · sockValid · sockClose + platform socket headers
#ifndef _WIN32
    #include <fcntl.h>
#endif

namespace ftc
{

class WebConsole
{
  public:
    /**
     * @brief Connect to ws://ip:port/path; true iff the server answered "101 Switching Protocols".
     * @param err set to a short reason on failure.
     */
    bool connect(const std::string& ip, uint16_t port, const std::string& path, std::string& err)
    {
        _sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (!detail::sockValid(_sock))
        {
            err = "socket()";
            return false;
        }
        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_port = htons(port);
        dst.sin_addr.s_addr = inet_addr(ip.c_str());
        if (dst.sin_addr.s_addr == INADDR_NONE)
        {
            err = "bad ip";
            close();
            return false;
        }
        setNonBlocking(); // non-blocking connect so a filtered host cannot hang us
        if (::connect(_sock, (sockaddr*)&dst, sizeof(dst)) != 0)
        {
            fd_set wf;
            FD_ZERO(&wf);
            FD_SET(_sock, &wf);
            timeval tv{2, 0};
            if (::select((int)_sock + 1, nullptr, &wf, nullptr, &tv) <= 0)
            {
                err = "connect timeout";
                close();
                return false;
            }
            int soErr = 0;
            socklen_t sl = sizeof(soErr);
            ::getsockopt(_sock, SOL_SOCKET, SO_ERROR, (char*)&soErr, &sl);
            if (soErr != 0)
            {
                err = "connect refused";
                close();
                return false;
            }
        }
        std::string req = "GET " + path + " HTTP/1.1\r\n"
                                          "Host: " +
                          ip + "\r\n"
                               "Upgrade: websocket\r\n"
                               "Connection: Upgrade\r\n"
                               "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                               "Sec-WebSocket-Version: 13\r\n\r\n";
        if (::send(_sock, req.data(), (int)req.size(), 0) != (int)req.size())
        {
            err = "send handshake";
            close();
            return false;
        }
        std::string resp;
        char b[512];
        for (int i = 0; i < 100 && resp.find("\r\n\r\n") == std::string::npos; ++i)
        {
            fd_set rf;
            FD_ZERO(&rf);
            FD_SET(_sock, &rf);
            timeval tv{0, 20000};
            if (::select((int)_sock + 1, &rf, nullptr, nullptr, &tv) > 0)
            {
                int n = (int)::recv(_sock, b, sizeof(b), 0);
                if (n > 0) resp.append(b, n);
                else if (n == 0)
                    break;
            }
        }
        if (resp.find(" 101") == std::string::npos)
        {
            err = resp.empty() ? "no response (no webserver?)" : "no websocket upgrade (no webconsole?)";
            close();
            return false;
        }
        const size_t he = resp.find("\r\n\r\n"); // bytes after the headers are already the first WS frame(s)
        if (he != std::string::npos && he + 4 < resp.size()) _rx.append(resp.substr(he + 4));
        _connected = true;
        return true;
    }

    bool connected() const { return _connected; }
    uint32_t bytesIn() const { return _bytesIn; }
    uint32_t bytesOut() const { return _bytesOut; }

    /**
     * @brief Non-blocking drain: parse frames, append complete TEXT lines to @p out, answer pings.
     * @details Returns false once the connection is closed.
     */
    bool poll(std::vector<std::string>& out)
    {
        if (!_connected) return false;
        char b[2048];
        for (;;)
        {
            int n = (int)::recv(_sock, b, sizeof(b), 0);
            if (n > 0)
            {
                _rx.append(b, (size_t)n);
                _bytesIn += (uint32_t)n;
                continue;
            }
            if (n == 0)
            {
                _connected = false;
                return false;
            } // peer closed
            break; // EWOULDBLOCK -> nothing more right now
        }
        size_t i = 0;
        while (i + 2 <= _rx.size())
        {
            const uint8_t b0 = (uint8_t)_rx[i], b1 = (uint8_t)_rx[i + 1];
            const uint8_t opcode = b0 & 0x0F;
            const bool masked = (b1 & 0x80) != 0;
            uint64_t len = b1 & 0x7F;
            size_t hdr = 2;
            if (len == 126)
            {
                if (i + 4 > _rx.size()) break;
                len = ((uint64_t)(uint8_t)_rx[i + 2] << 8) | (uint8_t)_rx[i + 3];
                hdr = 4;
            }
            else if (len == 127)
            {
                if (i + 10 > _rx.size()) break;
                len = 0;
                for (int k = 0; k < 8; ++k)
                    len = (len << 8) | (uint8_t)_rx[i + 2 + k];
                hdr = 10;
            }
            const size_t maskLen = masked ? 4 : 0;
            // Cap the peer-declared length before the incomplete-check: a crafted/garbled 8-byte length would
            // overflow `i + hdr + maskLen + len` on a 64-bit host (wrap past _rx.size() -> OOB read / bad_alloc),
            // and even a merely-huge non-wrapping length would grow _rx unboundedly. Console frames are tiny.
            if (len > (uint64_t)(1u << 20)) { _rx.clear(); break; }
            if (i + hdr + maskLen + len > _rx.size()) break; // frame incomplete -> wait for more
            const char* p = _rx.data() + i + hdr + maskLen;
            std::string payload;
            if (masked)
            {
                const uint8_t* mk = (const uint8_t*)(_rx.data() + i + hdr);
                for (uint64_t k = 0; k < len; ++k)
                    payload += (char)(p[k] ^ mk[k & 3]);
            }
            else
                payload.assign(p, (size_t)len);
            i += hdr + maskLen + (size_t)len;
            if (opcode == 0x1 || opcode == 0x0) pushLines(payload, out); // text / continuation
            else if (opcode == 0x9)
                sendFrame(0x0A, payload); // ping -> pong
            else if (opcode == 0x8)
            {
                _connected = false;
                _rx.erase(0, i);
                return false;
            } // close
        }
        _rx.erase(0, i);
        return true;
    }

    /**
     * @brief Send one command as a masked text frame (the webconsole runs it and echoes it back).
     */
    bool send(const std::string& text) { return sendFrame(0x01, text); }

    /**
     * @brief Close the socket (idempotent).
     */
    void close()
    {
        if (detail::sockValid(_sock)) detail::sockClose(_sock);
        _sock = (detail::socket_t)-1;
        _connected = false;
    }

  private:
    detail::socket_t _sock = (detail::socket_t)-1;
    bool _connected = false;
    std::string _rx;      // unparsed inbound bytes
    std::string _lineBuf; // partial (no-newline-yet) inbound text line
    uint32_t _bytesIn = 0, _bytesOut = 0;

    /**
     * @brief Split inbound text on '\n' into complete lines, buffering the trailing partial.
     */
    void pushLines(const std::string& s, std::vector<std::string>& out)
    {
        for (char ch : s)
        {
            if (ch == '\n')
            {
                out.push_back(_lineBuf);
                _lineBuf.clear();
            }
            else if (ch != '\r')
                _lineBuf += ch;
        }
    }

    /**
     * @brief Build + send one client-masked WS frame (client frames MUST be masked per RFC6455).
     */
    bool sendFrame(uint8_t opcode, const std::string& payload)
    {
        if (!detail::sockValid(_sock)) return false;
        std::string f;
        f += (char)(0x80 | opcode); // FIN + opcode
        const uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
        const size_t len = payload.size();
        if (len < 126) f += (char)(0x80 | (uint8_t)len);
        else if (len <= 0xFFFF)
        {
            f += (char)(0x80 | 126);
            f += (char)((len >> 8) & 0xFF);
            f += (char)(len & 0xFF);
        }
        else
        {
            f += (char)(0x80 | 127);
            for (int k = 7; k >= 0; --k)
                f += (char)((len >> (8 * k)) & 0xFF);
        }
        f.append((const char*)mask, 4);
        for (size_t k = 0; k < len; ++k)
            f += (char)(payload[k] ^ mask[k & 3]);
        const bool okv = (int)::send(_sock, f.data(), (int)f.size(), 0) == (int)f.size();
        if (okv) _bytesOut += (uint32_t)f.size();
        return okv;
    }

    /**
     * @brief Put the socket into non-blocking mode.
     */
    void setNonBlocking()
    {
#ifdef _WIN32
        u_long m = 1;
        ioctlsocket(_sock, FIONBIO, &m);
#else
        int fl = fcntl(_sock, F_GETFL, 0);
        fcntl(_sock, F_SETFL, fl | O_NONBLOCK);
#endif
    }
};

} // namespace ftc
