#pragma once

#include "server/BaseClientProxy.h"
#include "barrier/protocol_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace etherwaver {
namespace layout {
class Screen;
}
}

class BluetoothClientProxy : public BaseClientProxy {
public:
    BluetoothClientProxy(const std::string& hostName, const std::string& endpoint);
    ~BluetoothClientProxy();

    void setEndpoint(const std::string& endpoint);
    void updateScreens(const std::vector<etherwaver::layout::Screen>& screens);

    void* getEventTarget() const override;
    bool getClipboard(ClipboardID id, IClipboard* clipboard) const override;
    void getShape(SInt32& x, SInt32& y, SInt32& width, SInt32& height) const override;
    void getScreens(std::vector<ClientScreenInfo>& screens) const override;
    void getCursorPos(SInt32& x, SInt32& y) const override;

    void enter(SInt32 xAbs, SInt32 yAbs, UInt32 seqNum, KeyModifierMask mask,
               bool forScreensaver) override;
    bool leave() override;
    void setClipboard(ClipboardID, const IClipboard*) override;
    void grabClipboard(ClipboardID) override;
    void setClipboardDirty(ClipboardID, bool) override;
    void keyDown(KeyID, KeyModifierMask, KeyButton) override;
    void keyRepeat(KeyID, KeyModifierMask, SInt32 count, KeyButton) override;
    void keyUp(KeyID, KeyModifierMask, KeyButton) override;
    void mouseDown(ButtonID) override;
    void mouseUp(ButtonID) override;
    void mouseMove(SInt32 xAbs, SInt32 yAbs) override;
    void mouseRelativeMove(SInt32 xRel, SInt32 yRel) override;
    void mouseWheel(SInt32 xDelta, SInt32 yDelta) override;
    void screensaver(bool activate) override;
    void resetOptions() override;
    void setOptions(const OptionsList& options) override;
    void sendDragInfo(UInt32 fileCount, const char* info, size_t size) override;
    void fileChunkSending(UInt8 mark, char* data, size_t dataSize) override;
    barrier::IStream* getStream() const override;

private:
    bool sendLine(const std::string& line);
    bool ensureConnected();
    void closeSocket();

private:
    std::string m_endpoint;
    std::string m_host;
    std::vector<ClientScreenInfo> m_screens;
    SInt32 m_x;
    SInt32 m_y;
    intptr_t m_socket;
};
