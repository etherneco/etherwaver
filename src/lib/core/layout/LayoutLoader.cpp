#include "core/layout/LayoutLoader.h"

#include "server/Config.h"

#include <fstream>
#include <algorithm>
#include <map>
#include <limits>
#include <queue>
#include <set>
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
        std::string leftLink;
        std::string rightLink;
        std::string topLink;
        std::string bottomLink;
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
            else if (key == "links") {
                parseLinks(leftLink, rightLink, topLink, bottomLink);
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

        if (name.empty()) {
            if (id == host) {
                name = "screen0";
            }
            else {
                const std::string hostPrefix = host + ":";
                if (id.compare(0, hostPrefix.size(), hostPrefix) == 0) {
                    name = id.substr(hostPrefix.size());
                }
            }
        }

        if (id == host) {
            id = host + ":" + (name.empty() ? "screen0" : name);
        }
        else if (id.find(':') == std::string::npos && !name.empty()) {
            id = host + ":" + name;
        }

        return etherwaver::layout::Screen(id, host, name, x, y, width, height,
                                          leftLink, rightLink, topLink, bottomLink);
    }

    void parseLinks(std::string& leftLink,
                    std::string& rightLink,
                    std::string& topLink,
                    std::string& bottomLink)
    {
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

            std::string value;
            if (peek('"')) {
                value = parseString();
            }
            else if (matchKeyword("null")) {
                value.clear();
            }
            else {
                skipValue();
            }

            if (key == "left") {
                leftLink = value;
            }
            else if (key == "right") {
                rightLink = value;
            }
            else if (key == "up") {
                topLink = value;
            }
            else if (key == "down") {
                bottomLink = value;
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
            throw std::runtime_error("invalid links object");
        }
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
            std::stringstream message;
            message << "unexpected JSON token at offset " << m_pos
                    << ": expected '" << c << "'";
            if (m_pos < m_input.size()) {
                message << " got '" << m_input[m_pos] << "'";
            }
            else {
                message << " got end of input";
            }
            throw std::runtime_error(message.str());
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
    const std::string m_input;
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

static bool
isScreenIndexSuffix(const std::string& name, std::string& baseName)
{
    baseName.clear();
    if (name.empty()) {
        return false;
    }

    const std::string::size_type dash = name.rfind('-');
    if (dash == std::string::npos || dash == 0 || dash + 1 >= name.size()) {
        return false;
    }

    for (std::string::size_type i = dash + 1; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') {
            return false;
        }
    }

    baseName = name.substr(0, dash);
    return !baseName.empty();
}

static std::string
baseHostName(const std::string& name)
{
    std::string candidate = name;
    std::string baseName;
    while (isScreenIndexSuffix(candidate, baseName)) {
        candidate = baseName;
    }
    return candidate;
}

template <typename T>
static std::string
resolveRuntimeHostId(const std::map<std::string, T>& values, const std::string& hostId)
{
    if (values.find(hostId) != values.end()) {
        return hostId;
    }

    const std::string hostBase = baseHostName(hostId);
    if (hostBase != hostId && values.find(hostBase) != values.end()) {
        return hostBase;
    }

    std::string resolved;
    for (typename std::map<std::string, T>::const_iterator it = values.begin();
         it != values.end(); ++it) {
        if (baseHostName(it->first) != hostBase) {
            continue;
        }

        if (!resolved.empty()) {
            return hostId;
        }
        resolved = it->first;
    }

    return resolved.empty() ? hostId : resolved;
}

static std::vector<ClientScreenInfo>
getScreensForHost(const std::map<std::string, std::vector<ClientScreenInfo> >& hostScreens,
                  const std::map<std::string, etherwaver::layout::HostGeometry>& hostGeometries,
                  const std::string& hostId)
{
    const std::string resolvedHostId = resolveRuntimeHostId(hostScreens, hostId);
    std::map<std::string, std::vector<ClientScreenInfo> >::const_iterator it =
        hostScreens.find(resolvedHostId);
    if (it != hostScreens.end() && !it->second.empty()) {
        return it->second;
    }

    const etherwaver::layout::HostGeometry geometry =
        getGeometryForHost(hostGeometries, resolveRuntimeHostId(hostGeometries, hostId));
    std::vector<ClientScreenInfo> screens;
    screens.push_back(ClientScreenInfo("screen0", 0, 0, geometry.m_width, geometry.m_height));
    return screens;
}

static std::vector<etherwaver::layout::Screen>
getLayoutScreensForHost(const etherwaver::layout::ScreenManager& manager,
                        const std::string& hostId)
{
    std::vector<etherwaver::layout::Screen> screens;
    const std::vector<etherwaver::layout::Screen>& allScreens = manager.getScreens();
    for (std::vector<etherwaver::layout::Screen>::const_iterator it = allScreens.begin();
         it != allScreens.end(); ++it) {
        if (it->m_hostId == hostId) {
            screens.push_back(*it);
        }
    }
    return screens;
}

static bool
findClientScreenById(const std::vector<ClientScreenInfo>& screens,
                     const std::string& screenId,
                     ClientScreenInfo& screenInfo)
{
    for (std::vector<ClientScreenInfo>::const_iterator it = screens.begin();
         it != screens.end(); ++it) {
        if (it->m_id == screenId) {
            screenInfo = *it;
            return true;
        }
    }

    return false;
}

static bool
getNamedScreenForConfigEntry(
    const std::map<std::string, std::vector<ClientScreenInfo> >& hostScreens,
    const std::map<std::string, etherwaver::layout::HostGeometry>& hostGeometries,
    const std::string& configName,
    std::string& hostId,
    ClientScreenInfo& screenInfo)
{
    hostId = baseHostName(configName);
    if (hostId == configName) {
        return false;
    }

    const std::vector<ClientScreenInfo> actualScreens =
        getScreensForHost(hostScreens, hostGeometries, hostId);
    if (findClientScreenById(actualScreens, configName, screenInfo)) {
        return true;
    }

    return false;
}

static bool
configUsesLogicalScreenNames(
    const Config& config,
    const std::map<std::string, std::vector<ClientScreenInfo> >& hostScreens,
    const std::map<std::string, etherwaver::layout::HostGeometry>& hostGeometries)
{
    std::map<std::string, int> indexedHosts;
    bool foundIndexedConfigName = false;

    for (Config::const_iterator it = config.begin(); it != config.end(); ++it) {
        const std::string baseName = baseHostName(*it);
        if (baseName != *it) {
            foundIndexedConfigName = true;
            ++indexedHosts[baseName];
        }

        std::string hostId;
        ClientScreenInfo screenInfo;
        if (getNamedScreenForConfigEntry(hostScreens, hostGeometries, *it,
                                         hostId, screenInfo)) {
            return true;
        }
    }

    for (std::map<std::string, int>::const_iterator it = indexedHosts.begin();
         it != indexedHosts.end(); ++it) {
        if (it->second > 1) {
            return true;
        }
    }

    if (!foundIndexedConfigName) {
        return false;
    }

    for (Config::const_iterator it = config.begin(); it != config.end(); ++it) {
        const std::string baseName = baseHostName(*it);
        if (baseName == *it) {
            continue;
        }

        const std::string resolvedScreensHost = resolveRuntimeHostId(hostScreens, baseName);
        if (resolvedScreensHost != baseName || hostScreens.find(baseName) != hostScreens.end()) {
            return true;
        }

        const std::string resolvedGeometryHost =
            resolveRuntimeHostId(hostGeometries, baseName);
        if (resolvedGeometryHost != baseName ||
            hostGeometries.find(baseName) != hostGeometries.end()) {
            return true;
        }
    }

    return false;
}

static bool
hasRuntimeScreensForHost(const std::map<std::string, std::vector<ClientScreenInfo> >& hostScreens,
                         const std::string& hostId)
{
    const std::string resolvedHostId = resolveRuntimeHostId(hostScreens, hostId);
    std::map<std::string, std::vector<ClientScreenInfo> >::const_iterator it =
        hostScreens.find(resolvedHostId);
    return it != hostScreens.end() && !it->second.empty();
}

static bool
layoutScreenMatchesClientScreen(const etherwaver::layout::Screen& layoutScreen,
                                const ClientScreenInfo& clientScreen)
{
    if (layoutScreen.m_name == clientScreen.m_id ||
        layoutScreen.m_id == clientScreen.m_id) {
        return true;
    }

    const std::string hostPrefix = layoutScreen.m_hostId + ":";
    if (layoutScreen.m_id.compare(0, hostPrefix.size(), hostPrefix) == 0 &&
        layoutScreen.m_id.substr(hostPrefix.size()) == clientScreen.m_id) {
        return true;
    }

    return false;
}

static bool
linkMatchesScreen(const std::string& link, const etherwaver::layout::Screen& screen)
{
    return !link.empty() && (link == screen.m_id || link == screen.m_name);
}

static bool
isLinkedFromAnotherHost(const std::vector<etherwaver::layout::Screen>& screens,
                        const etherwaver::layout::Screen& target)
{
    for (std::vector<etherwaver::layout::Screen>::const_iterator it = screens.begin();
         it != screens.end(); ++it) {
        if (it->m_hostId == target.m_hostId) {
            continue;
        }

        if (linkMatchesScreen(it->m_leftLink, target) ||
            linkMatchesScreen(it->m_rightLink, target) ||
            linkMatchesScreen(it->m_topLink, target) ||
            linkMatchesScreen(it->m_bottomLink, target)) {
            return true;
        }
    }

    return false;
}

static bool
hasFirstScreenSuffix(const etherwaver::layout::Screen& screen)
{
    std::string baseName;
    return isScreenIndexSuffix(screen.m_name, baseName) &&
           screen.m_name.substr(baseName.size()) == "-1";
}

static bool
screenLinkExists(const std::vector<etherwaver::layout::Screen>& screens,
                 const std::string& link)
{
    if (link.empty()) {
        return true;
    }

    for (std::vector<etherwaver::layout::Screen>::const_iterator it = screens.begin();
         it != screens.end(); ++it) {
        if (linkMatchesScreen(link, *it)) {
            return true;
        }
    }

    return false;
}

static void
clearMissingLink(const std::vector<etherwaver::layout::Screen>& screens, std::string& link)
{
    if (!screenLinkExists(screens, link)) {
        link.clear();
    }
}

static void
clearLinksToMissingScreens(std::vector<etherwaver::layout::Screen>& screens)
{
    for (std::vector<etherwaver::layout::Screen>::iterator it = screens.begin();
         it != screens.end(); ++it) {
        clearMissingLink(screens, it->m_leftLink);
        clearMissingLink(screens, it->m_rightLink);
        clearMissingLink(screens, it->m_topLink);
        clearMissingLink(screens, it->m_bottomLink);
    }
}

struct HostScreenBounds {
    HostScreenBounds() :
        m_minX(0),
        m_minY(0),
        m_maxX(0),
        m_maxY(0)
    {
    }

    HostScreenBounds(int minX, int minY, int maxX, int maxY) :
        m_minX(minX),
        m_minY(minY),
        m_maxX(maxX),
        m_maxY(maxY)
    {
    }

    int width() const { return std::max(1, m_maxX - m_minX); }
    int height() const { return std::max(1, m_maxY - m_minY); }

    int m_minX;
    int m_minY;
    int m_maxX;
    int m_maxY;
};

static HostScreenBounds
getHostScreenBounds(const std::vector<ClientScreenInfo>& screens)
{
    if (screens.empty()) {
        return HostScreenBounds(0, 0, 1, 1);
    }

    int minX = screens.front().m_x;
    int minY = screens.front().m_y;
    int maxX = screens.front().m_x + screens.front().m_w;
    int maxY = screens.front().m_y + screens.front().m_h;
    for (std::vector<ClientScreenInfo>::const_iterator it = screens.begin();
         it != screens.end(); ++it) {
        minX = std::min(minX, static_cast<int>(it->m_x));
        minY = std::min(minY, static_cast<int>(it->m_y));
        maxX = std::max(maxX, static_cast<int>(it->m_x + it->m_w));
        maxY = std::max(maxY, static_cast<int>(it->m_y + it->m_h));
    }

    return HostScreenBounds(minX, minY, maxX, maxY);
}

static int
intervalOverlap(int a0, int a1, int b0, int b1)
{
    return std::max(0, std::min(a1, b1) - std::max(a0, b0));
}

static int
edgeGap(const etherwaver::layout::Screen& source,
        const etherwaver::layout::Screen& candidate,
        EDirection direction)
{
    switch (direction) {
    case kLeft:
        return source.m_x - (candidate.m_x + candidate.m_width);

    case kRight:
        return candidate.m_x - (source.m_x + source.m_width);

    case kTop:
        return source.m_y - (candidate.m_y + candidate.m_height);

    case kBottom:
        return candidate.m_y - (source.m_y + source.m_height);

    case kNoDirection:
        break;
    }

    return std::numeric_limits<int>::max();
}

static int
perpendicularOverlap(const etherwaver::layout::Screen& source,
                     const etherwaver::layout::Screen& candidate,
                     EDirection direction)
{
    switch (direction) {
    case kLeft:
    case kRight:
        return intervalOverlap(source.m_y, source.m_y + source.m_height,
                               candidate.m_y, candidate.m_y + candidate.m_height);

    case kTop:
    case kBottom:
        return intervalOverlap(source.m_x, source.m_x + source.m_width,
                               candidate.m_x, candidate.m_x + candidate.m_width);

    case kNoDirection:
        break;
    }

    return 0;
}

static int
perpendicularCenterDistance(const etherwaver::layout::Screen& source,
                            const etherwaver::layout::Screen& candidate,
                            EDirection direction)
{
    if (direction == kLeft || direction == kRight) {
        const int sourceCenter = source.m_y + source.m_height / 2;
        const int candidateCenter = candidate.m_y + candidate.m_height / 2;
        return std::abs(sourceCenter - candidateCenter);
    }

    const int sourceCenter = source.m_x + source.m_width / 2;
    const int candidateCenter = candidate.m_x + candidate.m_width / 2;
    return std::abs(sourceCenter - candidateCenter);
}

static etherwaver::layout::Screen*
findBestDirectionalNeighbor(std::vector<etherwaver::layout::Screen>& screens,
                            size_t sourceIndex,
                            EDirection direction,
                            const std::string& targetHostId,
                            bool requireTouching)
{
    etherwaver::layout::Screen* best = NULL;
    int bestGap = std::numeric_limits<int>::max();
    int bestOverlap = -1;
    int bestCenterDistance = std::numeric_limits<int>::max();

    const etherwaver::layout::Screen& source = screens[sourceIndex];
    for (size_t i = 0; i < screens.size(); ++i) {
        if (i == sourceIndex) {
            continue;
        }

        etherwaver::layout::Screen& candidate = screens[i];
        if (!targetHostId.empty() && candidate.m_hostId != targetHostId) {
            continue;
        }

        const int gap = edgeGap(source, candidate, direction);
        if (gap < 0) {
            continue;
        }
        if (requireTouching && gap != 0) {
            continue;
        }

        const int overlap = perpendicularOverlap(source, candidate, direction);
        const int centerDistance =
            perpendicularCenterDistance(source, candidate, direction);
        if (best == NULL ||
            gap < bestGap ||
            (gap == bestGap && overlap > bestOverlap) ||
            (gap == bestGap && overlap == bestOverlap &&
             centerDistance < bestCenterDistance)) {
            best = &candidate;
            bestGap = gap;
            bestOverlap = overlap;
            bestCenterDistance = centerDistance;
        }
    }

    return best;
}

static void
setDirectionalLink(etherwaver::layout::Screen& screen,
                   EDirection direction,
                   const std::string& targetId)
{
    switch (direction) {
    case kLeft:
        screen.m_leftLink = targetId;
        break;

    case kRight:
        screen.m_rightLink = targetId;
        break;

    case kTop:
        screen.m_topLink = targetId;
        break;

    case kBottom:
        screen.m_bottomLink = targetId;
        break;

    case kNoDirection:
        break;
    }
}

static etherwaver::layout::Screen*
findLayoutScreenByConfigName(std::vector<etherwaver::layout::Screen>& screens,
                             const std::string& configName)
{
    for (std::vector<etherwaver::layout::Screen>::iterator it = screens.begin();
         it != screens.end(); ++it) {
        if (it->m_name == configName || it->m_id == configName) {
            return &(*it);
        }
    }

    const std::string configBaseName = baseHostName(configName);
    for (std::vector<etherwaver::layout::Screen>::iterator it = screens.begin();
         it != screens.end(); ++it) {
        if (it->m_hostId == configBaseName &&
            (it->m_name == configName ||
             it->m_id == configBaseName + ":" + configName)) {
            return &(*it);
        }
    }

    return NULL;
}

static void
applyConfigLinksToScreens(const Config& config,
                          std::vector<etherwaver::layout::Screen>& screens)
{
    static const EDirection directions[] = { kLeft, kRight, kTop, kBottom };

    for (Config::const_iterator it = config.begin(); it != config.end(); ++it) {
        etherwaver::layout::Screen* source =
            findLayoutScreenByConfigName(screens, *it);
        if (source == NULL) {
            continue;
        }

        for (size_t i = 0; i < sizeof(directions) / sizeof(directions[0]); ++i) {
            const EDirection direction = directions[i];
            const std::string neighbor =
                config.getNeighbor(*it, direction, 0.5f, NULL);
            if (neighbor.empty()) {
                continue;
            }

            etherwaver::layout::Screen* destination =
                findLayoutScreenByConfigName(screens, neighbor);
            if (destination == NULL) {
                continue;
            }

            setDirectionalLink(*source, direction, destination->m_id);
        }
    }
}

static void
synthesizeDirectionalLinks(const Config& config,
                           std::vector<etherwaver::layout::Screen>& screens)
{
    static const EDirection directions[] = { kLeft, kRight, kTop, kBottom };

    for (size_t i = 0; i < screens.size(); ++i) {
        for (size_t d = 0; d < sizeof(directions) / sizeof(directions[0]); ++d) {
            const EDirection direction = directions[d];

            // Prefer real same-host monitor adjacency when multiple client
            // screens exist on one machine.
            etherwaver::layout::Screen* neighbor =
                findBestDirectionalNeighbor(screens, i, direction,
                                            screens[i].m_hostId, true);
            if (neighbor != NULL) {
                setDirectionalLink(screens[i], direction, neighbor->m_id);
                continue;
            }

            const std::string neighborHost =
                config.getNeighbor(screens[i].m_hostId, direction, 0.5f, NULL);
            if (neighborHost.empty()) {
                continue;
            }

            neighbor = findBestDirectionalNeighbor(
                screens, i, direction, neighborHost, false);
            if (neighbor != NULL) {
                setDirectionalLink(screens[i], direction, neighbor->m_id);
            }
        }
    }
}

static etherwaver::layout::ScreenManager
convertConfigLogicalScreensToObjectLayout(
    const Config& config,
    const std::map<std::string, etherwaver::layout::HostGeometry>& hostGeometries,
    const std::map<std::string, std::vector<ClientScreenInfo> >& hostScreens,
    const std::string& primaryHostId)
{
    std::map<std::string, HostScreenBounds> screenBounds;
    std::map<std::string, std::string> screenHosts;

    for (Config::const_iterator it = config.begin(); it != config.end(); ++it) {
        std::string hostId;
        ClientScreenInfo screenInfo;
        if (getNamedScreenForConfigEntry(hostScreens, hostGeometries, *it,
                                         hostId, screenInfo)) {
            std::vector<ClientScreenInfo> singleScreen;
            singleScreen.push_back(screenInfo);
            screenBounds[*it] = getHostScreenBounds(singleScreen);
            screenHosts[*it] = hostId;
            continue;
        }

        hostId = baseHostName(*it);
        screenHosts[*it] = hostId;
        screenBounds[*it] =
            getHostScreenBounds(getScreensForHost(hostScreens, hostGeometries, hostId));
    }

    std::map<std::string, std::pair<int, int> > positions;
    std::queue<std::string> pending;
    std::vector<etherwaver::layout::Screen> screens;

    std::string primaryScreenId = primaryHostId;
    if (!config.isScreen(primaryScreenId)) {
        const std::string basePrimaryHostId = baseHostName(primaryHostId);
        for (Config::const_iterator it = config.begin(); it != config.end(); ++it) {
            if (screenHosts[*it] == basePrimaryHostId) {
                primaryScreenId = *it;
                break;
            }
        }
    }

    if (!primaryScreenId.empty() && config.isScreen(primaryScreenId)) {
        positions[primaryScreenId] = std::make_pair(0, 0);
        pending.push(primaryScreenId);
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
            const HostScreenBounds currentBounds = screenBounds[current];
            const HostScreenBounds neighborBounds = screenBounds[neighbor];
            switch (direction) {
            case kLeft:
                nextPos.first -= neighborBounds.width();
                break;
            case kRight:
                nextPos.first += currentBounds.width();
                break;
            case kTop:
                nextPos.second -= neighborBounds.height();
                break;
            case kBottom:
                nextPos.second += currentBounds.height();
                break;
            default:
                break;
            }

            positions[neighbor] = nextPos;
            pending.push(neighbor);
        }
    }

    for (Config::const_iterator it = config.begin(); it != config.end(); ++it) {
        const std::string configName = *it;
        if (positions.find(configName) == positions.end()) {
            int detachedX = 0;
            for (std::map<std::string, std::pair<int, int> >::const_iterator pos = positions.begin();
                 pos != positions.end(); ++pos) {
                detachedX = std::max(detachedX,
                                     pos->second.first + screenBounds[pos->first].width());
            }
            positions[configName] = std::make_pair(detachedX, 0);
        }

        const std::pair<int, int> pos = positions[configName];
        const HostScreenBounds bounds = screenBounds[configName];
        const std::string hostId = screenHosts[configName];
        screens.push_back(etherwaver::layout::Screen(hostId + ":" + configName,
                                                     hostId,
                                                     configName,
                                                     pos.first,
                                                     pos.second,
                                                     bounds.width(),
                                                     bounds.height()));
    }

    for (size_t i = 0; i < screens.size(); ++i) {
        static const EDirection directions[] = { kLeft, kRight, kTop, kBottom };
        for (size_t d = 0; d < sizeof(directions) / sizeof(directions[0]); ++d) {
            const EDirection direction = directions[d];
            const std::string neighbor = config.getNeighbor(screens[i].m_name, direction, 0.5f, NULL);
            if (neighbor.empty()) {
                continue;
            }

            const std::string neighborHost = screenHosts[neighbor];
            setDirectionalLink(screens[i], direction, neighborHost + ":" + neighbor);
        }
    }

    etherwaver::layout::ScreenManager manager;
    manager.setScreens(screens);
    return manager;
}

static etherwaver::layout::ScreenManager
normalizeJsonLayout(const etherwaver::layout::ScreenManager& manager,
                    const Config& config,
                    const std::map<std::string, etherwaver::layout::HostGeometry>& hostGeometries,
                    const std::map<std::string, std::vector<ClientScreenInfo> >& hostScreens)
{
    const std::vector<etherwaver::layout::Screen>& originalScreens = manager.getScreens();
    std::vector<etherwaver::layout::Screen> normalizedScreens;
    std::set<std::string> processedHosts;

    for (std::vector<etherwaver::layout::Screen>::const_iterator it = originalScreens.begin();
         it != originalScreens.end(); ++it) {
        if (!processedHosts.insert(it->m_hostId).second) {
            continue;
        }

        const std::vector<etherwaver::layout::Screen> hostLayoutScreens =
            getLayoutScreensForHost(manager, it->m_hostId);
        const std::vector<ClientScreenInfo> actualHostScreens =
            getScreensForHost(hostScreens, hostGeometries, it->m_hostId);

        if (hostLayoutScreens.size() == 1 && actualHostScreens.size() > 1) {
            const etherwaver::layout::Screen& hostLayout = hostLayoutScreens.front();

            int minX = actualHostScreens.front().m_x;
            int minY = actualHostScreens.front().m_y;
            int maxX = actualHostScreens.front().m_x + actualHostScreens.front().m_w;
            int maxY = actualHostScreens.front().m_y + actualHostScreens.front().m_h;
            for (std::vector<ClientScreenInfo>::const_iterator screen = actualHostScreens.begin();
                 screen != actualHostScreens.end(); ++screen) {
                minX = std::min(minX, static_cast<int>(screen->m_x));
                minY = std::min(minY, static_cast<int>(screen->m_y));
                maxX = std::max(maxX, static_cast<int>(screen->m_x + screen->m_w));
                maxY = std::max(maxY, static_cast<int>(screen->m_y + screen->m_h));
            }

            const int actualWidth = std::max(1, maxX - minX);
            const int actualHeight = std::max(1, maxY - minY);
            for (std::vector<ClientScreenInfo>::const_iterator screen = actualHostScreens.begin();
                 screen != actualHostScreens.end(); ++screen) {
                const int left =
                    hostLayout.m_x +
                    ((static_cast<int>(screen->m_x) - minX) * hostLayout.m_width) / actualWidth;
                const int top =
                    hostLayout.m_y +
                    ((static_cast<int>(screen->m_y) - minY) * hostLayout.m_height) / actualHeight;
                const int right =
                    hostLayout.m_x +
                    ((static_cast<int>(screen->m_x + screen->m_w) - minX) * hostLayout.m_width) /
                        actualWidth;
                const int bottom =
                    hostLayout.m_y +
                    ((static_cast<int>(screen->m_y + screen->m_h) - minY) * hostLayout.m_height) /
                        actualHeight;

                normalizedScreens.push_back(
                    etherwaver::layout::Screen(it->m_hostId + ":" + screen->m_id,
                                               it->m_hostId,
                                               screen->m_id,
                                               left,
                                               top,
                                               std::max(1, right - left),
                                               std::max(1, bottom - top)));
            }
            continue;
        }

        if (hasRuntimeScreensForHost(hostScreens, it->m_hostId) &&
            actualHostScreens.size() < hostLayoutScreens.size()) {
            std::vector<etherwaver::layout::Screen> retainedScreens;

            for (std::vector<etherwaver::layout::Screen>::const_iterator layoutScreen =
                     hostLayoutScreens.begin();
                 layoutScreen != hostLayoutScreens.end(); ++layoutScreen) {
                for (std::vector<ClientScreenInfo>::const_iterator actualScreen =
                         actualHostScreens.begin();
                     actualScreen != actualHostScreens.end(); ++actualScreen) {
                    if (layoutScreenMatchesClientScreen(*layoutScreen, *actualScreen)) {
                        retainedScreens.push_back(*layoutScreen);
                        break;
                    }
                }
            }

            if (retainedScreens.empty() && actualHostScreens.size() == 1) {
                for (std::vector<etherwaver::layout::Screen>::const_iterator layoutScreen =
                         hostLayoutScreens.begin();
                     layoutScreen != hostLayoutScreens.end(); ++layoutScreen) {
                    if (isLinkedFromAnotherHost(originalScreens, *layoutScreen)) {
                        retainedScreens.push_back(*layoutScreen);
                    }
                }
            }

            if (retainedScreens.empty() && actualHostScreens.size() == 1) {
                for (std::vector<etherwaver::layout::Screen>::const_iterator layoutScreen =
                         hostLayoutScreens.begin();
                     layoutScreen != hostLayoutScreens.end(); ++layoutScreen) {
                    if (hasFirstScreenSuffix(*layoutScreen)) {
                        retainedScreens.push_back(*layoutScreen);
                        break;
                    }
                }
            }

            if (retainedScreens.empty() && actualHostScreens.size() == 1) {
                retainedScreens.push_back(hostLayoutScreens.front());
            }

            normalizedScreens.insert(normalizedScreens.end(),
                                     retainedScreens.begin(),
                                     retainedScreens.end());
            continue;
        }

        normalizedScreens.insert(normalizedScreens.end(),
                                 hostLayoutScreens.begin(),
                                 hostLayoutScreens.end());
    }

    applyConfigLinksToScreens(config, normalizedScreens);
    clearLinksToMissingScreens(normalizedScreens);

    etherwaver::layout::ScreenManager normalized;
    normalized.setScreens(normalizedScreens);
    return normalized;
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
    if (configUsesLogicalScreenNames(config, hostScreens, hostGeometries)) {
        return convertConfigLogicalScreensToObjectLayout(
            config, hostGeometries, hostScreens, primaryHostId);
    }

    std::ifstream stream(layoutPath.c_str());
    if (stream.good()) {
        return normalizeJsonLayout(loadJsonLayout(layoutPath), config,
                                   hostGeometries, hostScreens);
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
    if (configUsesLogicalScreenNames(config, hostScreens, hostGeometries)) {
        return convertConfigLogicalScreensToObjectLayout(
            config, hostGeometries, hostScreens, primaryHostId);
    }

    std::map<std::string, HostScreenBounds> hostBounds;
    for (Config::const_iterator it = config.begin(); it != config.end(); ++it) {
        hostBounds[*it] =
            getHostScreenBounds(getScreensForHost(hostScreens, hostGeometries, *it));
    }

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
            const HostScreenBounds currentBounds = hostBounds[current];
            const HostScreenBounds neighborBounds = hostBounds[neighbor];
            switch (direction) {
            case kLeft:
                nextPos.first -= neighborBounds.width();
                break;
            case kRight:
                nextPos.first += currentBounds.width();
                break;
            case kTop:
                nextPos.second -= neighborBounds.height();
                break;
            case kBottom:
                nextPos.second += currentBounds.height();
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
            int detachedX = 0;
            for (std::map<std::string, std::pair<int, int> >::const_iterator pos = positions.begin();
                 pos != positions.end(); ++pos) {
                detachedX = std::max(detachedX,
                                     pos->second.first + hostBounds[pos->first].width());
            }
            positions[hostId] = std::make_pair(detachedX, 0);
        }

        const std::pair<int, int> grid = positions[hostId];
        const std::vector<ClientScreenInfo> hostScreenList =
            getScreensForHost(hostScreens, hostGeometries, hostId);
        const HostScreenBounds bounds = hostBounds[hostId];
        const int hostOriginX = grid.first;
        const int hostOriginY = grid.second;

        for (std::vector<ClientScreenInfo>::const_iterator screen = hostScreenList.begin();
             screen != hostScreenList.end(); ++screen) {
            const std::string screenId = hostId + ":" + screen->m_id;
            screens.push_back(Screen(screenId, hostId, screenId,
                                     hostOriginX + (screen->m_x - bounds.m_minX),
                                     hostOriginY + (screen->m_y - bounds.m_minY),
                                     screen->m_w,
                                     screen->m_h));
        }
    }

    synthesizeDirectionalLinks(config, screens);

    ScreenManager manager;
    manager.setScreens(screens);
    return manager;
}

} // namespace layout
} // namespace etherwaver
