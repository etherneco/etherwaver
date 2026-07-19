#include "platform/BluetoothBridgeDaemon.h"

#include "base/Log.h"

#include <cstdlib>
#include <cstring>
#include <sstream>
#include <vector>

#if SYSAPI_UNIX
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

void splitAddress(const std::string& address, std::string& host, std::string& port)
{
    const std::string::size_type colon = address.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= address.size()) {
        host = "127.0.0.1";
        port = "24810";
        return;
    }
    host = address.substr(0, colon);
    port = address.substr(colon + 1);
}

std::vector<std::string> splitWords(const std::string& line)
{
    std::istringstream input(line);
    std::vector<std::string> words;
    std::string word;
    while (input >> word) {
        words.push_back(word);
    }
    return words;
}

long toLong(const std::string& value)
{
    return std::strtol(value.c_str(), NULL, 10);
}

}

BluetoothBridgeDaemon::Config::Config() :
    m_listenAddress("127.0.0.1:24810"),
    m_deviceName("EtherWaver Bluetooth HID"),
    m_noUhid(false)
{
}

BluetoothBridgeDaemon::BluetoothBridgeDaemon()
{
}

int BluetoothBridgeDaemon::run(int argc, char** argv)
{
    if (!parseArgs(argc, argv)) {
        return 1;
    }
    return serve();
}

bool BluetoothBridgeDaemon::parseArgs(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--listen") == 0 && i + 1 < argc) {
            m_config.m_listenAddress = argv[++i];
        }
        else if (std::strncmp(argv[i], "--listen=", 9) == 0) {
            m_config.m_listenAddress = argv[i] + 9;
        }
        else if (std::strcmp(argv[i], "--device-name") == 0 && i + 1 < argc) {
            m_config.m_deviceName = argv[++i];
        }
        else if (std::strncmp(argv[i], "--device-name=", 14) == 0) {
            m_config.m_deviceName = argv[i] + 14;
        }
        else if (std::strcmp(argv[i], "--no-uhid") == 0) {
            m_config.m_noUhid = true;
        }
        else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            printUsage(argv[0]);
            return false;
        }
        else {
            LOG((CLOG_PRINT "%s: unrecognized option `%s'\n", argv[0], argv[i]));
            return false;
        }
    }

    return true;
}

void BluetoothBridgeDaemon::printUsage(const char* argv0) const
{
    LOG((CLOG_PRINT "Usage: %s [--listen host:port] [--device-name name] [--no-uhid]\n", argv0));
}

bool BluetoothBridgeDaemon::startBackend()
{
    if (m_config.m_noUhid) {
        return true;
    }
    if (m_uhid.running()) {
        return true;
    }
    if (!m_uhid.start(m_config.m_deviceName)) {
        LOG((CLOG_WARN "waverb: UHID backend failed, events will be logged only"));
        return false;
    }
    return true;
}

int BluetoothBridgeDaemon::serve()
{
#if !SYSAPI_UNIX
    LOG((CLOG_ERR "waverb is available on Linux/Unix only"));
    return 1;
#else
    startBackend();

    std::string host;
    std::string port;
    splitAddress(m_config.m_listenAddress, host, port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    struct addrinfo* result = NULL;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &result) != 0) {
        LOG((CLOG_ERR "waverb: failed to resolve listen address %s", m_config.m_listenAddress.c_str()));
        return 1;
    }

    int listenFd = -1;
    for (struct addrinfo* it = result; it != NULL; it = it->ai_next) {
        listenFd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (listenFd < 0) {
            continue;
        }
        int yes = 1;
        setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if (::bind(listenFd, it->ai_addr, it->ai_addrlen) == 0 &&
            ::listen(listenFd, 4) == 0) {
            break;
        }
        ::close(listenFd);
        listenFd = -1;
    }
    freeaddrinfo(result);

    if (listenFd < 0) {
        LOG((CLOG_ERR "waverb: failed to listen on %s", m_config.m_listenAddress.c_str()));
        return 1;
    }

    LOG((CLOG_NOTE "waverb: listening on %s", m_config.m_listenAddress.c_str()));
    for (;;) {
        const int clientFd = ::accept(listenFd, NULL, NULL);
        if (clientFd < 0) {
            continue;
        }
        handleClient(clientFd);
        ::close(clientFd);
    }
#endif
}

void BluetoothBridgeDaemon::handleClient(int clientFd)
{
#if SYSAPI_UNIX
    std::string buffer;
    char chunk[512];
    for (;;) {
        const ssize_t bytes = ::recv(clientFd, chunk, sizeof(chunk), 0);
        if (bytes <= 0) {
            break;
        }
        buffer.append(chunk, chunk + bytes);
        for (;;) {
            const std::string::size_type newline = buffer.find('\n');
            if (newline == std::string::npos) {
                break;
            }
            const std::string line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            handleLine(line);
        }
    }
#else
    (void)clientFd;
#endif
}

void BluetoothBridgeDaemon::handleLine(const std::string& line)
{
    const std::vector<std::string> words = splitWords(line);
    if (words.empty()) {
        return;
    }

    if (words[0] == "hello" && words.size() >= 2) {
        LOG((CLOG_NOTE "waverb: server attached bluetooth-host=%s", words[1].c_str()));
        return;
    }

    if (m_config.m_noUhid || !m_uhid.running()) {
        LOG((CLOG_DEBUG "waverb: %s", line.c_str()));
        return;
    }

    if (words[0] == "kd" && words.size() >= 4) {
        m_uhid.keyDown(static_cast<KeyID>(toLong(words[1])),
                       static_cast<KeyModifierMask>(toLong(words[2])));
    }
    else if (words[0] == "ku" && words.size() >= 4) {
        m_uhid.keyUp(static_cast<KeyID>(toLong(words[1])),
                     static_cast<KeyModifierMask>(toLong(words[2])));
    }
    else if (words[0] == "kr" && words.size() >= 5) {
        m_uhid.keyRepeat(static_cast<KeyID>(toLong(words[1])),
                         static_cast<KeyModifierMask>(toLong(words[2])),
                         static_cast<SInt32>(toLong(words[3])));
    }
    else if (words[0] == "md" && words.size() >= 2) {
        m_uhid.mouseDown(static_cast<ButtonID>(toLong(words[1])));
    }
    else if (words[0] == "mu" && words.size() >= 2) {
        m_uhid.mouseUp(static_cast<ButtonID>(toLong(words[1])));
    }
    else if (words[0] == "mm" && words.size() >= 3) {
        m_uhid.mouseMoveAbsolute(static_cast<SInt32>(toLong(words[1])),
                                 static_cast<SInt32>(toLong(words[2])));
    }
    else if (words[0] == "mr" && words.size() >= 3) {
        m_uhid.mouseRelativeMove(static_cast<SInt32>(toLong(words[1])),
                                 static_cast<SInt32>(toLong(words[2])));
    }
    else if (words[0] == "mw" && words.size() >= 3) {
        m_uhid.mouseWheel(static_cast<SInt32>(toLong(words[1])),
                          static_cast<SInt32>(toLong(words[2])));
    }
}
