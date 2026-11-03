/*
 * Etherwaver -- remote control unit tests: wire format of the 1.8
 * messages (round trip through ProtocolUtil).
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "barrier/ProtocolUtil.h"
#include "barrier/protocol_types.h"
#include "barrier/RemoteInput.h"
#include "io/IStream.h"

#include "test/global/gtest.h"

#include <cstring>
#include <string>

namespace {

class MemoryStream : public barrier::IStream {
public:
    void close() { }
    UInt32 read(void* buffer, UInt32 n)
    {
        n = std::min<UInt32>(n, (UInt32)(m_data.size() - m_pos));
        if (buffer != NULL) {
            std::memcpy(buffer, m_data.data() + m_pos, n);
        }
        m_pos += n;
        return n;
    }
    void write(const void* buffer, UInt32 n) { m_data.append(static_cast<const char*>(buffer), n); }
    void flush() { }
    void shutdownInput() { }
    void shutdownOutput() { }
    void* getEventTarget() const { return NULL; }
    bool isReady() const { return m_pos < m_data.size(); }
    UInt32 getSize() const { return (UInt32)(m_data.size() - m_pos); }

    std::string m_data;
    size_t m_pos = 0;
};

} // namespace

TEST(RemoteControlProtocolTests, stateMessageWithTwoStringsRoundTrips)
{
    MemoryStream s;
    const std::string controller = "laptop", target = "rack-01";
    ProtocolUtil::writef(&s, kMsgCRemoteControlState, (UInt8)1, (UInt8)7, (UInt32)0xA1B2C3D4u,
                         (UInt32)(SInt32)-1920, (UInt32)0, (UInt32)1920, (UInt32)1080,
                         (UInt8)0x0f, &controller, &target);
    ASSERT_EQ(0, std::memcmp(s.m_data.data(), "RCST", 4));
    s.m_pos = 4;

    UInt8 state = 0, reason = 0, caps = 0;
    UInt32 epoch = 0, x = 0, y = 0, w = 0, h = 0;
    std::string c, t;
    ASSERT_TRUE(ProtocolUtil::readf(&s, kMsgCRemoteControlState + 4,
                                    &state, &reason, &epoch, &x, &y, &w, &h, &caps, &c, &t));
    EXPECT_EQ(1, state);
    EXPECT_EQ(7, reason);
    EXPECT_EQ(0xA1B2C3D4u, epoch);
    EXPECT_EQ(-1920, (SInt32)x);
    EXPECT_EQ(1920u, w);
    EXPECT_EQ(1080u, h);
    EXPECT_EQ(0x0f, caps);
    EXPECT_EQ(controller, c);
    EXPECT_EQ(target, t);
    EXPECT_EQ(s.m_data.size(), s.m_pos) << "no trailing bytes";
}

TEST(RemoteControlProtocolTests, inputAndRequestMessagesRoundTrip)
{
    MemoryStream s;
    const std::string target = "desk";
    ProtocolUtil::writef(&s, kMsgCRemoteControlRequest, (UInt8)1, &target);
    ProtocolUtil::writef(&s, kMsgDRemoteInput, (UInt32)42, (UInt8)barrier::remote::kEventMoveRel,
                         (UInt32)(SInt32)-5, (UInt32)7, (UInt32)0, (UInt32)0);
    ProtocolUtil::writef(&s, kMsgCRemoteControlRelease, (UInt32)42);

    UInt8 flags = 0;
    std::string t;
    s.m_pos = 4;
    ASSERT_TRUE(ProtocolUtil::readf(&s, kMsgCRemoteControlRequest + 4, &flags, &t));
    EXPECT_EQ(1, flags);
    EXPECT_EQ("desk", t);

    ASSERT_EQ(0, std::memcmp(s.m_data.data() + s.m_pos, "RINP", 4));
    s.m_pos += 4;
    UInt32 epoch = 0, a = 0, b = 0, c = 0, d = 0;
    UInt8 kind = 0;
    ASSERT_TRUE(ProtocolUtil::readf(&s, kMsgDRemoteInput + 4, &epoch, &kind, &a, &b, &c, &d));
    EXPECT_EQ(42u, epoch);
    EXPECT_EQ(barrier::remote::kEventMoveRel, kind);
    EXPECT_EQ(-5, (SInt32)a);
    EXPECT_EQ(7u, b);

    ASSERT_EQ(0, std::memcmp(s.m_data.data() + s.m_pos, "RCRL", 4));
    s.m_pos += 4;
    ASSERT_TRUE(ProtocolUtil::readf(&s, kMsgCRemoteControlRelease + 4, &epoch));
    EXPECT_EQ(42u, epoch);
}
