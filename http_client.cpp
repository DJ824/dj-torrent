#include "http_client.h"

#include <arpa/inet.h>
#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstring>
#include <netdb.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>

int connect_socket(const std::string& host, const std::string& port, int timeout_ms) {
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    addrinfo* res = nullptr;
    int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
    if (rc != 0) {
        throw std::runtime_error("getaddrinfo failed: " + std::string(gai_strerror(rc)));
    }

    int fd = -1;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (timeout_ms <= 0) {
            if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
                break;
            }
            close(fd);
            fd = -1;
            continue;
        }

        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) {
            fcntl(fd, F_SETFL, flags);
            break;
        }
        if (rc < 0 && errno == EINPROGRESS) {
            pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLOUT;
            int pres = ::poll(&pfd, 1, timeout_ms);
            if (pres > 0 && (pfd.revents & POLLOUT)) {
                int err = 0;
                socklen_t len = sizeof(err);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) {
                    fcntl(fd, F_SETFL, flags);
                    break;
                }
            }
        }

        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        throw std::runtime_error("Failed to connect to host");
    }
    return fd;
}

struct Connection {
    int fd{-1};
    SSL_CTX* ctx{nullptr};
    SSL* ssl{nullptr};
    bool tls{false};

    Connection() = default;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    Connection(Connection&& other) noexcept
        : fd(other.fd), ctx(other.ctx), ssl(other.ssl), tls(other.tls) {
        other.fd = -1;
        other.ctx = nullptr;
        other.ssl = nullptr;
        other.tls = false;
    }

    Connection& operator=(Connection&& other) noexcept {
        if (this != &other) {
            this->~Connection();
            fd = other.fd;
            ctx = other.ctx;
            ssl = other.ssl;
            tls = other.tls;
            other.fd = -1;
            other.ctx = nullptr;
            other.ssl = nullptr;
            other.tls = false;
        }
        return *this;
    }

    ~Connection() {
        if (ssl) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
        }
        if (ctx) SSL_CTX_free(ctx);
        if (fd >= 0) close(fd);
    }

    void write_all(const std::string& data) {
        size_t sent = 0;
        while (sent < data.size()) {
            int n = 0;
            if (tls) {
                n = SSL_write(ssl, data.data() + sent, static_cast<int>(data.size() - sent));
                if (n <= 0) {
                    throw std::runtime_error("Failed to send TLS request");
                }
            } else {
                n = static_cast<int>(::send(fd, data.data() + sent, data.size() - sent, 0));
                if (n < 0) {
                    throw std::runtime_error("Failed to send request");
                }
            }
            sent += static_cast<size_t>(n);
        }
    }

    std::string read_all(std::size_t max_bytes) {
        std::string buf;
        char tmp[4096];
        for (;;) {
            int n = 0;
            if (tls) {
                n = SSL_read(ssl, tmp, sizeof(tmp));
                if (n <= 0) {
                    int err = SSL_get_error(ssl, n);
                    if (err == SSL_ERROR_ZERO_RETURN) break;
                    throw std::runtime_error("Failed to read TLS response");
                }
            } else {
                n = static_cast<int>(::recv(fd, tmp, sizeof(tmp), 0));
                if (n < 0) {
                    throw std::runtime_error("Failed to read response");
                }
                if (n == 0) break;
            }
            if (buf.size() + static_cast<std::size_t>(n) > max_bytes) {
                throw std::runtime_error("HTTP response exceeded safety limit");
            }
            buf.append(tmp, tmp + n);
        }
        return buf;
    }
};

Connection make_connection(const HttpUrl& url, int timeout_ms) {
    int fd = connect_socket(url.host, url.port_str, timeout_ms);

    if (timeout_ms > 0) {
        timeval tv{};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    if (!url.use_tls) {
        Connection c;
        c.fd = fd;
        c.tls = false;
        return c;
    }

    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        close(fd);
        throw std::runtime_error("Failed to create SSL context");
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    SSL_CTX_set_default_verify_paths(ctx);

    SSL* ssl = SSL_new(ctx);
    if (!ssl) {
        SSL_CTX_free(ctx);
        close(fd);
        throw std::runtime_error("Failed to create SSL object");
    }
    SSL_set_tlsext_host_name(ssl, url.host.c_str());
#ifdef SSL_set1_host
    SSL_set1_host(ssl, url.host.c_str());
#endif
    if (SSL_set_fd(ssl, fd) != 1) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);
        throw std::runtime_error("Failed to associate socket with SSL");
    }
    if (SSL_connect(ssl) != 1) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);
        throw std::runtime_error("TLS handshake failed");
    }
    long verify_res = SSL_get_verify_result(ssl);
    if (verify_res != X509_V_OK) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);
        throw std::runtime_error("TLS certificate verification failed");
    }

    Connection c;
    c.fd = fd;
    c.ctx = ctx;
    c.ssl = ssl;
    c.tls = true;
    return c;
}


HttpUrl parse_http_url(const std::string& url) {
    constexpr std::string_view http_prefix = "http://";
    constexpr std::string_view https_prefix = "https://";
    bool use_tls = false;
    std::string_view rest;
    if (url.size() >= https_prefix.size() &&
        std::equal(https_prefix.begin(), https_prefix.end(), url.begin(),
                   [](char a, char b) {
                       return std::tolower(static_cast<unsigned char>(a)) ==
                              std::tolower(static_cast<unsigned char>(b));
                   })) {
        use_tls = true;
        rest = std::string_view(url.c_str() + https_prefix.size(), url.size() - https_prefix.size());
    } else if (url.size() >= http_prefix.size() &&
               std::equal(http_prefix.begin(), http_prefix.end(), url.begin(),
                          [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          })) {
        rest = std::string_view(url.c_str() + http_prefix.size(), url.size() - http_prefix.size());
    } else {
        throw std::runtime_error("Only http:// or https:// URLs are supported right now");
    }

    auto slash_pos = rest.find('/');
    std::string_view host_port = slash_pos == std::string_view::npos ? rest : rest.substr(0, slash_pos);
    std::string path = slash_pos == std::string_view::npos ? "/" : std::string(rest.substr(slash_pos));

    auto colon_pos = host_port.find(':');
    std::string host;
    uint16_t port = use_tls ? 443 : 80;
    if (colon_pos == std::string_view::npos) {
        host = std::string(host_port);
    } else {
        host = std::string(host_port.substr(0, colon_pos));
        auto port_sv = host_port.substr(colon_pos + 1);
        int p = 0;
        auto [ptr, ec] = std::from_chars(port_sv.data(), port_sv.data() + port_sv.size(), p);
        if (ec != std::errc() || ptr != port_sv.data() + port_sv.size() || p <= 0 || p > 65535) {
            throw std::runtime_error("Invalid port in URL");
        }
        port = static_cast<uint16_t>(p);
    }
    if (host.empty()) {
        throw std::runtime_error("URL missing host");
    }
    HttpUrl out;
    out.scheme = use_tls ? "https" : "http";
    out.use_tls = use_tls;
    out.host = std::move(host);
    out.port_str = std::to_string(port);
    out.port = port;
    out.path = std::move(path);
    return out;
}

std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

std::string decode_chunked(std::string_view body) {
    size_t pos = 0;
    std::string out;
    while (true) {
        auto line_end = body.find("\r\n", pos);
        if (line_end == std::string_view::npos) {
            throw std::runtime_error("Malformed chunked body");
        }
        std::string_view line = body.substr(pos, line_end - pos);
        auto semicolon = line.find(';');
        if (semicolon != std::string_view::npos) line = line.substr(0, semicolon);
        size_t chunk_size = 0;
        auto [ptr, ec] = std::from_chars(line.data(), line.data() + line.size(), chunk_size, 16);
        if (ec != std::errc()) {
            throw std::runtime_error("Invalid chunk size");
        }
        pos = line_end + 2;
        if (body.size() < pos + chunk_size + 2) {
            throw std::runtime_error("Incomplete chunk data");
        }
        out.append(body.substr(pos, chunk_size));
        pos += chunk_size;
        if (body.substr(pos, 2) != "\r\n") {
            throw std::runtime_error("Missing CRLF after chunk");
        }
        pos += 2;
        if (chunk_size == 0) break;
    }
    return out;
}

HttpResponse http_get(const HttpUrl& url,
                      const std::string& path,
                      const std::vector<std::pair<std::string, std::string>>& headers,
                      std::size_t max_response_bytes,
                      int timeout_ms) {
    std::ostringstream req;
    req << "GET " << path << " HTTP/1.1\r\n";
    req << "Host: " << url.host;
    bool default_port = (!url.use_tls && url.port == 80) || (url.use_tls && url.port == 443);
    if (!default_port) req << ":" << url.port;
    req << "\r\n";
    req << "User-Agent: dj-torrent/0.1\r\n";
    for (const auto& h : headers) {
        req << h.first << ": " << h.second << "\r\n";
    }
    req << "Connection: close\r\n";
    req << "\r\n";

    Connection conn = make_connection(url, timeout_ms);
    conn.write_all(req.str());
    std::string raw = conn.read_all(max_response_bytes);

    auto header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        throw std::runtime_error("Malformed HTTP response");
    }
    std::string header_block = raw.substr(0, header_end);
    std::string body = raw.substr(header_end + 4);

    auto status_end = header_block.find("\r\n");
    if (status_end == std::string::npos) {
        throw std::runtime_error("Missing status line");
    }
    std::string status_line = header_block.substr(0, status_end);

    int status_code = 0;
    auto first_space = status_line.find(' ');
    if (first_space != std::string::npos) {
        auto second_space = status_line.find(' ', first_space + 1);
        auto code_sv = status_line.substr(first_space + 1,
                                          second_space == std::string::npos
                                              ? std::string::npos
                                              : second_space - first_space - 1);
        (void)std::from_chars(code_sv.data(), code_sv.data() + code_sv.size(), status_code);
    }

    std::string headers_lower = to_lower(header_block);
    if (headers_lower.find("transfer-encoding: chunked") != std::string::npos) {
        body = decode_chunked(body);
    }

    return HttpResponse{std::move(status_line), status_code, std::move(header_block), std::move(body)};
}

HttpResponse http_get(const HttpUrl& url,
                      const std::string& path,
                      const std::vector<std::pair<std::string, std::string>>& headers,
                      std::size_t max_response_bytes) {
    return http_get(url, path, headers, max_response_bytes, -1);
}
