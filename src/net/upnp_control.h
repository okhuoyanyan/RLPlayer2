// upnp_control.h — UPnP/DLNA 媒体投放控制面与格式取证（零 windows.h，跨平台）
//
// 契约要点（ROADMAP P5a）：
//   1) SOAP 动作：SetAVTransportURI、Play、Pause、Stop、Seek、GetTransportInfo、
//      GetPositionInfo、GetMediaInfo、GetProtocolInfo；纯 HTTP POST，解析响应；
//      错误必带 UPnPError code / description，绝不静默；
//   2) DIDL-Lite 元数据构造与解析（XML 转义、采样率/位深属性）；
//   3) 渲染器实际采样率取证：对照源格式与渲染器声明，产出 Match / Mismatch / Unknown 三态；
//      若不一致明确告警「渲染器可能重采样，非 bit-perfect」；
//   4) 本地 HTTP 供源 Range 语义与 token 防遍历逻辑（纯函数，可离线测）。
#pragma once
#include "net/http_client.h"
#include <string>
#include <vector>
#include <map>
#include <utility>
#include <sstream>
#include <cstdint>
#include <cstdlib>
#include <chrono>
#include <functional>
#include <algorithm>
#include <cstdio>

namespace rta::net {

// ---------------- XML 转义与解析基础 ----------------

inline std::string xml_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out += c; break;
        }
    }
    return out;
}

inline std::string xml_unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        if (s[i] == '&') {
            if (s.compare(i, 5, "&amp;") == 0)       { out += '&'; i += 5; }
            else if (s.compare(i, 4, "&lt;") == 0)   { out += '<'; i += 4; }
            else if (s.compare(i, 4, "&gt;") == 0)   { out += '>'; i += 4; }
            else if (s.compare(i, 6, "&quot;") == 0) { out += '"'; i += 6; }
            else if (s.compare(i, 6, "&apos;") == 0) { out += '\''; i += 6; }
            else { out += s[i++]; }
        } else {
            out += s[i++];
        }
    }
    return out;
}

// 提取 <tag>...</tag> 或 <ns:tag>...</ns:tag> 内文本
inline std::string xml_extract_tag(const std::string& xml, const std::string& tagName) {
    size_t pos = 0;
    while ((pos = xml.find('<', pos)) != std::string::npos) {
        size_t nameStart = pos + 1;
        if (nameStart < xml.size() && (xml[nameStart] == '/' || xml[nameStart] == '!' || xml[nameStart] == '?')) {
            pos++;
            continue;
        }
        size_t closeAngle = xml.find('>', nameStart);
        if (closeAngle == std::string::npos) break;
        bool selfClosing = (closeAngle > nameStart && xml[closeAngle - 1] == '/');
        size_t tagSpace = xml.find_first_of(" \t\r\n/", nameStart);
        size_t nameEnd = (tagSpace != std::string::npos && tagSpace < closeAngle)
                         ? tagSpace : (selfClosing ? closeAngle - 1 : closeAngle);
        std::string fullTag = xml.substr(nameStart, nameEnd - nameStart);
        size_t colon = fullTag.find(':');
        std::string localName = (colon != std::string::npos) ? fullTag.substr(colon + 1) : fullTag;
        if (localName == tagName) {
            if (selfClosing) return "";
            size_t searchClose = closeAngle + 1;
            while (true) {
                size_t closeStart = xml.find("</", searchClose);
                if (closeStart == std::string::npos) return "";
                size_t closeTagEnd = xml.find('>', closeStart + 2);
                if (closeTagEnd == std::string::npos) return "";
                std::string closeFull = xml.substr(closeStart + 2, closeTagEnd - closeStart - 2);
                size_t cColon = closeFull.find(':');
                std::string cLocal = (cColon != std::string::npos) ? closeFull.substr(cColon + 1) : closeFull;
                size_t fa = cLocal.find_first_not_of(" \t\r\n");
                size_t fb = cLocal.find_last_not_of(" \t\r\n");
                if (fa != std::string::npos && fb != std::string::npos)
                    cLocal = cLocal.substr(fa, fb - fa + 1);
                if (cLocal == tagName) {
                    return xml.substr(closeAngle + 1, closeStart - closeAngle - 1);
                }
                searchClose = closeStart + 2;
            }
        }
        pos = closeAngle + 1;
    }
    return "";
}

inline std::string xml_extract_attr(const std::string& tagText, const std::string& attrName) {
    std::string key = attrName + "=\"";
    size_t p = tagText.find(key);
    if (p == std::string::npos) {
        key = attrName + "='";
        p = tagText.find(key);
        if (p == std::string::npos) return "";
        p += key.size();
        size_t e = tagText.find('\'', p);
        if (e == std::string::npos) return "";
        return tagText.substr(p, e - p);
    }
    p += key.size();
    size_t e = tagText.find('\"', p);
    if (e == std::string::npos) return "";
    return tagText.substr(p, e - p);
}

// ---------------- URL 解析辅助 ----------------

inline std::string resolve_url(const std::string& baseUrl, const std::string& targetUrl) {
    if (targetUrl.empty()) return baseUrl;
    if (targetUrl.rfind("http://", 0) == 0 || targetUrl.rfind("https://", 0) == 0) return targetUrl;
    if (baseUrl.empty()) return targetUrl;

    if (targetUrl.front() == '/') {
        size_t schemeEnd = baseUrl.find("://");
        if (schemeEnd == std::string::npos) return targetUrl;
        size_t firstSlash = baseUrl.find('/', schemeEnd + 3);
        if (firstSlash == std::string::npos) return baseUrl + targetUrl;
        return baseUrl.substr(0, firstSlash) + targetUrl;
    }

    size_t lastSlash = baseUrl.find_last_of('/');
    if (lastSlash == std::string::npos) return baseUrl + "/" + targetUrl;
    size_t schemeEnd = baseUrl.find("://");
    if (schemeEnd != std::string::npos && lastSlash == schemeEnd + 2) {
        return baseUrl + "/" + targetUrl;
    }
    return baseUrl.substr(0, lastSlash + 1) + targetUrl;
}

// ---------------- DIDL-Lite 元数据模型 ----------------

struct UpnpDidlItem {
    std::string id = "0";
    std::string title;
    std::string creator;
    std::string album;
    std::string uri;
    std::string protocolInfo = "http-get:*:audio/wav:*";
    uint32_t    sampleRate = 0;
    int         bits = 0;
    int         channels = 0;
    double      durationSec = 0.0;
    // 2026-10-29：res@size。多数渲染器不看它也能播，但一部分（严格 DLNA 实现 / 需要预分配或
    // 按大小算 seek 的设备）会因为它缺失而拒绝或无法拖动。填的是实际供源字节数。
    int64_t     sizeBytes = 0;
};

inline std::string format_time_hh_mm_ss(double seconds) {
    if (seconds < 0.0) seconds = 0.0;
    int totalSec = (int)seconds;
    int h = totalSec / 3600;
    int m = (totalSec % 3600) / 60;
    int s = totalSec % 60;
    char b[32];
    std::snprintf(b, sizeof(b), "%02d:%02d:%02d", h, m, s);
    return b;
}

inline double parse_time_hh_mm_ss(const std::string& str) {
    int h = 0, m = 0, s = 0;
    double frac = 0.0;
    size_t c1 = str.find(':');
    if (c1 != std::string::npos) {
        h = std::atoi(str.substr(0, c1).c_str());
        size_t c2 = str.find(':', c1 + 1);
        if (c2 != std::string::npos) {
            m = std::atoi(str.substr(c1 + 1, c2 - c1 - 1).c_str());
            s = std::atoi(str.substr(c2 + 1).c_str());
            size_t dot = str.find('.', c2 + 1);
            if (dot != std::string::npos) {
                frac = std::atof(str.substr(dot).c_str());
            }
            return (double)(h * 3600 + m * 60 + s) + frac;
        }
    }
    return 0.0;
}

inline std::string build_didl_lite(const UpnpDidlItem& item) {
    std::ostringstream ss;
    ss << "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" "
          "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
          "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\">"
       << "<item id=\"" << xml_escape(item.id.empty() ? "0" : item.id) << "\" parentID=\"-1\" restricted=\"1\">";
    if (!item.title.empty()) {
        ss << "<dc:title>" << xml_escape(item.title) << "</dc:title>";
    } else {
        ss << "<dc:title>RLPlayer Cast</dc:title>";
    }
    if (!item.creator.empty()) {
        ss << "<dc:creator>" << xml_escape(item.creator) << "</dc:creator>";
    }
    if (!item.album.empty()) {
        ss << "<upnp:album>" << xml_escape(item.album) << "</upnp:album>";
    }
    ss << "<upnp:class>object.item.audioItem.musicTrack</upnp:class>";
    ss << "<res protocolInfo=\"" << xml_escape(item.protocolInfo) << "\"";
    if (item.sampleRate > 0) {
        ss << " sampleFrequency=\"" << item.sampleRate << "\"";
    }
    if (item.bits > 0) {
        ss << " bitsPerSample=\"" << item.bits << "\"";
    }
    if (item.channels > 0) {
        ss << " nrAudioChannels=\"" << item.channels << "\"";
    }
    if (item.durationSec > 0.0) {
        ss << " duration=\"" << format_time_hh_mm_ss(item.durationSec) << "\"";
    }
    if (item.sizeBytes > 0) {
        ss << " size=\"" << item.sizeBytes << "\"";
    }
    ss << ">" << xml_escape(item.uri) << "</res>";
    ss << "</item></DIDL-Lite>";
    return ss.str();
}

inline UpnpDidlItem parse_didl_lite(const std::string& didlXml) {
    UpnpDidlItem item;
    std::string xml = didlXml;
    if (xml.find("&lt;") != std::string::npos && xml.find("<DIDL-Lite") == std::string::npos) {
        xml = xml_unescape(xml);
    }
    item.title = xml_unescape(xml_extract_tag(xml, "title"));
    item.creator = xml_unescape(xml_extract_tag(xml, "creator"));
    item.album = xml_unescape(xml_extract_tag(xml, "album"));

    size_t resPos = xml.find("<res");
    if (resPos != std::string::npos) {
        size_t tagClose = xml.find('>', resPos);
        if (tagClose != std::string::npos) {
            std::string openTag = xml.substr(resPos, tagClose - resPos + 1);
            item.protocolInfo = xml_extract_attr(openTag, "protocolInfo");
            std::string sf = xml_extract_attr(openTag, "sampleFrequency");
            if (!sf.empty()) item.sampleRate = (uint32_t)std::strtoul(sf.c_str(), nullptr, 10);
            std::string bps = xml_extract_attr(openTag, "bitsPerSample");
            if (!bps.empty()) item.bits = std::atoi(bps.c_str());
            std::string ch = xml_extract_attr(openTag, "nrAudioChannels");
            if (!ch.empty()) item.channels = std::atoi(ch.c_str());
            std::string dur = xml_extract_attr(openTag, "duration");
            if (!dur.empty()) item.durationSec = parse_time_hh_mm_ss(dur);

            size_t endRes = xml.find("</res>", tagClose);
            if (endRes != std::string::npos) {
                item.uri = xml_unescape(xml.substr(tagClose + 1, endRes - tagClose - 1));
            }
        }
    }
    return item;
}

// ---------------- SOAP 错误与消息构造 ----------------

struct UpnpError {
    int         code = 0;
    std::string description;
    bool has_error() const { return code != 0 || !description.empty(); }
    std::string str() const {
        if (!has_error()) return "OK";
        return "UPnPError " + std::to_string(code) + " (" + description + ")";
    }
};

inline bool parse_upnp_error(const std::string& xml, UpnpError& err) {
    std::string codeStr = xml_extract_tag(xml, "errorCode");
    std::string descStr = xml_extract_tag(xml, "errorDescription");
    if (!codeStr.empty()) {
        err.code = std::atoi(codeStr.c_str());
        err.description = xml_unescape(descStr);
        return true;
    }
    std::string faultString = xml_extract_tag(xml, "faultstring");
    if (!faultString.empty()) {
        err.code = 500;
        err.description = xml_unescape(faultString);
        return true;
    }
    return false;
}

inline std::string build_soap_envelope(const std::string& serviceType,
                                       const std::string& action,
                                       const std::vector<std::pair<std::string, std::string>>& args) {
    std::ostringstream ss;
    ss << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
       << "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
          "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\r\n"
       << "  <s:Body>\r\n"
       << "    <u:" << action << " xmlns:u=\"" << serviceType << "\">\r\n";
    for (const auto& kv : args) {
        ss << "      <" << kv.first << ">" << kv.second << "</" << kv.first << ">\r\n";
    }
    ss << "    </u:" << action << ">\r\n"
       << "  </s:Body>\r\n"
       << "</s:Envelope>\r\n";
    return ss.str();
}

constexpr const char* kServiceAVTransport = "urn:schemas-upnp-org:service:AVTransport:1";
constexpr const char* kServiceConnectionManager = "urn:schemas-upnp-org:service:ConnectionManager:1";

inline std::string build_set_av_transport_uri_soap(uint32_t instanceId,
                                                   const std::string& currentUri,
                                                   const std::string& metadataXml) {
    return build_soap_envelope(kServiceAVTransport, "SetAVTransportURI", {
        {"InstanceID", std::to_string(instanceId)},
        {"CurrentURI", xml_escape(currentUri)},
        {"CurrentURIMetaData", xml_escape(metadataXml)}
    });
}

inline std::string build_play_soap(uint32_t instanceId, const std::string& speed = "1") {
    return build_soap_envelope(kServiceAVTransport, "Play", {
        {"InstanceID", std::to_string(instanceId)},
        {"Speed", xml_escape(speed)}
    });
}

inline std::string build_pause_soap(uint32_t instanceId) {
    return build_soap_envelope(kServiceAVTransport, "Pause", {
        {"InstanceID", std::to_string(instanceId)}
    });
}

inline std::string build_stop_soap(uint32_t instanceId) {
    return build_soap_envelope(kServiceAVTransport, "Stop", {
        {"InstanceID", std::to_string(instanceId)}
    });
}

inline std::string build_seek_soap(uint32_t instanceId,
                                   const std::string& unit,
                                   const std::string& target) {
    return build_soap_envelope(kServiceAVTransport, "Seek", {
        {"InstanceID", std::to_string(instanceId)},
        {"Unit", xml_escape(unit)},
        {"Target", xml_escape(target)}
    });
}

inline std::string build_get_transport_info_soap(uint32_t instanceId) {
    return build_soap_envelope(kServiceAVTransport, "GetTransportInfo", {
        {"InstanceID", std::to_string(instanceId)}
    });
}

inline std::string build_get_position_info_soap(uint32_t instanceId) {
    return build_soap_envelope(kServiceAVTransport, "GetPositionInfo", {
        {"InstanceID", std::to_string(instanceId)}
    });
}

inline std::string build_get_media_info_soap(uint32_t instanceId) {
    return build_soap_envelope(kServiceAVTransport, "GetMediaInfo", {
        {"InstanceID", std::to_string(instanceId)}
    });
}

inline std::string build_get_protocol_info_soap() {
    return build_soap_envelope(kServiceConnectionManager, "GetProtocolInfo", {});
}

// ---------------- SOAP 响应模型与解析 ----------------

struct UpnpActionResult {
    bool        ok = false;
    int         httpStatus = 0;
    std::string action;
    std::string rawXml;
    UpnpError   error;
};

struct UpnpTransportInfo {
    bool        ok = false;
    std::string state;      // PLAYING, PAUSED_PLAYBACK, STOPPED, TRANSITIONING, NO_MEDIA_PRESENT
    std::string status;     // OK, ERROR_OCCURRED
    std::string speed;      // 1
    UpnpError   error;
};

struct UpnpPositionInfo {
    bool        ok = false;
    uint32_t    track = 0;
    std::string trackDuration;   // HH:MM:SS
    std::string trackMetaData;
    std::string trackUri;
    std::string relTime;         // HH:MM:SS
    std::string absTime;         // HH:MM:SS
    UpnpError   error;
};

struct UpnpMediaInfo {
    bool        ok = false;
    uint32_t    nrTracks = 0;
    std::string mediaDuration;   // HH:MM:SS
    std::string currentUri;
    std::string currentUriMetaData;
    std::string nextUri;
    UpnpError   error;
};

struct UpnpProtocolInfo {
    bool        ok = false;
    std::string source;
    std::string sink;            // Comma-separated protocol infos
    UpnpError   error;
};

inline UpnpTransportInfo parse_transport_info_response(const std::string& xml) {
    UpnpTransportInfo info;
    if (parse_upnp_error(xml, info.error)) return info;
    info.state = xml_extract_tag(xml, "CurrentTransportState");
    info.status = xml_extract_tag(xml, "CurrentTransportStatus");
    info.speed = xml_extract_tag(xml, "CurrentSpeed");
    info.ok = !info.state.empty();
    return info;
}

inline UpnpPositionInfo parse_position_info_response(const std::string& xml) {
    UpnpPositionInfo info;
    if (parse_upnp_error(xml, info.error)) return info;
    std::string t = xml_extract_tag(xml, "Track");
    if (!t.empty()) info.track = (uint32_t)std::strtoul(t.c_str(), nullptr, 10);
    info.trackDuration = xml_extract_tag(xml, "TrackDuration");
    info.trackMetaData = xml_extract_tag(xml, "TrackMetaData");
    info.trackUri = xml_extract_tag(xml, "TrackURI");
    info.relTime = xml_extract_tag(xml, "RelTime");
    info.absTime = xml_extract_tag(xml, "AbsTime");
    info.ok = true;
    return info;
}

inline UpnpMediaInfo parse_media_info_response(const std::string& xml) {
    UpnpMediaInfo info;
    if (parse_upnp_error(xml, info.error)) return info;
    std::string nr = xml_extract_tag(xml, "NrTracks");
    if (!nr.empty()) info.nrTracks = (uint32_t)std::strtoul(nr.c_str(), nullptr, 10);
    info.mediaDuration = xml_extract_tag(xml, "MediaDuration");
    info.currentUri = xml_extract_tag(xml, "CurrentURI");
    info.currentUriMetaData = xml_extract_tag(xml, "CurrentURIMetaData");
    info.nextUri = xml_extract_tag(xml, "NextURI");
    info.ok = true;
    return info;
}

inline UpnpProtocolInfo parse_protocol_info_response(const std::string& xml) {
    UpnpProtocolInfo info;
    if (parse_upnp_error(xml, info.error)) return info;
    info.source = xml_extract_tag(xml, "Source");
    info.sink = xml_extract_tag(xml, "Sink");
    info.ok = true;
    return info;
}

// ---------------- UPnP 控制客户端 ----------------

class UpnpControlClient {
public:
    UpnpControlClient() = default;
    UpnpControlClient(std::string avTransportUrl, std::string connectionManagerUrl = "")
        : avTransportUrl_(std::move(avTransportUrl)), connectionManagerUrl_(std::move(connectionManagerUrl)) {}

    void set_endpoints(const std::string& avTransportUrl, const std::string& connectionManagerUrl = "") {
        avTransportUrl_ = avTransportUrl;
        connectionManagerUrl_ = connectionManagerUrl;
    }

    const std::string& av_transport_url() const { return avTransportUrl_; }
    const std::string& connection_manager_url() const { return connectionManagerUrl_; }

    UpnpActionResult set_av_transport_uri(const std::string& uri, const std::string& metadataXml = "", uint32_t instanceId = 0) {
        std::string body = build_set_av_transport_uri_soap(instanceId, uri, metadataXml);
        return execute_action(avTransportUrl_, kServiceAVTransport, "SetAVTransportURI", body);
    }

    UpnpActionResult play(const std::string& speed = "1", uint32_t instanceId = 0) {
        std::string body = build_play_soap(instanceId, speed);
        return execute_action(avTransportUrl_, kServiceAVTransport, "Play", body);
    }

    UpnpActionResult pause(uint32_t instanceId = 0) {
        std::string body = build_pause_soap(instanceId);
        return execute_action(avTransportUrl_, kServiceAVTransport, "Pause", body);
    }

    UpnpActionResult stop(uint32_t instanceId = 0) {
        std::string body = build_stop_soap(instanceId);
        return execute_action(avTransportUrl_, kServiceAVTransport, "Stop", body);
    }

    UpnpActionResult seek_rel_time(const std::string& targetHhMmSs, uint32_t instanceId = 0) {
        std::string body = build_seek_soap(instanceId, "REL_TIME", targetHhMmSs);
        return execute_action(avTransportUrl_, kServiceAVTransport, "Seek", body);
    }

    UpnpTransportInfo get_transport_info(uint32_t instanceId = 0) {
        std::string body = build_get_transport_info_soap(instanceId);
        auto r = execute_action(avTransportUrl_, kServiceAVTransport, "GetTransportInfo", body);
        if (!r.ok) {
            UpnpTransportInfo t; t.ok = false; t.error = r.error; return t;
        }
        return parse_transport_info_response(r.rawXml);
    }

    UpnpPositionInfo get_position_info(uint32_t instanceId = 0) {
        std::string body = build_get_position_info_soap(instanceId);
        auto r = execute_action(avTransportUrl_, kServiceAVTransport, "GetPositionInfo", body);
        if (!r.ok) {
            UpnpPositionInfo p; p.ok = false; p.error = r.error; return p;
        }
        return parse_position_info_response(r.rawXml);
    }

    UpnpMediaInfo get_media_info(uint32_t instanceId = 0) {
        std::string body = build_get_media_info_soap(instanceId);
        auto r = execute_action(avTransportUrl_, kServiceAVTransport, "GetMediaInfo", body);
        if (!r.ok) {
            UpnpMediaInfo m; m.ok = false; m.error = r.error; return m;
        }
        return parse_media_info_response(r.rawXml);
    }

    UpnpProtocolInfo get_protocol_info() {
        std::string url = !connectionManagerUrl_.empty() ? connectionManagerUrl_ : avTransportUrl_;
        std::string body = build_get_protocol_info_soap();
        auto r = execute_action(url, kServiceConnectionManager, "GetProtocolInfo", body);
        if (!r.ok) {
            UpnpProtocolInfo p; p.ok = false; p.error = r.error; return p;
        }
        return parse_protocol_info_response(r.rawXml);
    }

private:
    UpnpActionResult execute_action(const std::string& url,
                                   const std::string& serviceType,
                                   const std::string& action,
                                   const std::string& body) {
        UpnpActionResult res;
        res.action = action;
        if (url.empty()) {
            res.ok = false;
            res.error.code = -1;
            res.error.description = "控制 URL 为空，无法执行 " + action;
            return res;
        }
        std::vector<std::string> headers = {
            "SOAPAction: \"" + std::string(serviceType) + "#" + action + "\""
        };
        HttpResponse httpRes = http_post(url, body, headers, "text/xml; charset=\"utf-8\"", 8000);
        res.httpStatus = httpRes.status;
        res.rawXml = std::string(httpRes.body.begin(), httpRes.body.end());
        if (httpRes.ok && httpRes.status >= 200 && httpRes.status < 300) {
            if (parse_upnp_error(res.rawXml, res.error)) {
                res.ok = false;
            } else {
                res.ok = true;
            }
        } else {
            res.ok = false;
            if (!parse_upnp_error(res.rawXml, res.error)) {
                res.error.code = httpRes.status ? httpRes.status : -1;
                res.error.description = !httpRes.error.empty() ? httpRes.error : "HTTP " + std::to_string(httpRes.status);
            }
        }
        return res;
    }

    std::string avTransportUrl_;
    std::string connectionManagerUrl_;
};

// ---------------- 渲染器实际采样率取证 ----------------

struct AudioSourceFormat {
    uint32_t    sampleRate = 0;
    int         bits = 0;
    int         channels = 2;
    bool        isDsd = false;
    std::string encoding = "PCM";
};

enum class SamplingRateVerdict {
    Match,       // 声明采样率/位深与源格式一致
    Mismatch,    // 声明采样率/位深与源格式不一致 -> 警告可能重采样
    Unknown      // 渲染器未声明具体硬件工作采样率/位深
};

struct SamplingRateAnalysis {
    SamplingRateVerdict verdict = SamplingRateVerdict::Unknown;
    uint32_t    declaredRate = 0;
    int         declaredBits = 0;
    int         declaredChannels = 0;
    std::string sourceDesc;
    std::string rendererDesc;
    std::string warning;
    std::string detail;

    bool is_bit_perfect_verified() const {
        return verdict == SamplingRateVerdict::Match;
    }
};

// ⚠️ 2026-10-29 遗留函数（仍有离线单测断言它，故保留行为不动）：
//   它把 media.currentUriMetaData（渲染器回显的、我们自己发过去的 DIDL）当成"渲染器能力"，
//   那份 DIDL 里的 sampleFrequency/bitsPerSample 就是源格式 ⟹ 第一分支恒为 Match（同一律），
//   证明不了任何事。投放链路已改用 judge_upnp_bit_perfect()（只认 GetProtocolInfo 的 Sink）。
inline SamplingRateAnalysis analyze_sampling_rate(const AudioSourceFormat& src,
                                                 const UpnpMediaInfo& media,
                                                 const UpnpPositionInfo& pos,
                                                 const UpnpProtocolInfo& proto) {
    SamplingRateAnalysis a;
    char srcBuf[128];
    std::snprintf(srcBuf, sizeof(srcBuf), "%u Hz / %d bit / %d ch (%s)",
                  src.sampleRate, src.bits, src.channels, src.encoding.c_str());
    a.sourceDesc = srcBuf;

    // 1) 优先解析 CurrentURIMetaData 或 TrackMetaData 里的 DIDL-Lite 信息
    std::string metaXml = !media.currentUriMetaData.empty() ? media.currentUriMetaData : pos.trackMetaData;
    UpnpDidlItem didl = parse_didl_lite(metaXml);
    if (didl.sampleRate > 0) a.declaredRate = didl.sampleRate;
    if (didl.bits > 0) a.declaredBits = didl.bits;
    if (didl.channels > 0) a.declaredChannels = didl.channels;

    // 2) 如果 metadata 没有，从 proto.sink 解析支持格式
    std::vector<std::pair<uint32_t, int>> supportedFormats;
    bool hasGenericWav = false;
    if (!proto.sink.empty()) {
        std::stringstream ss(proto.sink);
        std::string token;
        while (std::getline(ss, token, ',')) {
            size_t fa = token.find_first_not_of(" \t\r\n");
            size_t fb = token.find_last_not_of(" \t\r\n");
            if (fa == std::string::npos) continue;
            std::string item = token.substr(fa, fb - fa + 1);
            if (item.find("audio/wav") != std::string::npos || item.find("audio/x-wav") != std::string::npos) {
                hasGenericWav = true;
            }
            size_t pRate = item.find("rate=");
            uint32_t r = 0;
            if (pRate != std::string::npos) {
                r = (uint32_t)std::strtoul(item.c_str() + pRate + 5, nullptr, 10);
            }
            int b = 0;
            if (item.find("audio/L16") != std::string::npos) b = 16;
            else if (item.find("audio/L24") != std::string::npos) b = 24;
            else if (item.find("audio/L32") != std::string::npos) b = 32;

            if (r > 0 && b > 0) {
                supportedFormats.push_back({r, b});
            }
        }
    }

    if (a.declaredRate > 0 && a.declaredBits > 0) {
        char rendBuf[128];
        std::snprintf(rendBuf, sizeof(rendBuf), "%u Hz / %d bit / %d ch (DIDL-Lite 声明)",
                      a.declaredRate, a.declaredBits, a.declaredChannels > 0 ? a.declaredChannels : 2);
        a.rendererDesc = rendBuf;

        if (a.declaredRate == src.sampleRate && a.declaredBits == src.bits) {
            a.verdict = SamplingRateVerdict::Match;
            a.detail = "渲染器声明的接收格式与源格式完全匹配 (" + a.rendererDesc + ")。";
        } else {
            a.verdict = SamplingRateVerdict::Mismatch;
            a.warning = "⚠️ 告警：渲染器可能重采样，非 bit-perfect！源格式: " + a.sourceDesc + "，渲染器声明: " + a.rendererDesc;
            a.detail = a.warning;
        }
        return a;
    }

    if (!supportedFormats.empty()) {
        bool exactMatchFound = false;
        uint32_t maxSupportedRate = 0;
        int maxSupportedBits = 0;
        for (const auto& f : supportedFormats) {
            if (f.first == src.sampleRate && (f.second == src.bits || src.bits == 0)) {
                exactMatchFound = true;
            }
            if (f.first > maxSupportedRate) {
                maxSupportedRate = f.first;
                maxSupportedBits = f.second;
            }
        }
        if (exactMatchFound) {
            a.verdict = SamplingRateVerdict::Match;
            a.declaredRate = src.sampleRate;
            a.declaredBits = src.bits;
            a.rendererDesc = "支持 " + std::to_string(src.sampleRate) + " Hz / " + std::to_string(src.bits) + " bit (来自 ProtocolInfo Sink)";
            a.detail = "渲染器 ProtocolInfo 声明支持源采样率与位深。";
            return a;
        } else if (maxSupportedRate > 0 && (maxSupportedRate < src.sampleRate || maxSupportedBits < src.bits)) {
            a.verdict = SamplingRateVerdict::Mismatch;
            a.declaredRate = maxSupportedRate;
            a.declaredBits = maxSupportedBits;
            char rendBuf[128];
            std::snprintf(rendBuf, sizeof(rendBuf), "最高 %u Hz / %d bit (ProtocolInfo Sink)", maxSupportedRate, maxSupportedBits);
            a.rendererDesc = rendBuf;
            a.warning = "⚠️ 告警：渲染器可能重采样，非 bit-perfect！源格式: " + a.sourceDesc + " 超出渲染器声明能力 (" + a.rendererDesc + ")";
            a.detail = a.warning;
            return a;
        }
    }

    a.verdict = SamplingRateVerdict::Unknown;
    if (hasGenericWav) {
        a.rendererDesc = "audio/wav 通配（未声明具体硬件工作采样率）";
    } else {
        a.rendererDesc = !proto.sink.empty() ? proto.sink.substr(0, 80) : "(渲染器未返回 Sink 描述)";
    }
    a.detail = "未声明具体采样率/未做端到端重采样实测（渲染器未返回具体硬件工作时钟，需硬件级采集才能确证实际速率）。";
    return a;
}

// ---------------- 渲染器 Sink 能力解析与 bit-perfect 三态判定（2026-10-29）----------------
//
// 背景（用户原话）：「UPNP 投流，默认显示『非 bit-perfect』。这很离谱……你不能不知道就说不是」。
// 旧实现有两处硬伤：
//   ① 结论根本不在投放链路里算 —— rtaweb.cpp 的 upnp_mark_player_state() 直接写死 ps.bpState="no"；
//   ② 本文件的 analyze_sampling_rate() 把 GetMediaInfo 回显的 CurrentURIMetaData 当成"渲染器能力"，
//      而那份 DIDL 正是我们自己按源格式填好发过去的 —— 拿它比源格式永远是 Match（同一律），
//      同一次会话里于是能同时出现「匹配（Bit-Perfect）」与「非 bit-perfect」两个相反结论。
// 现在只认两件可证的事实：
//   A) 软件侧：供出去的 PCM 是否与源解码结果逐位相同（无重采样/无位深转换/无音量/无 EQ/无抖动）；
//   B) 渲染器侧：ConnectionManager.GetProtocolInfo 的 Sink 到底声明了什么（MIME + 显式 rate/channels）。
// 两条都成立才给 yes；能证明不成立给 no；其余一律 unknown —— 宁可不显示，也不给错误结论。

inline std::string upnp_lower_ascii(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
inline std::string upnp_trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

struct UpnpSinkEntry {
    std::string protocol;     // http-get
    std::string network;      // *
    std::string mime;         // audio/wav、audio/L16（已小写）
    uint32_t    rate = 0;     // contentFormat 里显式写的 rate=，0 = 未声明
    int         channels = 0; // 显式 channels=，0 = 未声明
    int         bits = 0;     // 由 L16/L24/L32 或 DLNA PN 推出，0 = 未声明
    std::string pn;           // DLNA.ORG_PN=
    bool is_audio() const { return mime.rfind("audio/", 0) == 0; }
    bool is_wav() const { return mime == "audio/wav" || mime == "audio/x-wav" || mime == "audio/wave"; }
    bool is_lpcm() const {
        return mime.rfind("audio/l16", 0) == 0 || mime.rfind("audio/l24", 0) == 0 ||
               mime.rfind("audio/l32", 0) == 0;
    }
};

inline UpnpSinkEntry parse_sink_entry(const std::string& token) {
    UpnpSinkEntry e;
    const std::string t = token;
    const size_t c1 = t.find(':');
    if (c1 == std::string::npos) { e.mime = upnp_lower_ascii(upnp_trim(t)); return e; }
    e.protocol = upnp_lower_ascii(upnp_trim(t.substr(0, c1)));
    const size_t c2 = t.find(':', c1 + 1);
    if (c2 == std::string::npos) { e.mime = upnp_lower_ascii(upnp_trim(t.substr(c1 + 1))); return e; }
    e.network = upnp_trim(t.substr(c1 + 1, c2 - c1 - 1));
    const size_t c3 = t.find(':', c2 + 1);
    const std::string content = (c3 == std::string::npos) ? t.substr(c2 + 1)
                                                          : t.substr(c2 + 1, c3 - c2 - 1);
    const std::string extra = (c3 == std::string::npos) ? std::string() : t.substr(c3 + 1);

    size_t semi = content.find(';');
    e.mime = upnp_lower_ascii(upnp_trim(content.substr(0, semi)));
    while (semi != std::string::npos) {
        const size_t next = content.find(';', semi + 1);
        const std::string kv = upnp_trim(content.substr(semi + 1,
                                     next == std::string::npos ? std::string::npos : next - semi - 1));
        const size_t eq = kv.find('=');
        if (eq != std::string::npos) {
            const std::string k = upnp_lower_ascii(upnp_trim(kv.substr(0, eq)));
            const std::string v = upnp_trim(kv.substr(eq + 1));
            if (k == "rate") e.rate = (uint32_t)std::strtoul(v.c_str(), nullptr, 10);
            else if (k == "channels") e.channels = std::atoi(v.c_str());
        }
        semi = next;
    }
    if (e.mime.rfind("audio/l16", 0) == 0) e.bits = 16;
    else if (e.mime.rfind("audio/l24", 0) == 0) e.bits = 24;
    else if (e.mime.rfind("audio/l32", 0) == 0) e.bits = 32;

    const std::string ex = upnp_lower_ascii(extra);
    const size_t pn = ex.find("dlna.org_pn=");
    if (pn != std::string::npos) {
        const size_t end = ex.find(';', pn);
        e.pn = upnp_trim(extra.substr(pn + 12, end == std::string::npos ? std::string::npos : end - pn - 12));
        if (e.bits == 0 && e.pn == "wav") e.bits = 16;   // DLNA WAV 档 = 16-bit LPCM
    }
    return e;
}

struct UpnpSinkCaps {
    bool ok = false;                                  // 是否拿到非空 Sink
    std::vector<UpnpSinkEntry> entries;
    std::string raw;

    enum class Answer { Yes, No, Unknown };

    // 我们的流是 PCM WAV（http-get audio/wav，显式 16/24-bit）。渲染器能不能原样收，看 Sink：
    //   Yes     = 有一条 wav/LPCM 声明，速率/位深/声道都写得下且与我们一致
    //   No      = 它明确声明了 wav/LPCM 但没有一条能装下我们的格式（或压根没声明 WAV）
    //   Unknown = 只有通配 audio/wav:*（没写 rate）── 绝大多数设备的实际情况
    Answer match_stream(uint32_t rate, int bits, int channels) const {
        if (!ok) return Answer::Unknown;
        bool saw_wav_family = false;
        bool saw_unconstrained = false;
        bool saw_explicit = false;
        bool saw_any_wildcard = false;
        for (const auto& e : entries) {
            if (e.mime == "*") { saw_any_wildcard = true; continue; }
            if (!(e.is_wav() || e.is_lpcm())) continue;
            saw_wav_family = true;
            if (e.rate == 0 && e.channels == 0) { saw_unconstrained = true; continue; }
            saw_explicit = true;
            const bool rate_ok = (e.rate == 0) || (rate == 0) || (e.rate == rate);
            const bool bits_ok = (e.bits == 0) || (bits == 0) || (e.bits == bits);
            const bool ch_ok = (e.channels == 0) || (channels == 0) || (e.channels == channels);
            if (rate_ok && bits_ok && ch_ok) return Answer::Yes;
        }
        if (!saw_wav_family) {
            if (saw_any_wildcard) return Answer::Unknown;
            return Answer::No;              // Sink 里根本没有 wav/LPCM ⟹ 我们的 WAV 它不收
        }
        if (saw_unconstrained) return Answer::Unknown;
        if (saw_explicit) return Answer::No;   // 全部带显式约束，且没有一条匹配
        return Answer::Unknown;
    }

    std::string summary() const {
        if (!ok) return "(空)";
        std::string s;
        size_t n = 0;
        for (const auto& e : entries) {
            if (!(e.is_audio() || e.mime == "*")) continue;
            if (n++) s += ", ";
            s += e.mime;
            if (e.rate) s += "/" + std::to_string(e.rate) + "Hz";
            if (e.channels) s += "/" + std::to_string(e.channels) + "ch";
            if (e.bits) s += "/" + std::to_string(e.bits) + "bit";
            if (n >= 8) { s += ", …"; break; }
        }
        return s.empty() ? "(无音频条目)" : s;
    }
};

inline UpnpSinkCaps parse_sink_caps(const std::string& sink) {
    UpnpSinkCaps caps;
    caps.raw = sink.size() > 1200 ? sink.substr(0, 1200) + "…" : sink;
    if (sink.empty()) return caps;
    size_t pos = 0;
    while (pos <= sink.size()) {
        const size_t comma = sink.find(',', pos);
        const std::string tok = upnp_trim(sink.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos));
        if (!tok.empty()) {
            caps.entries.push_back(parse_sink_entry(tok));
            caps.ok = true;
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return caps;
}

enum class BitPerfectState { Yes, No, Unknown };

struct BitPerfectVerdict {
    BitPerfectState state = BitPerfectState::Unknown;
    std::string why;
    std::string sourceDesc;
    std::string rendererDesc;
    const char* str() const {
        switch (state) {
            case BitPerfectState::Yes: return "yes";
            case BitPerfectState::No:  return "no";
            default: return "unknown";
        }
    }
};

struct BitPerfectInput {
    AudioSourceFormat src;
    UpnpSinkCaps      caps;
    bool        softwareBitExact = true;   // 软件侧：供源 PCM == 源解码样本（无重采样/位深转换/音量/EQ/抖动）
    std::string softwareWhy;               // 软件侧结论的人话（"逐位直通…" / "已重采样 44.1k→192k…"）
    bool        dop = false;               // DSD over PCM 载波
    bool        fetched = false;           // 渲染器是否已经来取过流
    bool        playing = false;           // 渲染器是否报告 PLAYING/PAUSED
};

inline BitPerfectVerdict judge_upnp_bit_perfect(const BitPerfectInput& in) {
    BitPerfectVerdict v;
    {
        char b[160];
        std::snprintf(b, sizeof(b), "%u Hz / %d bit / %d ch (%s)",
                      in.src.sampleRate, in.src.bits, in.src.channels, in.src.encoding.c_str());
        v.sourceDesc = b;
    }
    v.rendererDesc = in.caps.ok ? in.caps.summary() : "(渲染器未返回 Sink)";

    // 1) 软件侧已经确定改过数据 ⟹ 铁定不是 bit-perfect，且原因是我们自己的
    if (!in.softwareBitExact) {
        v.state = BitPerfectState::No;
        v.why = "软件侧已改动：" + (in.softwareWhy.empty() ? std::string("供源链做了转换") : in.softwareWhy);
        return v;
    }

    // 2) DSD/DoP：软件侧逐位（DoP 载波），但 UPnP 没有"我支持 DoP"的表达能力
    if (in.dop) {
        v.state = BitPerfectState::Unknown;
        v.why = "软件侧：DSD 以 DoP v1.1 打包（" + v.sourceDesc +
                "），载波逐位无损；渲染器侧：UPnP 协议无法声明 DoP 能力，载波是否被原样送到 DAC 探测不到"
                "（不支持 DoP 的渲染器会把它当 24bit PCM 解码成噪声）。";
        return v;
    }

    // 3) 没拿到 Sink ⟹ 不知道
    if (!in.caps.ok) {
        v.state = BitPerfectState::Unknown;
        v.why = "软件侧：" + (in.softwareWhy.empty() ? std::string("逐位直通") : in.softwareWhy) +
                "；渲染器侧：GetProtocolInfo 未返回可用的 Sink 能力，无法判断设备是否原样接收 " + v.sourceDesc + "。";
        return v;
    }

    const auto ans = in.caps.match_stream(in.src.sampleRate, in.src.bits, in.src.channels);
    if (ans == UpnpSinkCaps::Answer::No) {
        v.state = BitPerfectState::No;
        v.why = "渲染器 Sink 未声明支持 " + v.sourceDesc + "（Sink: " + v.rendererDesc +
                "）⟹ 设备会拒收或自行转换，不可能 bit-perfect。";
        return v;
    }
    if (ans == UpnpSinkCaps::Answer::Yes) {
        if (!in.fetched) {
            v.state = BitPerfectState::Unknown;
            v.why = "渲染器尚未来取流（Sink 声明支持，但没有实际取流证据）。";
            return v;
        }
        if (!in.playing) {
            v.state = BitPerfectState::Unknown;
            v.why = "渲染器 Sink 声明支持 " + v.sourceDesc + "，但当前没有报告 PLAYING，无法确认在播。";
            return v;
        }
        v.state = BitPerfectState::Yes;
        v.why = "软件侧：" + (in.softwareWhy.empty() ? std::string("逐位直通") : in.softwareWhy) +
                "；渲染器 Sink 显式声明支持 " + v.sourceDesc + "（" + v.rendererDesc +
                "）且已取流并在播放。设备内部是否再处理（音量/DSP）无法从 UPnP 探测。";
        return v;
    }

    v.state = BitPerfectState::Unknown;
    v.why = "软件侧：" + (in.softwareWhy.empty() ? std::string("逐位直通") : in.softwareWhy) +
            "；渲染器侧：Sink 只有通配声明（" + v.rendererDesc +
            "），没给采样率/位深 ⟹ 设备内部是否重采样无法从 UPnP 探测。";
    return v;
}

// ---------------- HTTP Range 语义与 Token 防遍历纯函数 ----------------

struct HttpRange {
    int64_t start = 0;
    int64_t end = -1;
    bool    hasRange = false;
    bool    valid = false;
};

inline HttpRange parse_http_range(const std::string& rangeHeader, int64_t totalSize) {
    HttpRange r;
    if (rangeHeader.empty() || totalSize <= 0) return r;

    size_t eq = rangeHeader.find("bytes=");
    if (eq == std::string::npos) return r;
    r.hasRange = true;

    std::string spec = rangeHeader.substr(eq + 6);
    size_t fa = spec.find_first_not_of(" \t\r\n");
    size_t fb = spec.find_last_not_of(" \t\r\n");
    if (fa == std::string::npos) return r;
    spec = spec.substr(fa, fb - fa + 1);

    size_t dash = spec.find('-');
    if (dash == std::string::npos) return r;

    if (dash == 0) {
        int64_t suffix = std::strtoll(spec.c_str() + 1, nullptr, 10);
        if (suffix <= 0) return r;
        if (suffix > totalSize) suffix = totalSize;
        r.start = totalSize - suffix;
        r.end = totalSize - 1;
        r.valid = true;
        return r;
    }

    int64_t start = std::strtoll(spec.substr(0, dash).c_str(), nullptr, 10);
    if (start < 0 || start >= totalSize) {
        r.valid = false; // 416
        return r;
    }
    r.start = start;

    std::string endStr = spec.substr(dash + 1);
    if (endStr.empty()) {
        r.end = totalSize - 1;
        r.valid = true;
        return r;
    }

    int64_t end = std::strtoll(endStr.c_str(), nullptr, 10);
    if (end < start) {
        r.valid = false;
        return r;
    }
    if (end >= totalSize) end = totalSize - 1;
    r.end = end;
    r.valid = true;
    return r;
}

struct StreamResponseHeaders {
    int         statusCode = 200;
    std::string statusText = "OK";
    int64_t     rangeStart = 0;
    int64_t     rangeEnd = 0;
    int64_t     contentLength = 0;
    int64_t     totalSize = 0;
    std::string headerString;
};

inline StreamResponseHeaders build_stream_response_headers(const HttpRange& range,
                                                           int64_t totalSize,
                                                           const std::string& mimeType = "audio/wav",
                                                           const std::string& extraHeaders = "") {
    StreamResponseHeaders h;
    h.totalSize = totalSize;

    if (!range.hasRange) {
        h.statusCode = 200;
        h.statusText = "OK";
        h.rangeStart = 0;
        h.rangeEnd = (totalSize > 0) ? totalSize - 1 : 0;
        h.contentLength = totalSize;

        std::ostringstream ss;
        ss << "HTTP/1.1 200 OK\r\n"
           << "Content-Type: " << mimeType << "\r\n"
           << "Content-Length: " << totalSize << "\r\n"
           << "Accept-Ranges: bytes\r\n"
           << extraHeaders
           << "Connection: close\r\n\r\n";
        h.headerString = ss.str();
        return h;
    }

    if (!range.valid) {
        h.statusCode = 416;
        h.statusText = "Range Not Satisfiable";
        h.contentLength = 0;

        std::ostringstream ss;
        ss << "HTTP/1.1 416 Range Not Satisfiable\r\n"
           << "Content-Range: bytes */" << totalSize << "\r\n"
           << "Content-Length: 0\r\n"
           << "Connection: close\r\n\r\n";
        h.headerString = ss.str();
        return h;
    }

    h.statusCode = 206;
    h.statusText = "Partial Content";
    h.rangeStart = range.start;
    h.rangeEnd = range.end;
    h.contentLength = (range.end - range.start + 1);

    std::ostringstream ss;
    ss << "HTTP/1.1 206 Partial Content\r\n"
       << "Content-Type: " << mimeType << "\r\n"
       << "Content-Range: bytes " << h.rangeStart << "-" << h.rangeEnd << "/" << totalSize << "\r\n"
       << "Content-Length: " << h.contentLength << "\r\n"
       << "Accept-Ranges: bytes\r\n"
       << extraHeaders
       << "Connection: close\r\n\r\n";
    h.headerString = ss.str();
    return h;
}

inline bool validate_stream_token(const std::string& requestPath, const std::string& expectedToken) {
    if (expectedToken.empty() || requestPath.empty()) return false;
    // 防目录遍历
    if (requestPath.find("..") != std::string::npos) return false;
    if (requestPath.find('\\') != std::string::npos) return false;

    // 路径契约：/stream/<token>.wav
    const std::string prefix = "/stream/";
    if (requestPath.rfind(prefix, 0) != 0) return false;
    const std::string suffix = ".wav";
    if (requestPath.size() <= prefix.size() + suffix.size()) return false;
    if (requestPath.rfind(suffix) != requestPath.size() - suffix.size()) return false;

    std::string tokenInPath = requestPath.substr(prefix.size(), requestPath.size() - prefix.size() - suffix.size());
    return tokenInPath == expectedToken;
}

inline std::string generate_random_token(size_t hexLength = 16) {
    static const char hexChars[] = "0123456789abcdef";
    std::string tok;
    tok.reserve(hexLength);
    uint64_t seed = (uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
    for (size_t i = 0; i < hexLength; ++i) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        tok += hexChars[(seed >> 32) & 0x0F];
    }
    return tok;
}

} // namespace rta::net
