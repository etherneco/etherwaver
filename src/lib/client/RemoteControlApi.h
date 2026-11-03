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

#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>

//! Local HTTP control endpoint of the client (remote control)
/*!
Listens on 127.0.0.1 only.  Every request must carry the token from the
token file (header "X-Etherwaver-Token") which is created with owner-only
permissions in the profile directory, so other users and web pages
cannot drive it.  Used by the GUI; also usable with curl:

  GET  /remote-control                      status (JSON)
  POST /remote-control/start?mode=capture|window&target=<host>&takeover=1
  POST /remote-control/stop
  POST /remote-control/ctrl-alt-del

Commands run on the main thread through \c Dispatcher.
*/
class Thread;

class RemoteControlApi {
public:
    //! Runs \p job on the main thread and returns its JSON answer.
    //! Implementations may time out and return an error.
    typedef std::function<std::string(const std::string& method, const std::string& path,
                                      const std::map<std::string, std::string>& query)> Handler;

    RemoteControlApi(int port, Handler handler);
    ~RemoteControlApi();

    bool                start(std::string& error);
    void                stop();

    const std::string&  token() const { return m_token; }
    int                 port() const { return m_port; }
    //! where the port and token are published for the GUI
    static std::string  tokenFilePath();

    // exposed for unit tests
    static bool         parseRequest(const std::string& request, std::string& method,
                            std::string& path, std::map<std::string, std::string>& query,
                            std::map<std::string, std::string>& headers);

private:
    void                loop();
    void                serve(void* socket);
    bool                writeTokenFile(std::string& error);

private:
    int                 m_port;
    Handler             m_handler;
    std::string         m_token;
    void*               m_listener;
    std::atomic<bool>   m_running;
    Thread*             m_thread; // barrier thread: ARCH sockets need one
};
