#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void printUsage(const char* exe)
{
    std::cerr
        << "Usage:\n"
        << "  " << exe << " pos\n"
        << "  " << exe << " left <pixels>\n"
        << "  " << exe << " right <pixels>\n"
        << "  " << exe << " up <pixels>\n"
        << "  " << exe << " down <pixels>\n"
        << "  " << exe << " move <dx> <dy>\n"
        << "  " << exe << " send <dx> <dy>    # relative SendInput event\n";
}

bool getCursor(POINT& point)
{
    if (!GetCursorPos(&point)) {
        std::cerr << "GetCursorPos failed: " << GetLastError() << "\n";
        return false;
    }
    return true;
}

bool moveCursor(int dx, int dy)
{
    POINT current = {};
    if (!getCursor(current)) {
        return false;
    }

    if (!SetCursorPos(current.x + dx, current.y + dy)) {
        std::cerr << "SetCursorPos failed: " << GetLastError() << "\n";
        return false;
    }

    return true;
}

bool sendRelativeMove(int dx, int dy)
{
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;

    if (SendInput(1, &input, sizeof(input)) != 1) {
        std::cerr << "SendInput failed: " << GetLastError() << "\n";
        return false;
    }

    return true;
}

int parseInt(const char* value)
{
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0') {
        throw std::string("invalid integer: ") + value;
    }
    return static_cast<int>(parsed);
}

}

int main(int argc, char** argv)
{
    if (argc < 2) {
        printUsage(argv[0]);
        return 2;
    }

    const std::string command = argv[1];
    POINT before = {};
    if (!getCursor(before)) {
        return 1;
    }

    if (command == "pos") {
        std::cout << "pos x=" << before.x << " y=" << before.y << "\n";
        return 0;
    }

    int dx = 0;
    int dy = 0;

    try {
        if (command == "left" && argc == 3) {
            dx = -parseInt(argv[2]);
        }
        else if (command == "right" && argc == 3) {
            dx = parseInt(argv[2]);
        }
        else if (command == "up" && argc == 3) {
            dy = -parseInt(argv[2]);
        }
        else if (command == "down" && argc == 3) {
            dy = parseInt(argv[2]);
        }
        else if (command == "move" && argc == 4) {
            dx = parseInt(argv[2]);
            dy = parseInt(argv[3]);
        }
        else if (command == "send" && argc == 4) {
            dx = parseInt(argv[2]);
            dy = parseInt(argv[3]);
        }
        else {
            printUsage(argv[0]);
            return 2;
        }
    }
    catch (const std::string& error) {
        std::cerr << error << "\n";
        return 2;
    }

    const bool useSendInput = command == "send";
    if (!(useSendInput ? sendRelativeMove(dx, dy) : moveCursor(dx, dy))) {
        return 1;
    }

    Sleep(50);

    POINT after = {};
    if (!getCursor(after)) {
        return 1;
    }

    std::cout
        << "before x=" << before.x << " y=" << before.y
        << " dx=" << dx << " dy=" << dy
        << " after x=" << after.x << " y=" << after.y
        << "\n";
    return 0;
}
