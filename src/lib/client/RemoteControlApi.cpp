/*
 * Etherwaver -- remote control of a server from a client's local input.
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "client/RemoteControlApi.h"

#include "arch/Arch.h"
#include "arch/XArch.h"
#include "base/Log.h"
#include "common/DataDirectories.h"
#include "mt/Thread.h"
#include "net/NetworkAddress.h"
#include "net/XSocket.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>

#if SYSAPI_UNIX
#include <sys/stat.h>
#endif

namespace {

std::string
lower(const std::string& s)
{
    std::string r(s);
    for (size_t i = 0; i < r.size(); ++i) {
        r[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(r[i])));
    }
    return r;
}

std::string
urlDecode(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out += ' ';
        }
        else if (s[i] == '%' && i + 2 < s.size() &&
                 std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
                 std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), NULL, 16));
            i += 2;
        }
        else {
            out += s[i];
        }
    }
    return out;
}

bool
constantTimeEquals(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}

std::string
httpResponse(int code, const std::string& body)
{
    const char* text = code == 200 ? "OK" : code == 401 ? "Unauthorized" :
                       code == 404 ? "Not Found" : code == 405 ? "Method Not Allowed" :
                       "Bad Request";
    std::ostringstream out;
    out << "HTTP/1.1 " << code << " " << text << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Cache-Control: no-store\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n\r\n" << body;
    return out.str();
}

} // namespace

RemoteControlApi::RemoteControlApi(int port, Handler handler) :
    m_port(port),
    m_handler(handler),
    m_listener(NULL),
    m_running(false),
    m_thread(NULL)
{
    std::random_device rd;
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        m_token += hex[rd() & 0xf];
    }
}

RemoteControlApi::~RemoteControlApi()
{
    stop();
}

std::string
RemoteControlApi::tokenFilePath()
{
    return (barrier::DataDirectories::profile() / "remote-control-api.json").u8string();
}

bool
RemoteControlApi::writeTokenFile(std::string& error)
{
    const std::string path = tokenFilePath();
    try {
        barrier::fs::create_directories(barrier::DataDirectories::profile());
    }
    catch (...) {
    }
#if SYSAPI_UNIX
    // create with owner-only permissions before writing the secret
    std::remove(path.c_str());
    mode_t old = umask(077);
#endif
    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
#if SYSAPI_UNIX
    umask(old);
#endif
    if (!out.is_open()) {
        error = "cannot write " + path;
        return false;
    }
    out << "{\"port\":" << m_port << ",\"token\":\"" << m_token << "\"}\n";
    out.close();
#if SYSAPI_UNIX
    chmod(path.c_str(), S_IRUSR | S_IWUSR);
#endif
    return !out.fail();
}

bool
RemoteControlApi::start(std::string& error)
{
    if (m_port <= 0) {
        error = "disabled";
        return false;
    }
    try {
        NetworkAddress addr("127.0.0.1", m_port);
        addr.resolve();
        ArchSocket s = ARCH->newSocket(IArchNetwork::kINET, IArchNetwork::kSTREAM);
        try {
            ARCH->setReuseAddrOnSocket(s, true);
            ARCH->bindSocket(s, addr.getAddress());
            ARCH->listenOnSocket(s);
        }
        catch (...) {
            ARCH->closeSocket(s);
            throw;
        }
        m_listener = s;
    }
    catch (XBase& e) {
        error = e.what();
        return false;
    }
    catch (std::exception& e) {
        error = e.what();
        return false;
    }

    if (!writeTokenFile(error)) {
        ARCH->closeSocket(static_cast<ArchSocket>(m_listener));
        m_listener = NULL;
        return false;
    }

    m_running = true;
    m_thread = new Thread([this]() { loop(); });
    LOG((CLOG_NOTE "remote control API listening on 127.0.0.1:%d (token in %s)",
         m_port, tokenFilePath().c_str()));
    return true;
}

void
RemoteControlApi::stop()
{
    if (!m_running.exchange(false)) {
        return;
    }
    if (m_thread != NULL) {
        m_thread->wait();
        delete m_thread;
        m_thread = NULL;
    }
    if (m_listener != NULL) {
        ARCH->closeSocket(static_cast<ArchSocket>(m_listener));
        m_listener = NULL;
    }
    std::remove(tokenFilePath().c_str());
}

void
RemoteControlApi::loop()
{
    while (m_running) {
        IArchNetwork::PollEntry pe;
        pe.m_socket  = static_cast<ArchSocket>(m_listener);
        pe.m_events  = IArchNetwork::kPOLLIN;
        pe.m_revents = 0;
        try {
            if (ARCH->pollSocket(&pe, 1, 0.2) <= 0 || (pe.m_revents & IArchNetwork::kPOLLIN) == 0) {
                continue;
            }
            ArchSocket client = ARCH->acceptSocket(static_cast<ArchSocket>(m_listener), NULL);
            if (client == NULL) {
                continue;
            }
            try {
                serve(client);
            }
            catch (...) {
            }
            ARCH->closeSocket(client);
        }
        catch (...) {
            // keep serving
        }
    }
}

bool
RemoteControlApi::parseRequest(const std::string& request, std::string& method,
                               std::string& path, std::map<std::string, std::string>& query,
                               std::map<std::string, std::string>& headers)
{
    size_t headerEnd = request.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        headerEnd = request.find("\n\n");
        if (headerEnd == std::string::npos) {
            return false;
        }
    }
    std::istringstream in(request.substr(0, headerEnd));
    std::string line;
    if (!std::getline(in, line)) {
        return false;
    }
    if (!line.empty() && line[line.size() - 1] == '\r') {
        line.erase(line.size() - 1);
    }
    std::istringstream rl(line);
    std::string target, version;
    rl >> method >> target >> version;
    if (method.empty() || target.empty() || target[0] != '/') {
        return false;
    }
    const size_t q = target.find('?');
    path = target.substr(0, q);
    if (q != std::string::npos) {
        std::istringstream qs(target.substr(q + 1));
        std::string pair;
        while (std::getline(qs, pair, '&')) {
            const size_t eq = pair.find('=');
            if (eq == std::string::npos) {
                query[urlDecode(pair)] = "";
            }
            else {
                query[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
            }
        }
    }
    while (std::getline(in, line)) {
        if (!line.empty() && line[line.size() - 1] == '\r') {
            line.erase(line.size() - 1);
        }
        const size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        std::string value = line.substr(colon + 1);
        const size_t b = value.find_first_not_of(" \t");
        value = (b == std::string::npos) ? std::string() : value.substr(b);
        headers[lower(line.substr(0, colon))] = value;
    }
    return true;
}

void
RemoteControlApi::serve(void* vsocket)
{
    ArchSocket socket = static_cast<ArchSocket>(vsocket);
    std::string request;
    char buffer[2048];
    double deadline = ARCH->time() + 2.0;
    while (request.find("\r\n\r\n") == std::string::npos &&
           request.find("\n\n") == std::string::npos) {
        if (request.size() > 16384 || ARCH->time() > deadline) {
            return;
        }
        IArchNetwork::PollEntry pe;
        pe.m_socket = socket;
        pe.m_events = IArchNetwork::kPOLLIN;
        pe.m_revents = 0;
        if (ARCH->pollSocket(&pe, 1, 0.1) <= 0) {
            continue;
        }
        const size_t n = ARCH->readSocket(socket, buffer, sizeof(buffer));
        if (n == 0) {
            break;
        }
        request.append(buffer, n);
    }

    std::string method, path;
    std::map<std::string, std::string> query, headers;
    std::string response;
    if (!parseRequest(request, method, path, query, headers)) {
        response = httpResponse(400, "{\"ok\":false,\"error\":\"bad request\"}");
    }
    else if (!constantTimeEquals(headers["x-etherwaver-token"], m_token)) {
        response = httpResponse(401, "{\"ok\":false,\"error\":\"missing or wrong token\"}");
    }
    else if (path.compare(0, 15, "/remote-control") != 0) {
        response = httpResponse(404, "{\"ok\":false,\"error\":\"not found\"}");
    }
    else if (method != "GET" && method != "POST") {
        response = httpResponse(405, "{\"ok\":false,\"error\":\"method not allowed\"}");
    }
    else {
        response = httpResponse(200, m_handler(method, path, query));
    }

    size_t sent = 0;
    while (sent < response.size()) {
        const size_t n = ARCH->writeSocket(socket,
            reinterpret_cast<const UInt8*>(response.data()) + sent, response.size() - sent);
        if (n == 0) {
            IArchNetwork::PollEntry pe;
            pe.m_socket = socket;
            pe.m_events = IArchNetwork::kPOLLOUT;
            pe.m_revents = 0;
            if (ARCH->pollSocket(&pe, 1, 0.5) <= 0) {
                break;
            }
            continue;
        }
        sent += n;
    }
}
