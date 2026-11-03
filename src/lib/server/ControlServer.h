/*
 * Etherwaver -- keyboard and mouse sharing utility
 * Copyright (C) 2026 Etherneco Ltd.
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

/*
 * Etherwaver Control Protocol (ECP) v1
 * ------------------------------------
 * A local management channel used by the web panel backend (waver-web) and
 * other tools.  It replaces nothing: the legacy one-shot endpoint on port
 * 24802 keeps working for existing scripts.
 *
 *  - transport:  TCP, bound to 127.0.0.1:24803 by default
 *  - framing:    newline-delimited JSON (UTF-8), one object per line,
 *                at most kMaxLineBytes per line
 *  - greeting:   {"type":"hello","protocol":"etherwaver-control","version":1,...}
 *  - requests:   {"id":<int|string>,"cmd":"<name>", ...flat parameters...}
 *  - responses:  {"type":"response","id":<same>,"ok":true,"result":{...}}
 *                {"type":"response","id":<same>,"ok":false,
 *                 "error":{"code":"...","message":"..."}}
 *  - events:     {"type":"event","seq":<n>,"event":"<name>","data":{...}}
 *                sent only to connections that issued "subscribe".
 *  - auth:       first command must be {"cmd":"auth","token":"..."}; the
 *                token is stored in a file readable only by the user that
 *                runs the server (see --control-token-file).
 *
 * Commands are executed on the server's main event loop thread; the network
 * I/O runs on a dedicated thread, so a slow client never blocks input
 * handling.
 */

#include "arch/IArchMultithread.h"
#include "arch/IArchNetwork.h"
#include "base/ILogOutputter.h"

#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace etherwaver {
namespace control {

//! Minimal JSON helpers used by the control protocol.
struct JsonValue {
    enum Type { kNull, kBool, kNumber, kString, kOther };
    Type        m_type = kNull;
    std::string m_string;   // decoded string, or number text
    bool        m_bool = false;
    std::string m_raw;      // raw JSON literal as received
};

typedef std::map<std::string, JsonValue> JsonObject;

//! Parses a JSON object whose values are primitives.  Nested objects and
//! arrays are accepted but only kept as raw text (type kOther).
bool        parseFlatObject(const std::string& text, JsonObject& out, std::string& error);
std::string jsonEscape(const std::string& value);
std::string jsonString(const std::string& value);

//! A command received from an authenticated control connection.
struct Request {
    int         m_connectionId = 0;
    std::string m_rawId;        // JSON literal of the request id
    std::string m_command;
    JsonObject  m_params;
};

//! Implemented by the server; called on the control thread and must
//! hand the request over to the main thread.
class IRequestHandler {
public:
    virtual ~IRequestHandler() {}
    virtual void        postControlRequest(Request* request) = 0;
    virtual std::string controlServerName() const = 0;
};

class ControlServer {
public:
    static const size_t kMaxLineBytes = 1024 * 1024;
    static const size_t kMaxOutputBytes = 8 * 1024 * 1024;
    static const int    kMaxConnections = 16;
    static const int    kMaxPendingPerConnection = 16;

    ControlServer(IRequestHandler* handler,
                  const std::string& address,
                  const std::string& tokenFile);
    ~ControlServer();

    //! Binds and starts the I/O thread.  Returns false (and logs) on error.
    bool                start();
    void                stop();

    //! Thread-safe: queue a response for a request.
    void                respond(const Request& request, const std::string& resultJson);
    void                respondError(const Request& request,
                                     const std::string& code,
                                     const std::string& message);

    //! Thread-safe: broadcast an event to subscribed connections.
    //! \p dataJson must be a JSON object.
    void                publish(const std::string& event, const std::string& dataJson);

    //! Thread-safe: forward a log line to log subscribers.
    void                publishLog(int level, const char* message);

    const std::string&  tokenFile() const { return m_tokenFile; }
    const std::string&  address() const { return m_address; }

private:
    struct Connection {
        int         m_id = 0;
        ArchSocket  m_socket = nullptr;
        std::string m_in;
        std::string m_out;
        bool        m_authenticated = false;
        bool        m_subscribed = false;
        bool        m_logSubscribed = false;
        bool        m_closing = false;
        int         m_pending = 0;
        int         m_authFailures = 0;
    };

    void                ioLoop();
    static void*        threadMain(void* self);
    void                acceptConnections();
    bool                readConnection(Connection& c);
    bool                writeConnection(Connection& c);
    void                handleLine(Connection& c, const std::string& line);
    void                queueLocked(Connection& c, const std::string& line);
    void                queueById(int connectionId, const std::string& line, bool completesRequest);
    void                closeConnection(Connection& c);
    bool                loadOrCreateToken();
    std::string         helloLine() const;

private:
    IRequestHandler*    m_handler;
    std::string         m_address;
    std::string         m_tokenFile;
    std::string         m_token;
    ArchSocket          m_listener = nullptr;
    ArchThread          m_thread = nullptr;
    std::atomic<bool>   m_running;
    std::mutex          m_mutex;
    std::map<int, std::unique_ptr<Connection> > m_connections;
    int                 m_nextConnectionId = 1;
    unsigned long long  m_eventSeq = 0;
    std::string         m_instanceId;
};

//! Log outputter that forwards log lines to control log subscribers.
class ControlLogOutputter : public ILogOutputter {
public:
    explicit ControlLogOutputter(ControlServer* server) : m_server(server) {}
    void open(const char*) override {}
    void close() override {}
    void show(bool) override {}
    bool write(ELevel level, const char* message) override;
    void detach() { m_server = nullptr; }
private:
    std::atomic<ControlServer*> m_server;
};

} // namespace control
} // namespace etherwaver
