#pragma once
#include <string>

namespace rta::net {

// HTTP Host/Origin boundaries need an authority parser, not prefix matching.
// This product's control server binds IPv4; IPv6 literals are deliberately not
// accepted here. Credentials, escapes and path delimiters never belong to Host.
struct HttpAuthority {
    bool valid = false;
    std::string host;
    unsigned port = 0;
};
inline HttpAuthority parse_http_authority(const std::string& value, unsigned defaultPort) {
    HttpAuthority result;
    if (value.empty() || value.size() > 512 || !defaultPort || defaultPort > 65535) return result;
    const size_t colon = value.find(':');
    const std::string host = value.substr(0, colon);
    if (host.empty()) return result;
    for (unsigned char c : host) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')) return result;
        result.host += (c >= 'A' && c <= 'Z') ? char(c + ('a' - 'A')) : char(c);
    }
    result.port = defaultPort;
    if (colon != std::string::npos) {
        const auto port = value.substr(colon + 1);
        if (port.empty() || port.size() > 5) return result;
        unsigned parsed = 0;
        for (unsigned char c : port) {
            if (c < '0' || c > '9') return result;
            parsed = parsed * 10 + (c - '0');
        }
        if (!parsed || parsed > 65535) return result;
        result.port = parsed;
    }
    result.valid = true;
    return result;
}

} // namespace rta::net
