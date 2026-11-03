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

#include "server/ClientProxy1_6.h"

class Server;
class IEventQueue;

//! Proxy for client implementing protocol version 1.8 (remote control)
/*!
Version 1.7 only added a client message (kMsgDScreenList) that every
proxy already parses, so 1.7 clients use ClientProxy1_6.  This proxy
adds the remote control messages: the client may ask to control a host
and then forward its own keyboard and mouse input.
*/
class ClientProxy1_8 : public ClientProxy1_6 {
public:
    ClientProxy1_8(const std::string& name, barrier::IStream* adoptedStream, Server* server,
                   IEventQueue* events);
    ~ClientProxy1_8();

    virtual bool        supportsRemoteControl() const { return true; }
    virtual void        sendRemoteControlState(int state, int reason,
                            UInt32 epoch, SInt32 x, SInt32 y,
                            SInt32 w, SInt32 h, UInt8 capabilities,
                            const std::string& controller,
                            const std::string& target);

protected:
    virtual bool        parseMessage(const UInt8* code);

private:
    bool                recvRemoteControlRequest();
    bool                recvRemoteControlRelease();
    bool                recvRemoteInput();
};
