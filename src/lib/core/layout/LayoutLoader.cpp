#include "core/layout/LayoutLoader.h"

#include "server/Config.h"

#include <fstream>
#include <algorithm>
#include <map>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <cstdlib>
#include <cstring>

namespace {

class JsonParser {
public:
    explicit JsonParser(const std::string& input) :
        m_input(input),
        m_pos(0)
    {
        if (m_input.size() >= 3 &&
            static_cast<unsigned char>(m_input[0]) == 0xEF &&
            static_cast<unsigned char>(m_input[1]) == 0xBB &&
            static_cast<unsigned char>(m_input[2]) == 0xBF) {
            m_pos = 3;
        }
    }

    etherwaver::layout::ScreenManager parseLayout()
    {
        etherwaver::layout::ScreenManager manager;
        std::vector<etherwaver::layout::Screen> screens;

        skipWhitespace();
        expect('{');
        for (;;) {
            skipWhitespace();
            if (peek('}')) {
                expect('}');
                break;
            }

            const std::string key = parseString();
            skipWhitespace();
            expect(':');
            skipWhitespace();

            if (key == "screens") {
                screens = parseScreens();
            }
            else {
                skipValue();
            }

            skipWhitespace();
            if (peek(',')) {
                expect(',');
                continue;
            }
            if (peek('}')) {
                expect('}');
                break;
            }
            throw std::runtime_error("invalid layout JSON object");
        }

        manager.setScreens(screens);
        return manager;
    }

private:
    std::vector<etherwaver::layout::Screen> parseScreens()
    {
        std::vector<etherwaver::layout::Screen> screens;
        expect('[');

        for (;;) {
            skipWhitespace();
            if (peek(']')) {
                expect(']');
                break;
            }

            screens.push_back(parseScreen());

            skipWhitespace();
            if (peek(',')) {
                expect(',');
                continue;
            }
            if (peek(']')) {
                expect(']');
                break;
            }
            throw std::runtime_error("invalid screens array");
        }

        return screens;
    }

    etherwaver::layout::Screen parseScreen()
    {
        std::string id;
        std::string host;
        std::string name;
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;

        expect('{');
        for (;;) {
            skipWhitespace();
            if (peek('}')) {
                expect('}');
                break;
            }

            const std::string key = parseString();
            skipWhitespace();
            expect(':');
            skipWhitespace();

            if (key == "id") {
                id = parseString();
            }
            else if (key == "host") {
                host = parseString();
            }
            else if (key == "name") {
                name = parseString();
            }
            else if (key == "x") {
                x = parseInt();
            }
            else if (key == "y") {
                y = parseInt();
            }
            else if (key == "width") {
                width = parseInt();
            }
            else if (key == "height") {
                height = parseInt();
            }
            else {
                skipValue();
            }

            skipWhitespace();
            if (peek(',')) {
                expect(',');
                continue;
            }
            if (peek('}')) {
                expect('}');
                break;
            }
            throw std::runtime_error("invalid screen object");
        }

        if (id.empty() || host.empty() || width <= 0 || height <= 0) {
            throw std::runtime_error("screen object is missing required fields");
        }

        return etherwaver::layout::Screen(id, host, name, x, y, width, height);
    }

    void skipValue()
    {
        skipWhitespace();
        if (peek('"')) {
            parseString();
            return;
        }
        if (peek('{')) {
            skipObject();
            return;
        }
        if (peek('[')) {
            skipArray();
            return;
        }
        if (peek('-') || isDigit(current())) {
            skipNumber();
            return;
        }
        if (matchKeyword("true") || matchKeyword("false") || matchKeyword("null")) {
            return;
        }
        throw std::runtime_error("unsupported JSON value");
    }

    void skipObject()
    {
        expect('{');
        for (;;) {
            skipWhitespace();
            if (peek('}')) {
                expect('}');
                return;
            }

            parseString();
            skipWhitespace();
            expect(':');
            skipValue();
            skipWhitespace();

            if (peek(',')) {
                expect(',');
                continue;
            }
            if (peek('}')) {
                expect('}');
                return;
            }
            throw std::runtime_error("invalid JSON object");
        }
    }

    void skipArray()
    {
        expect('[');
        for (;;) {
            skipWhitespace();
            if (peek(']')) {
                expect(']');
                return;
            }

            skipValue();
            skipWhitespace();

            if (peek(',')) {
                expect(',');
                continue;
            }
            if (peek(']')) {
                expect(']');
                return;
            }
            throw std::runtime_error("invalid JSON array");
        }
    }

    std::string parseString()
    {
        expect('"');
        std::string value;
        while (m_pos < m_input.size()) {
            const char c = m_input[m_pos++];
            if (c == '"') {
                return value;
            }
            if (c == '\\') {
                if (m_pos >= m_input.size()) {
                    throw std::runtime_error("unterminated JSON escape");
                }
                const char escaped = m_input[m_pos++];
                switch (escaped) {
                case '"':
                case '\\':
                case '/':
                    value.push_back(escaped);
                    break;
                case 'b':
                    value.push_back('\b');
                    break;
                case 'f':
                    value.push_back('\f');
                    break;
                case 'n':
                    value.push_back('\n');
                    break;
                case 'r':
                    value.push_back('\r');
                    break;
                case 't':
                    value.push_back('\t');
                    break;
                default:
                    throw std::runtime_error("unsupported JSON escape");
                }
                continue;
            }
            value.push_back(c);
        }
        throw std::runtime_error("unterminated JSON string");
    }

    int parseInt()
    {
        skipWhitespace();
        const size_t start = m_pos;
        if (peek('-')) {
            ++m_pos;
        }
        if (!isDigit(current())) {
            throw std::runtime_error("expected integer");
        }
        while (isDigit(current())) {
            ++m_pos;
        }
        return std::atoi(m_input.substr(start, m_pos - start).c_str());
    }

    void skipNumber()
    {
        skipWhitespace();
        if (peek('-')) {
            ++m_pos;
        }
        if (!isDigit(current())) {
            throw std::runtime_error("expected number");
        }
        while (isDigit(current())) {
            ++m_pos;
        }
        if (current() == '.') {
            ++m_pos;
            if (!isDigit(current())) {
                throw std::runtime_error("expected number");
            }
            while (isDigit(current())) {
                ++m_pos;
            }
        }
        if (current() == 'e' || current() == 'E') {
            ++m_pos;
            if (current() == '+' || current() == '-') {
                ++m_pos;
            }
            if (!isDigit(current())) {
                throw std::runtime_error("expected number");
            }
            while (isDigit(current())) {
                ++m_pos;
            }
        }
    }

    bool matchKeyword(const char* keyword)
    {
        size_t len = std::strlen(keyword);
        if (m_input.compare(m_pos, len, keyword) == 0) {
            m_pos += len;
            return true;
        }
        return false;
    }

    void skipWhitespace()
    {
        while (m_pos < m_input.size()) {
            char c = m_input[m_pos];
            if (c != ' ' && c != '\n' && c != '\r' && c != '\t') {
                break;
            }
            ++m_pos;
        }
    }

    void expect(char c)
    {
        skipWhitespace();
        if (m_pos >= m_input.size() || m_input[m_pos] != c) {
            throw std::runtime_error("unexpected JSON token");
        }
        ++m_pos;
    }

    bool peek(char c)
    {
        skipWhitespace();
        return (m_pos < m_input.size() && m_input[m_pos] == c);
    }

    char current() const
    {
        if (m_pos >= m_input.size()) {
            return '\0';
        }
        return m_input[m_pos];
    }

    static bool isDigit(char c)
    {
        return (c >= '0' && c <= '9');
    }

private:
    const std::string& m_input;
    size_t m_pos;
};

static etherwaver::layout::HostGeometry
getGeometryForHost(const std::map<std::string, etherwaver::layout::HostGeometry>& hostGeometries,
                   const std::string& hostId)
{
    std::map<std::string, etherwaver::layout::HostGeometry>::const_iterator it =
        hostGeometries.find(hostId);
    if (it != hostGeometries.end()) {
        return it->second;
    }
    return etherwaver::layout::HostGeometry(0, 0, 1920, 1080);
}

static std::vector<ClientScreenInfo>
getScreensForHost(const std::map<std::string, std::vector<ClientScreenInfo> >& hostScreens,
                  const std::map<std::string, etherwaver::layout::HostGeometry>& hostGeometries,
                  const std::string& hostId)
{
    std::map<std::string, std::vector<ClientScreenInfo> >::const_iterator it =
        hostScreens.find(hostId);
    if (it != hostScreens.end() && !it->second.empty()) {
        return it->second;
    }

    const etherwaver::layout::HostGeometry geometry = getGeometryForHost(hostGeometries, hostId);
    std::vector<ClientScreenInfo> screens;
    screens.push_back(ClientScreenInfo("screen0", 0, 0, geometry.m_width, geometry.m_height));
    return screens;
}

} // namespace

namespace etherwaver {
namespace layout {

HostGeometry::HostGeometry() :
    m_x(0),
    m_y(0),
    m_width(1920),
    m_height(1080)
{
}

HostGeometry::HostGeometry(int x, int y, int width, int height) :
    m_x(x),
    m_y(y),
    m_width(width),
    m_height(height)
{
}

ScreenManager
LayoutLoader::loadLayout(const std::string& layoutPath,
                         const Config& config,
                         const std::map<std::string, HostGeometry>& hostGeometries,
                         const std::map<std::string, std::vector<ClientScreenInfo> >& hostScreens,
                         const std::string& primaryHostId)
{
    std::ifstream stream(layoutPath.c_str());
    if (stream.good()) {
        try {
            return loadJsonLayout(layoutPath);
        }
        catch (const std::exception&) {
            // Fall back to the config-derived layout if the JSON file is
            // temporarily incomplete or otherwise unreadable.
        }
    }

    return convertConfigToObjectLayout(config, hostGeometries, hostScreens, primaryHostId);
}

ScreenManager
LayoutLoader::loadJsonLayout(const std::string& layoutPath)
{
    std::ifstream stream(layoutPath.c_str());
    if (!stream) {
        throw std::runtime_error("unable to open layout file");
    }

    std::stringstream buffer;
    buffer << stream.rdbuf();
    JsonParser parser(buffer.str());
    return parser.parseLayout();
}

ScreenManager
LayoutLoader::convertConfigToObjectLayout(const Config& config,
                                          const std::map<std::string, HostGeometry>& hostGeometries,
                                          const std::map<std::string, std::vector<ClientScreenInfo> >& hostScreens,
                                          const std::string& primaryHostId)
{
    std::map<std::string, std::pair<int, int> > positions;
    std::queue<std::string> pending;
    std::vector<Screen> screens;

    if (!primaryHostId.empty() && config.isScreen(primaryHostId)) {
        positions[primaryHostId] = std::make_pair(0, 0);
        pending.push(primaryHostId);
    }
    else if (config.begin() != config.end()) {
        const std::string first = *config.begin();
        positions[first] = std::make_pair(0, 0);
        pending.push(first);
    }

    while (!pending.empty()) {
        const std::string current = pending.front();
        pending.pop();

        const std::pair<int, int> currentPos = positions[current];
        static const EDirection directions[] = { kLeft, kRight, kTop, kBottom };
        for (size_t i = 0; i < sizeof(directions) / sizeof(directions[0]); ++i) {
            const EDirection direction = directions[i];
            const std::string neighbor = config.getNeighbor(current, direction, 0.5f, NULL);
            if (neighbor.empty() || positions.find(neighbor) != positions.end()) {
                continue;
            }

            std::pair<int, int> nextPos = currentPos;
            switch (direction) {
            case kLeft:
                --nextPos.first;
                break;
            case kRight:
                ++nextPos.first;
                break;
            case kTop:
                --nextPos.second;
                break;
            case kBottom:
                ++nextPos.second;
                break;
            default:
                break;
            }

            positions[neighbor] = nextPos;
            pending.push(neighbor);
        }
    }

    for (Config::const_iterator it = config.begin(); it != config.end(); ++it) {
        const std::string hostId = *it;
        if (positions.find(hostId) == positions.end()) {
            positions[hostId] = std::make_pair(static_cast<int>(positions.size()), 0);
        }

        const std::pair<int, int> grid = positions[hostId];
        const std::vector<ClientScreenInfo> hostScreenList =
            getScreensForHost(hostScreens, hostGeometries, hostId);

        int minX = hostScreenList.front().m_x;
        int minY = hostScreenList.front().m_y;
        int maxX = hostScreenList.front().m_x + hostScreenList.front().m_w;
        int maxY = hostScreenList.front().m_y + hostScreenList.front().m_h;
        for (std::vector<ClientScreenInfo>::const_iterator screen = hostScreenList.begin();
             screen != hostScreenList.end(); ++screen) {
            minX = std::min(minX, static_cast<int>(screen->m_x));
            minY = std::min(minY, static_cast<int>(screen->m_y));
            maxX = std::max(maxX, static_cast<int>(screen->m_x + screen->m_w));
            maxY = std::max(maxY, static_cast<int>(screen->m_y + screen->m_h));
        }

        const int hostWidth = maxX - minX;
        const int hostHeight = maxY - minY;
        const int hostOriginX = grid.first * hostWidth;
        const int hostOriginY = grid.second * hostHeight;

        for (std::vector<ClientScreenInfo>::const_iterator screen = hostScreenList.begin();
             screen != hostScreenList.end(); ++screen) {
            const std::string screenId = hostId + ":" + screen->m_id;
            screens.push_back(Screen(screenId, hostId, screenId,
                                     hostOriginX + (screen->m_x - minX),
                                     hostOriginY + (screen->m_y - minY),
                                     screen->m_w,
                                     screen->m_h));
        }
    }

    ScreenManager manager;
    manager.setScreens(screens);
    return manager;
}

} // namespace layout
} // namespace etherwaver
