/*
 * Etherwaver -- keyboard and mouse sharing utility
 * Copyright (C) 2026 Etherneco Ltd.
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "server/ControlServer.h"

#include "arch/Arch.h"
#include "arch/XArch.h"
#include "base/Log.h"
#include "base/XBase.h"
#include "common/Version.h"
#include "net/NetworkAddress.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>

#if SYSAPI_UNIX
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace etherwaver {
namespace control {

namespace {

const char* kLevelNames[] = {
    "PRINT", "FATAL", "ERROR", "WARNING", "NOTE", "INFO",
    "DEBUG", "DEBUG1", "DEBUG2", "DEBUG3", "DEBUG4", "DEBUG5"
};

class Parser {
public:
    explicit Parser(const std::string& text) : m_text(text), m_pos(0) {}

    bool parseObject(JsonObject& out, std::string& error)
    {
        skipWs();
        if (!consume('{')) { error = "expected object"; return false; }
        skipWs();
        if (consume('}')) { return finish(error); }
        while (true) {
            skipWs();
            std::string key;
            if (!parseString(key)) { error = "expected key"; return false; }
            skipWs();
            if (!consume(':')) { error = "expected ':'"; return false; }
            skipWs();
            JsonValue value;
            if (!parseValue(value)) { error = "invalid value for '" + key + "'"; return false; }
            out[key] = value;
            skipWs();
            if (consume(',')) { continue; }
            if (consume('}')) { break; }
            error = "expected ',' or '}'";
            return false;
        }
        return finish(error);
    }

private:
    bool finish(std::string& error)
    {
        skipWs();
        if (m_pos != m_text.size()) { error = "trailing data"; return false; }
        return true;
    }

    void skipWs()
    {
        while (m_pos < m_text.size() &&
               (m_text[m_pos] == ' ' || m_text[m_pos] == '\t' ||
                m_text[m_pos] == '\r' || m_text[m_pos] == '\n')) {
            ++m_pos;
        }
    }

    bool consume(char c)
    {
        if (m_pos < m_text.size() && m_text[m_pos] == c) { ++m_pos; return true; }
        return false;
    }

    static void appendUtf8(std::string& out, unsigned int cp)
    {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool parseHex4(unsigned int& cp)
    {
        if (m_pos + 4 > m_text.size()) { return false; }
        cp = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = m_text[m_pos++];
            cp <<= 4;
            if (c >= '0' && c <= '9') cp |= static_cast<unsigned int>(c - '0');
            else if (c >= 'a' && c <= 'f') cp |= static_cast<unsigned int>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cp |= static_cast<unsigned int>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool parseString(std::string& out)
    {
        if (!consume('"')) { return false; }
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos++];
            if (c == '"') { return true; }
            if (static_cast<unsigned char>(c) < 0x20) { return false; }
            if (c != '\\') { out += c; continue; }
            if (m_pos >= m_text.size()) { return false; }
            const char e = m_text[m_pos++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned int cp = 0;
                if (!parseHex4(cp)) { return false; }
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    unsigned int low = 0;
                    if (!consume('\\') || !consume('u') || !parseHex4(low) ||
                        low < 0xDC00 || low > 0xDFFF) {
                        return false;
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
                appendUtf8(out, cp);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    bool skipComposite()
    {
        // skips a nested object/array, honouring strings
        int depth = 0;
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos];
            if (c == '"') {
                std::string ignored;
                if (!parseString(ignored)) { return false; }
                continue;
            }
            ++m_pos;
            if (c == '{' || c == '[') { ++depth; }
            else if (c == '}' || c == ']') {
                if (--depth == 0) { return true; }
            }
        }
        return false;
    }

    bool parseValue(JsonValue& value)
    {
        const size_t start = m_pos;
        if (m_pos >= m_text.size()) { return false; }
        const char c = m_text[m_pos];
        if (c == '"') {
            value.m_type = JsonValue::kString;
            if (!parseString(value.m_string)) { return false; }
        } else if (c == '{' || c == '[') {
            value.m_type = JsonValue::kOther;
            if (!skipComposite()) { return false; }
        } else if (m_text.compare(m_pos, 4, "true") == 0) {
            value.m_type = JsonValue::kBool; value.m_bool = true; m_pos += 4;
        } else if (m_text.compare(m_pos, 5, "false") == 0) {
            value.m_type = JsonValue::kBool; value.m_bool = false; m_pos += 5;
        } else if (m_text.compare(m_pos, 4, "null") == 0) {
            value.m_type = JsonValue::kNull; m_pos += 4;
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            value.m_type = JsonValue::kNumber;
            while (m_pos < m_text.size()) {
                const char d = m_text[m_pos];
                if ((d >= '0' && d <= '9') || d == '-' || d == '+' || d == '.' || d == 'e' || d == 'E') {
                    ++m_pos;
                } else {
                    break;
                }
            }
            value.m_string = m_text.substr(start, m_pos - start);
        } else {
            return false;
        }
        value.m_raw = m_text.substr(start, m_pos - start);
        return true;
    }

    const std::string& m_text;
    size_t m_pos;
};

bool constantTimeEquals(const std::string& a, const std::string& b)
{
    unsigned char diff = static_cast<unsigned char>(a.size() != b.size());
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}

std::string randomHex(size_t bytes)
{
    std::random_device rd;
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < bytes; ++i) {
        const unsigned int v = rd() & 0xFF;
        out += hex[v >> 4];
        out += hex[v & 0xF];
    }
    return out;
}

std::string trim(const std::string& value)
{
    const size_t b = value.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) { return std::string(); }
    const size_t e = value.find_last_not_of(" \t\r\n");
    return value.substr(b, e - b + 1);
}

void splitAddress(const std::string& address, std::string& host, int& port)
{
    host = "127.0.0.1";
    port = 24803;
    std::string value = trim(address);
    if (value.empty()) { return; }
    if (value[0] == '[') {
        const size_t close = value.find(']');
        if (close != std::string::npos) {
            host = value.substr(1, close - 1);
            if (close + 1 < value.size() && value[close + 1] == ':') {
                port = std::atoi(value.c_str() + close + 2);
            }
            return;
        }
    }
    const size_t colon = value.rfind(':');
    if (colon == std::string::npos) {
        host = value;
        return;
    }
    if (colon > 0) {
        host = value.substr(0, colon);
    }
    port = std::atoi(value.c_str() + colon + 1);
}

} // namespace

bool
parseFlatObject(const std::string& text, JsonObject& out, std::string& error)
{
    Parser parser(text);
    return parser.parseObject(out, error);
}

std::string
jsonEscape(const std::string& value)
{
    std::string out;
    out.reserve(value.size() + 8);
    for (size_t i = 0; i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out;
}

std::string
jsonString(const std::string& value)
{
    return "\"" + jsonEscape(value) + "\"";
}

ControlServer::ControlServer(IRequestHandler* handler,
                             const std::string& address,
                             const std::string& tokenFile) :
    m_handler(handler),
    m_address(address),
    m_tokenFile(tokenFile),
    m_running(false),
    m_instanceId(randomHex(8))
{
}

ControlServer::~ControlServer()
{
    stop();
}

bool
ControlServer::loadOrCreateToken()
{
    {
        std::ifstream in(m_tokenFile.c_str());
        if (in.good()) {
            std::string token;
            std::getline(in, token);
            token = trim(token);
            if (token.size() >= 32) {
                m_token = token;
                return true;
            }
        }
    }

    // create the parent directory and a new token readable only by us
#if SYSAPI_UNIX
    const size_t slash = m_tokenFile.find_last_of('/');
    if (slash != std::string::npos && slash > 0) {
        std::string dir = m_tokenFile.substr(0, slash);
        std::string partial;
        std::istringstream parts(dir);
        std::string part;
        if (dir[0] == '/') { partial = ""; }
        while (std::getline(parts, part, '/')) {
            if (part.empty()) { continue; }
            partial += "/" + part;
            ::mkdir(partial.c_str(), 0700);
        }
    }
    const mode_t old = ::umask(077);
#endif
    m_token = randomHex(32);
    bool ok = false;
    {
        std::ofstream out(m_tokenFile.c_str(), std::ios::trunc);
        if (out.good()) {
            out << m_token << "\n";
            ok = out.good();
        }
    }
#if SYSAPI_UNIX
    ::umask(old);
    ::chmod(m_tokenFile.c_str(), 0600);
#endif
    return ok;
}

bool
ControlServer::start()
{
    std::string host;
    int port = 0;
    splitAddress(m_address, host, port);
    if (port <= 0 || port > 65535) {
        LOG((CLOG_ERR "control: invalid control address \"%s\"", m_address.c_str()));
        return false;
    }

    if (!loadOrCreateToken()) {
        LOG((CLOG_ERR "control: cannot write token file \"%s\"; control endpoint disabled",
             m_tokenFile.c_str()));
        return false;
    }

    try {
        NetworkAddress addr(host, port);
        addr.resolve();
        m_listener = ARCH->newSocket(ARCH->getAddrFamily(addr.getAddress()),
                                     IArchNetwork::kSTREAM);
        ARCH->setReuseAddrOnSocket(m_listener, true);
        ARCH->bindSocket(m_listener, addr.getAddress());
        ARCH->listenOnSocket(m_listener);
    }
    catch (XBase& e) {
        LOG((CLOG_ERR "control: failed to listen on %s:%d: %s", host.c_str(), port, e.what()));
        if (m_listener != nullptr) {
            ARCH->closeSocket(m_listener);
            m_listener = nullptr;
        }
        return false;
    }

    m_address = host + ":" + std::to_string(port);
    m_running = true;
    m_thread = ARCH->newThread([this]() { ioLoop(); });
    LOG((CLOG_NOTE "control: listening on %s (token file %s)",
         m_address.c_str(), m_tokenFile.c_str()));
    return true;
}

void
ControlServer::stop()
{
    if (!m_running.exchange(false)) {
        return;
    }
    if (m_thread != nullptr) {
        ARCH->unblockPollSocket(m_thread);
        ARCH->wait(m_thread, -1.0);
        ARCH->closeThread(m_thread);
        m_thread = nullptr;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& entry : m_connections) {
        try { ARCH->closeSocket(entry.second->m_socket); } catch (...) {}
    }
    m_connections.clear();
    if (m_listener != nullptr) {
        try { ARCH->closeSocket(m_listener); } catch (...) {}
        m_listener = nullptr;
    }
}

std::string
ControlServer::helloLine() const
{
    std::ostringstream out;
    out << "{\"type\":\"hello\",\"protocol\":\"etherwaver-control\",\"version\":1"
        << ",\"server\":" << jsonString(m_handler->controlServerName())
        << ",\"serverVersion\":" << jsonString(kVersion)
        << ",\"instance\":" << jsonString(m_instanceId)
        << ",\"auth\":\"token\"}";
    return out.str();
}

void
ControlServer::queueLocked(Connection& c, const std::string& line)
{
    if (c.m_closing) {
        return;
    }
    if (c.m_out.size() + line.size() + 1 > kMaxOutputBytes) {
        // slow consumer: drop it rather than grow without bound.
        c.m_out.clear();
        c.m_closing = true;
        return;
    }
    c.m_out += line;
    c.m_out += '\n';
}

void
ControlServer::queueById(int connectionId, const std::string& line, bool completesRequest)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_connections.find(connectionId);
    if (it == m_connections.end()) {
        return;
    }
    if (completesRequest && it->second->m_pending > 0) {
        --it->second->m_pending;
    }
    queueLocked(*it->second, line);
}

void
ControlServer::respond(const Request& request, const std::string& resultJson)
{
    queueById(request.m_connectionId,
              "{\"type\":\"response\",\"id\":" + request.m_rawId +
              ",\"ok\":true,\"result\":" + (resultJson.empty() ? "{}" : resultJson) + "}",
              true);
}

void
ControlServer::respondError(const Request& request, const std::string& code,
                            const std::string& message)
{
    queueById(request.m_connectionId,
              "{\"type\":\"response\",\"id\":" + request.m_rawId +
              ",\"ok\":false,\"error\":{\"code\":" + jsonString(code) +
              ",\"message\":" + jsonString(message) + "}}",
              true);
}

void
ControlServer::publish(const std::string& event, const std::string& dataJson)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const unsigned long long seq = ++m_eventSeq;
    const std::string line =
        "{\"type\":\"event\",\"seq\":" + std::to_string(seq) +
        ",\"event\":" + jsonString(event) +
        ",\"data\":" + (dataJson.empty() ? "{}" : dataJson) + "}";
    for (auto& entry : m_connections) {
        if (entry.second->m_authenticated && entry.second->m_subscribed) {
            queueLocked(*entry.second, line);
        }
    }
}

void
ControlServer::publishLog(int level, const char* message)
{
    if (!m_running || message == nullptr) {
        return;
    }
    const int maxLevel = static_cast<int>(sizeof(kLevelNames) / sizeof(kLevelNames[0])) - 1;
    const char* levelName = kLevelNames[level < 0 ? 0 : (level > maxLevel ? maxLevel : level)];
    std::lock_guard<std::mutex> lock(m_mutex);
    bool any = false;
    for (auto& entry : m_connections) {
        if (entry.second->m_authenticated && entry.second->m_logSubscribed) {
            any = true;
            break;
        }
    }
    if (!any) {
        return;
    }
    const unsigned long long seq = ++m_eventSeq;
    const std::string line =
        "{\"type\":\"event\",\"seq\":" + std::to_string(seq) +
        ",\"event\":\"log\",\"data\":{\"level\":" + jsonString(levelName) +
        ",\"message\":" + jsonString(message) + "}}";
    for (auto& entry : m_connections) {
        if (entry.second->m_authenticated && entry.second->m_logSubscribed) {
            queueLocked(*entry.second, line);
        }
    }
}

void
ControlServer::handleLine(Connection& c, const std::string& rawLine)
{
    const std::string line = trim(rawLine);
    if (line.empty()) {
        return;
    }

    JsonObject object;
    std::string parseError;
    std::unique_ptr<Request> request(new Request);
    request->m_connectionId = c.m_id;
    request->m_rawId = "null";

    std::lock_guard<std::mutex> lock(m_mutex);

    auto replyError = [&](const std::string& code, const std::string& message) {
        queueLocked(c, "{\"type\":\"response\",\"id\":" + request->m_rawId +
                       ",\"ok\":false,\"error\":{\"code\":" + jsonString(code) +
                       ",\"message\":" + jsonString(message) + "}}");
    };
    auto replyOk = [&](const std::string& result) {
        queueLocked(c, "{\"type\":\"response\",\"id\":" + request->m_rawId +
                       ",\"ok\":true,\"result\":" + result + "}");
    };

    if (!parseFlatObject(line, object, parseError)) {
        replyError("bad_request", "malformed JSON: " + parseError);
        return;
    }

    auto idIt = object.find("id");
    if (idIt != object.end() &&
        (idIt->second.m_type == JsonValue::kNumber || idIt->second.m_type == JsonValue::kString)) {
        request->m_rawId = idIt->second.m_raw;
    }
    auto cmdIt = object.find("cmd");
    if (cmdIt == object.end() || cmdIt->second.m_type != JsonValue::kString) {
        replyError("bad_request", "missing \"cmd\"");
        return;
    }
    request->m_command = cmdIt->second.m_string;

    if (request->m_command == "auth") {
        auto tokenIt = object.find("token");
        const std::string token =
            (tokenIt != object.end() && tokenIt->second.m_type == JsonValue::kString)
                ? tokenIt->second.m_string : std::string();
        if (!m_token.empty() && constantTimeEquals(token, m_token)) {
            c.m_authenticated = true;
            replyOk("{\"authenticated\":true}");
        } else {
            replyError("unauthorized", "invalid token");
            if (++c.m_authFailures >= 3) {
                c.m_closing = true;
            }
        }
        return;
    }

    if (request->m_command == "ping") {
        replyOk("{\"pong\":true}");
        return;
    }

    if (!c.m_authenticated) {
        replyError("unauthorized", "authenticate first");
        return;
    }

    if (request->m_command == "subscribe") {
        auto logIt = object.find("log");
        c.m_subscribed = true;
        c.m_logSubscribed = (logIt != object.end() &&
                             logIt->second.m_type == JsonValue::kBool &&
                             logIt->second.m_bool);
        replyOk("{\"subscribed\":true,\"log\":" +
                std::string(c.m_logSubscribed ? "true" : "false") +
                ",\"seq\":" + std::to_string(m_eventSeq) + "}");
        return;
    }

    if (request->m_command == "unsubscribe") {
        c.m_subscribed = false;
        c.m_logSubscribed = false;
        replyOk("{\"subscribed\":false}");
        return;
    }

    if (c.m_pending >= kMaxPendingPerConnection) {
        replyError("busy", "too many outstanding requests on this connection");
        return;
    }

    ++c.m_pending;
    object.erase("id");
    object.erase("cmd");
    request->m_params = object;
    m_handler->postControlRequest(request.release());
}

void
ControlServer::closeConnection(Connection& c)
{
    try {
        ARCH->closeSocket(c.m_socket);
    } catch (...) {
    }
    c.m_socket = nullptr;
}

void
ControlServer::acceptConnections()
{
    while (true) {
        ArchSocket socket = nullptr;
        try {
            socket = ARCH->acceptSocket(m_listener, nullptr);
        } catch (XArch&) {
            return;
        }
        if (socket == nullptr) {
            return;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        if (static_cast<int>(m_connections.size()) >= kMaxConnections) {
            const std::string line =
                "{\"type\":\"response\",\"id\":null,\"ok\":false,\"error\":"
                "{\"code\":\"busy\",\"message\":\"too many control connections\"}}\n";
            try {
                ARCH->writeSocket(socket, reinterpret_cast<const UInt8*>(line.data()), line.size());
                ARCH->closeSocket(socket);
            } catch (...) {
            }
            continue;
        }
        std::unique_ptr<Connection> c(new Connection);
        c->m_id = m_nextConnectionId++;
        c->m_socket = socket;
        try { ARCH->setNoDelayOnSocket(socket, true); } catch (...) {}
        queueLocked(*c, helloLine());
        m_connections[c->m_id] = std::move(c);
    }
}

bool
ControlServer::readConnection(Connection& c)
{
    char buffer[16384];
    size_t n = 0;
    try {
        n = ARCH->readSocket(c.m_socket, buffer, sizeof(buffer));
    } catch (XArch&) {
        return false;
    }
    if (n == 0) {
        // poll reported readability, so a zero read means EOF
        return false;
    }
    c.m_in.append(buffer, n);

    size_t start = 0;
    while (true) {
        const size_t nl = c.m_in.find('\n', start);
        if (nl == std::string::npos) {
            break;
        }
        const std::string line = c.m_in.substr(start, nl - start);
        start = nl + 1;
        if (line.size() > kMaxLineBytes) {
            return false;
        }
        handleLine(c, line);
    }
    c.m_in.erase(0, start);
    if (c.m_in.size() > kMaxLineBytes) {
        std::lock_guard<std::mutex> lock(m_mutex);
        queueLocked(c, "{\"type\":\"response\",\"id\":null,\"ok\":false,\"error\":"
                       "{\"code\":\"bad_request\",\"message\":\"line too long\"}}");
        c.m_closing = true;
        c.m_in.clear();
    }
    return true;
}

bool
ControlServer::writeConnection(Connection& c)
{
    std::string chunk;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (c.m_out.empty()) {
            return true;
        }
        chunk = c.m_out.substr(0, 65536);
    }
    size_t written = 0;
    try {
        written = ARCH->writeSocket(c.m_socket, reinterpret_cast<const UInt8*>(chunk.data()),
                                    chunk.size());
    } catch (XArch&) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    c.m_out.erase(0, written);
    return true;
}

void*
ControlServer::threadMain(void* self)
{
    static_cast<ControlServer*>(self)->ioLoop();
    return nullptr;
}

void
ControlServer::ioLoop()
{
    while (m_running) {
        std::vector<IArchNetwork::PollEntry> entries;
        std::vector<int> ids;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            IArchNetwork::PollEntry listen;
            listen.m_socket = m_listener;
            listen.m_events = IArchNetwork::kPOLLIN;
            listen.m_revents = 0;
            entries.push_back(listen);
            ids.push_back(0);
            for (auto& entry : m_connections) {
                IArchNetwork::PollEntry pe;
                pe.m_socket = entry.second->m_socket;
                pe.m_events = IArchNetwork::kPOLLIN;
                if (!entry.second->m_out.empty()) {
                    pe.m_events |= IArchNetwork::kPOLLOUT;
                }
                pe.m_revents = 0;
                entries.push_back(pe);
                ids.push_back(entry.first);
            }
        }

        try {
            // short timeout: responses/events queued by other threads are
            // flushed on the next iteration.
            ARCH->pollSocket(entries.data(), static_cast<int>(entries.size()), 0.02);
        } catch (XArch&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        if (entries[0].m_revents & IArchNetwork::kPOLLIN) {
            acceptConnections();
        }

        std::vector<int> dead;
        for (size_t i = 1; i < entries.size(); ++i) {
            Connection* c = nullptr;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto it = m_connections.find(ids[i]);
                if (it == m_connections.end()) { continue; }
                c = it->second.get();
            }
            const unsigned short rev = entries[i].m_revents;
            bool alive = true;
            if (rev & (IArchNetwork::kPOLLERR | IArchNetwork::kPOLLNVAL)) {
                alive = false;
            }
            if (alive && (rev & IArchNetwork::kPOLLIN)) {
                alive = readConnection(*c);
            }
            if (alive) {
                alive = writeConnection(*c);
            }
            bool closing = false;
            bool drained = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                closing = c->m_closing;
                drained = c->m_out.empty();
            }
            if (!alive || (closing && drained)) {
                dead.push_back(ids[i]);
            }
        }

        if (!dead.empty()) {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (int id : dead) {
                auto it = m_connections.find(id);
                if (it != m_connections.end()) {
                    closeConnection(*it->second);
                    m_connections.erase(it);
                }
            }
        }
    }
}

bool
ControlLogOutputter::write(ELevel level, const char* message)
{
    ControlServer* server = m_server.load();
    if (server != nullptr) {
        server->publishLog(static_cast<int>(level) + 1, message);
    }
    // let the remaining outputters see the message too
    return false;
}

} // namespace control
} // namespace etherwaver
