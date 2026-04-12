#pragma once

#include <initializer_list>
#include <string>
#include <vector>

class UhidKeyboardDevice {
public:
    UhidKeyboardDevice();
    ~UhidKeyboardDevice();

    bool start(const std::string& deviceName);
    void stop();
    bool running() const;

    void clear();
    bool handleKeyEvent(unsigned short code, int value);
    bool tapKey(unsigned short code);
    bool pressKey(unsigned short code);
    bool releaseKey(unsigned short code);
    bool pressChord(std::initializer_list<unsigned short> codes);
    bool releaseChord(std::initializer_list<unsigned short> codes);

private:
    bool sendReport() const;

private:
    int m_fd;
    unsigned char m_modifiers;
    unsigned char m_reserved;
    unsigned char m_keys[6];
};

class UhidMouseDevice {
public:
    UhidMouseDevice();
    ~UhidMouseDevice();

    bool start(const std::string& deviceName);
    void stop();
    bool running() const;

    void clear();
    bool handleButtonEvent(unsigned short code, int value);
    bool moveRelative(int dx, int dy);
    bool wheel(int xDelta, int yDelta);

private:
    bool sendReport(signed char dx, signed char dy, signed char wheel, signed char pan) const;

private:
    int m_fd;
    unsigned char m_buttons;
};
