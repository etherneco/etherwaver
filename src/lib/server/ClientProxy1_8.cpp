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

#include "server/ClientProxy1_8.h"

#include "server/Server.h"
#include "barrier/ProtocolUtil.h"
#include "barrier/RemoteInput.h"
#include "barrier/protocol_types.h"
#include "base/Log.h"

#include <cstring>

ClientProxy1_8::ClientProxy1_8(const std::string& name, barrier::IStream* stream,
                               Server* server, IEventQueue* events) :
    ClientProxy1_6(name, stream, server, events)
{
}

ClientProxy1_8::~ClientProxy1_8()
{
}

bool
ClientProxy1_8::parseMessage(const UInt8* code)
{
    if (memcmp(code, kMsgDRemoteInput, 4) == 0) {
        return recvRemoteInput();
    }
    else if (memcmp(code, kMsgCRemoteControlRequest, 4) == 0) {
        return recvRemoteControlRequest();
    }
    else if (memcmp(code, kMsgCRemoteControlRelease, 4) == 0) {
        return recvRemoteControlRelease();
    }
    return ClientProxy1_6::parseMessage(code);
}

bool
ClientProxy1_8::recvRemoteControlRequest()
{
    UInt8 flags = 0;
    std::string target;
    if (!ProtocolUtil::readf(getStream(), kMsgCRemoteControlRequest + 4, &flags, &target)) {
        return false;
    }
    if (target.size() > 255) {
        return false;
    }
    if (m_server != NULL) {
        m_server->onRemoteControlRequest(this, (flags & 1) != 0, target);
    }
    return true;
}

bool
ClientProxy1_8::recvRemoteControlRelease()
{
    UInt32 epoch = 0;
    if (!ProtocolUtil::readf(getStream(), kMsgCRemoteControlRelease + 4, &epoch)) {
        return false;
    }
    if (m_server != NULL) {
        m_server->onRemoteControlRelease(this, epoch);
    }
    return true;
}

bool
ClientProxy1_8::recvRemoteInput()
{
    UInt32 epoch = 0;
    UInt8 kind = 0;
    UInt32 a = 0, b = 0, c = 0, d = 0;
    if (!ProtocolUtil::readf(getStream(), kMsgDRemoteInput + 4,
                             &epoch, &kind, &a, &b, &c, &d)) {
        return false;
    }
    // note: the content of input events is never logged
    if (m_server != NULL) {
        barrier::remote::Event event((barrier::remote::EventKind)kind,
                                     (SInt32)a, (SInt32)b, (SInt32)c, (SInt32)d);
        m_server->onRemoteInput(this, epoch, event);
    }
    return true;
}

void
ClientProxy1_8::sendRemoteControlState(int state, int reason, UInt32 epoch,
                                       SInt32 x, SInt32 y, SInt32 w, SInt32 h,
                                       UInt8 capabilities,
                                       const std::string& controller,
                                       const std::string& target)
{
    LOG((CLOG_DEBUG "send remote control state %s to \"%s\"",
         barrier::remote::controlStateName(state), getName().c_str()));
    ProtocolUtil::writef(getStream(), kMsgCRemoteControlState,
                         (UInt8)state, (UInt8)reason, epoch,
                         (UInt32)x, (UInt32)y, (UInt32)w, (UInt32)h,
                         capabilities, &controller, &target);
}
