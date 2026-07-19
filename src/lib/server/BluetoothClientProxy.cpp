#include "server/BluetoothClientProxy.h"

#include "base/Log.h"
#include "core/layout/Screen.h"

#include <algorithm>
#include <cstring>
#include <sstream>

#if SYSAPI_UNIX
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#if SYSAPI_WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace {

#if SYSAPI_WIN32
bool ensureWinsockStarted()
{
    static bool started = false;
    if (started) {
        return true;
    }

    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return false;
    }

    started = true;
    return true;
}
#endif

intptr_t invalidSocket()
{
#if SYSAPI_WIN32
    return static_cast<intptr_t>(INVALID_SOCKET);
#else
    return -1;
#endif
}

void splitEndpoint(const std::string& endpoint, std::string& host, std::string& port)
{
    const std::string::size_type colon = endpoint.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= endpoint.size()) {
        host = "127.0.0.1";
        port = "24810";
        return;
    }
    host = endpoint.substr(0, colon);
    port = endpoint.substr(colon + 1);
}

std::string boolValue(bool value)
{
    return value ? "1" : "0";
}

}

BluetoothClientProxy::BluetoothClientProxy(const std::string& hostName, const std::string& endpoint) :
    BaseClientProxy(hostName),
    m_endpoint(endpoint.empty() ? "127.0.0.1:24810" : endpoint),
    m_host(hostName),
    m_x(0),
    m_y(0),
    m_socket(invalidSocket())
{
}

BluetoothClientProxy::~BluetoothClientProxy()
{
    closeSocket();
}

void BluetoothClientProxy::setEndpoint(const std::string& endpoint)
{
    const std::string normalized = endpoint.empty() ? "127.0.0.1:24810" : endpoint;
    if (normalized == m_endpoint) {
        return;
    }
    closeSocket();
    m_endpoint = normalized;
}

void BluetoothClientProxy::updateScreens(const std::vector<etherwaver::layout::Screen>& screens)
{
    m_screens.clear();
    for (std::vector<etherwaver::layout::Screen>::const_iterator it = screens.begin();
         it != screens.end(); ++it) {
        if (it->m_hostId != m_host) {
            continue;
        }
        const int width = it->m_width > 1 ? it->m_width : 1;
        const int height = it->m_height > 1 ? it->m_height : 1;
        m_screens.push_back(ClientScreenInfo(it->m_name.empty() ? it->m_id : it->m_name,
                                             it->m_x, it->m_y, width, height));
    }
}

void* BluetoothClientProxy::getEventTarget() const { return const_cast<BluetoothClientProxy*>(this); }
bool BluetoothClientProxy::getClipboard(ClipboardID, IClipboard*) const { return false; }

void BluetoothClientProxy::getShape(SInt32& x, SInt32& y, SInt32& width, SInt32& height) const
{
    if (m_screens.empty()) {
        x = 0; y = 0; width = 240; height = 140;
        return;
    }

    x = m_screens.front().m_x;
    y = m_screens.front().m_y;
    SInt32 right = x + m_screens.front().m_w;
    SInt32 bottom = y + m_screens.front().m_h;
    for (std::vector<ClientScreenInfo>::const_iterator it = m_screens.begin();
         it != m_screens.end(); ++it) {
        x = x < it->m_x ? x : it->m_x;
        y = y < it->m_y ? y : it->m_y;
        right = right > it->m_x + it->m_w ? right : it->m_x + it->m_w;
        bottom = bottom > it->m_y + it->m_h ? bottom : it->m_y + it->m_h;
    }
    width = (right - x) > 1 ? (right - x) : 1;
    height = (bottom - y) > 1 ? (bottom - y) : 1;
}

void BluetoothClientProxy::getScreens(std::vector<ClientScreenInfo>& screens) const { screens = m_screens; }
void BluetoothClientProxy::getCursorPos(SInt32& x, SInt32& y) const { x = m_x; y = m_y; }

void BluetoothClientProxy::enter(SInt32 xAbs, SInt32 yAbs, UInt32 seqNum, KeyModifierMask mask, bool forScreensaver)
{
    m_x = xAbs;
    m_y = yAbs;
    std::ostringstream line;
    line << "enter " << xAbs << " " << yAbs << " " << seqNum << " " << mask << " " << boolValue(forScreensaver);
    sendLine(line.str());
}

bool BluetoothClientProxy::leave()
{
    sendLine("leave");
    return true;
}

void BluetoothClientProxy::setClipboard(ClipboardID, const IClipboard*) {}
void BluetoothClientProxy::grabClipboard(ClipboardID) {}
void BluetoothClientProxy::setClipboardDirty(ClipboardID, bool) {}

void BluetoothClientProxy::keyDown(KeyID id, KeyModifierMask mask, KeyButton button)
{
    std::ostringstream line; line << "kd " << id << " " << mask << " " << button; sendLine(line.str());
}

void BluetoothClientProxy::keyRepeat(KeyID id, KeyModifierMask mask, SInt32 count, KeyButton button)
{
    std::ostringstream line; line << "kr " << id << " " << mask << " " << count << " " << button; sendLine(line.str());
}

void BluetoothClientProxy::keyUp(KeyID id, KeyModifierMask mask, KeyButton button)
{
    std::ostringstream line; line << "ku " << id << " " << mask << " " << button; sendLine(line.str());
}

void BluetoothClientProxy::mouseDown(ButtonID id)
{
    std::ostringstream line; line << "md " << id; sendLine(line.str());
}

void BluetoothClientProxy::mouseUp(ButtonID id)
{
    std::ostringstream line; line << "mu " << id; sendLine(line.str());
}

void BluetoothClientProxy::mouseMove(SInt32 xAbs, SInt32 yAbs)
{
    m_x = xAbs;
    m_y = yAbs;
    std::ostringstream line; line << "mm " << xAbs << " " << yAbs; sendLine(line.str());
}

void BluetoothClientProxy::mouseRelativeMove(SInt32 xRel, SInt32 yRel)
{
    m_x += xRel;
    m_y += yRel;
    std::ostringstream line; line << "mr " << xRel << " " << yRel; sendLine(line.str());
}

void BluetoothClientProxy::mouseWheel(SInt32 xDelta, SInt32 yDelta)
{
    std::ostringstream line; line << "mw " << xDelta << " " << yDelta; sendLine(line.str());
}

void BluetoothClientProxy::screensaver(bool activate)
{
    std::ostringstream line; line << "screensaver " << boolValue(activate); sendLine(line.str());
}

void BluetoothClientProxy::resetOptions() {}
void BluetoothClientProxy::setOptions(const OptionsList&) {}
void BluetoothClientProxy::sendDragInfo(UInt32, const char*, size_t) {}
void BluetoothClientProxy::fileChunkSending(UInt8, char*, size_t) {}
barrier::IStream* BluetoothClientProxy::getStream() const { return NULL; }

bool BluetoothClientProxy::sendLine(const std::string& line)
{
    if (!ensureConnected()) {
        return false;
    }

    const std::string payload = line + "\n";
    const char* data = payload.c_str();
    size_t left = payload.size();
    while (left > 0) {
        const int kMaxSendChunk = 0x7fffffff;
        const int chunk = left > static_cast<size_t>(kMaxSendChunk)
            ? kMaxSendChunk
            : static_cast<int>(left);
#if SYSAPI_WIN32
        const int written = ::send(static_cast<SOCKET>(m_socket), data, chunk, 0);
        if (written == SOCKET_ERROR || written == 0) {
            LOG((CLOG_WARN "bluetooth bridge send failed host=%s endpoint=%s",
                 m_host.c_str(), m_endpoint.c_str()));
            closeSocket();
            return false;
        }
#else
        const ssize_t written = ::send(static_cast<int>(m_socket), data, chunk, 0);
        if (written <= 0) {
            LOG((CLOG_WARN "bluetooth bridge send failed host=%s endpoint=%s",
                 m_host.c_str(), m_endpoint.c_str()));
            closeSocket();
            return false;
        }
#endif
        data += written;
        left -= static_cast<size_t>(written);
    }
    return true;
}

bool BluetoothClientProxy::ensureConnected()
{
    if (m_socket != invalidSocket()) {
        return true;
    }

#if SYSAPI_WIN32
    if (!ensureWinsockStarted()) {
        LOG((CLOG_WARN "bluetooth bridge winsock startup failed host=%s endpoint=%s",
             m_host.c_str(), m_endpoint.c_str()));
        return false;
    }
#endif

    std::string host;
    std::string port;
    splitEndpoint(m_endpoint, host, port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result = NULL;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &result) != 0) {
        LOG((CLOG_WARN "bluetooth bridge resolve failed host=%s endpoint=%s",
             m_host.c_str(), m_endpoint.c_str()));
        return false;
    }

    for (struct addrinfo* it = result; it != NULL; it = it->ai_next) {
#if SYSAPI_WIN32
        const SOCKET fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd == INVALID_SOCKET) {
            continue;
        }
        if (::connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
            m_socket = static_cast<intptr_t>(fd);
            freeaddrinfo(result);
            std::ostringstream hello;
            hello << "hello " << m_host;
            sendLine(hello.str());
            LOG((CLOG_NOTE "bluetooth bridge connected host=%s endpoint=%s",
                 m_host.c_str(), m_endpoint.c_str()));
            return true;
        }
        ::closesocket(fd);
#else
        const int fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (::connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
            m_socket = static_cast<intptr_t>(fd);
            freeaddrinfo(result);
            std::ostringstream hello;
            hello << "hello " << m_host;
            sendLine(hello.str());
            LOG((CLOG_NOTE "bluetooth bridge connected host=%s endpoint=%s",
                 m_host.c_str(), m_endpoint.c_str()));
            return true;
        }
        ::close(fd);
#endif
    }

    freeaddrinfo(result);
    LOG((CLOG_WARN "bluetooth bridge connect failed host=%s endpoint=%s",
         m_host.c_str(), m_endpoint.c_str()));
    return false;
}

void BluetoothClientProxy::closeSocket()
{
    if (m_socket != invalidSocket()) {
#if SYSAPI_WIN32
        ::closesocket(static_cast<SOCKET>(m_socket));
#else
        ::close(static_cast<int>(m_socket));
#endif
    }
    m_socket = invalidSocket();
}
