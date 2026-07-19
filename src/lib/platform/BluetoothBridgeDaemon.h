#pragma once

#include "platform/UhidServer.h"

#include <string>

class BluetoothBridgeDaemon {
public:
    struct Config {
        Config();

        std::string m_listenAddress;
        std::string m_deviceName;
        bool m_noUhid;
    };

    BluetoothBridgeDaemon();
    int run(int argc, char** argv);

private:
    bool parseArgs(int argc, char** argv);
    void printUsage(const char* argv0) const;
    int serve();
    void handleClient(int clientFd);
    void handleLine(const std::string& line);
    bool startBackend();

private:
    Config m_config;
    UhidServer m_uhid;
};
