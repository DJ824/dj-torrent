// Minimal HTTP helper for tracker and web seed requests.
#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct HttpUrl {
    std::string scheme;
    bool use_tls{};
    std::string host;
    std::string port_str;
    uint16_t port{};
    std::string path;
};

struct HttpResponse {
    std::string status_line;
    int status_code{};
    std::string headers;
    std::string body;
};

HttpUrl parse_http_url(const std::string& url);
std::string to_lower(std::string_view s);
std::string decode_chunked(std::string_view body);

HttpResponse http_get(const HttpUrl& url,
                      const std::string& path,
                      const std::vector<std::pair<std::string, std::string>>& headers = {},
                      std::size_t max_response_bytes = std::numeric_limits<std::size_t>::max());
