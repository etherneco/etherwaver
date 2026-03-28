#include "LayoutEditorWidget.h"

#include "ScreenSettingsDialog.h"
#include "ServerConfig.h"

#include <QEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPolygon>
#include <QStyleOption>
#include <QSvgRenderer>

#include <set>

namespace {

const int kCardWidth = 92;
const int kCardHeight = 86;
const int kCardGap = 28;
const int kCanvasPadding = 32;
const qreal kZoomStep = 0.1;
const qreal kZoomMin = 0.5;
const qreal kZoomMax = 2.0;
const int kIconSize = 48;
const int kLabelTopGap = 8;
const int kContentPaddingX = 12;
const int kContentPaddingTop = 6;
const int kContentPaddingBottom = 10;
const int kHandleInset = 8;
const int kHandleSize = 10;

QColor backgroundColor()
{
    return QColor(245, 243, 238);
}

QColor gridColor()
{
    return QColor(220, 214, 204);
}

QColor textColor()
{
    return QColor(78, 72, 62);
}

QColor borderColor(bool selected)
{
    return selected ? QColor(57, 114, 197) : QColor(0, 0, 0, 0);
}

QColor linkColor()
{
    return QColor(120, 126, 134);
}

QColor handleFillColor(bool active)
{
    return active ? QColor(57, 114, 197) : QColor(110, 118, 130, 210);
}

QColor handleStrokeColor(bool active)
{
    return active ? QColor(27, 79, 156) : QColor(81, 88, 97);
}

} // namespace

LayoutEditorWidget::LayoutEditorWidget(ServerConfig& config, QWidget* parent) :
    QWidget(parent),
    m_config(config),
    m_screens(config.m_Screens),
    m_selectedIndex(-1),
    m_dragging(false),
    m_linkDragging(false),
    m_dragOffset(0, 0),
    m_hoveredIndex(-1),
    m_hoveredHandle(),
    m_linkSourceHandle(),
    m_linkTargetHandle(),
    m_originalLinkTargetHandle(),
    m_lastMousePos(0, 0),
    m_zoom(1.0)
{
    setMouseTracking(true);
    setAutoFillBackground(true);
    setFocusPolicy(Qt::StrongFocus);
}

QSize
LayoutEditorWidget::minimumSizeHint() const
{
    return QSize(720, 360);
}

void
LayoutEditorWidget::addScreen(const QString& defaultName)
{
    const int slot = findFreeSlot();
    if (slot == -1) {
        return;
    }

    Screen screen(defaultName);
    ensureGeometry(screen, slot);

    QWidget* dialogParent = window();
    ScreenSettingsDialog dialog(dialogParent, &screen);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    m_screens[slot] = screen;
    setSelectedIndex(slot);
    updateHoverState(m_lastMousePos);
    update();
}

void
LayoutEditorWidget::editSelectedScreen()
{
    if (m_selectedIndex < 0 || m_selectedIndex >= static_cast<int>(m_screens.size())) {
        return;
    }

    Screen& screen = m_screens[m_selectedIndex];
    if (screen.isNull()) {
        return;
    }

    const QString oldName = screen.name();
    QWidget* dialogParent = window();
    ScreenSettingsDialog dialog(dialogParent, &screen);
    if (dialog.exec() == QDialog::Accepted) {
        renameLinks(oldName, screen.name());
        updateHoverState(m_lastMousePos);
        update();
    }
}

void
LayoutEditorWidget::removeSelectedScreen()
{
    if (m_selectedIndex < 0 || m_selectedIndex >= static_cast<int>(m_screens.size())) {
        return;
    }

    const QString removedName = m_screens[m_selectedIndex].name();
    removeLinksToScreen(removedName);
    m_screens[m_selectedIndex] = Screen();
    setSelectedIndex(-1);
    updateHoverState(m_lastMousePos);
    update();
}

void
LayoutEditorWidget::autoLayout()
{
    int ordinal = 0;
    for (std::vector<Screen>::iterator it = m_screens.begin(); it != m_screens.end(); ++it) {
        if (it->isNull()) {
            continue;
        }

        ensureGeometry(*it, ordinal++);
    }

    updateHoverState(m_lastMousePos);
    update();
}

void
LayoutEditorWidget::zoomIn()
{
    m_zoom = qMin(kZoomMax, m_zoom + kZoomStep);
    updateHoverState(m_lastMousePos);
    update();
}

void
LayoutEditorWidget::zoomOut()
{
    m_zoom = qMax(kZoomMin, m_zoom - kZoomStep);
    updateHoverState(m_lastMousePos);
    update();
}

void
LayoutEditorWidget::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), backgroundColor());

    painter.setPen(QPen(gridColor(), 1));
    const int gridStep = qMax(12, qRound(24 * m_zoom));
    for (int x = 0; x < width(); x += gridStep) {
        painter.drawLine(x, 0, x, height());
    }
    for (int y = 0; y < height(); y += gridStep) {
        painter.drawLine(0, y, width(), y);
    }

    const std::vector<ScreenRef> screens = visibleScreens();
    const std::vector<LinkRef> links = visibleLinks();

    painter.setPen(QPen(linkColor(), qMax(2, qRound(2 * m_zoom)), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    for (std::vector<LinkRef>::const_iterator it = links.begin(); it != links.end(); ++it) {
        painter.drawLine(it->line);
    }

    if (m_linkDragging && m_linkSourceHandle.isValid()) {
        const QRect sourceRect = contentRect(m_screens[m_linkSourceHandle.index]);
        QPen dragPen(linkColor(), qMax(2, qRound(2 * m_zoom)), Qt::DashLine, Qt::RoundCap, Qt::RoundJoin);
        painter.setPen(dragPen);
        painter.drawLine(handleCenter(sourceRect, m_linkSourceHandle.direction), m_lastMousePos);
    }

    static QSvgRenderer renderer(QString(":/res/icons/48x48/monitor-heroicons.svg"));
    for (std::vector<ScreenRef>::const_iterator it = screens.begin(); it != screens.end(); ++it) {
        const bool selected = (it->index == m_selectedIndex);
        const bool showHandles = m_linkDragging || it->index == m_hoveredIndex || it->index == m_selectedIndex;
        const Screen& screen = m_screens[it->index];

        QPainterPath path;
        path.addRoundedRect(it->rect, 12.0, 12.0);
        painter.setPen(QPen(borderColor(selected), selected ? 3 : 2));
        painter.drawPath(path);

        const int iconLeft = it->rect.x() + (it->rect.width() - qRound(kIconSize * m_zoom)) / 2;
        const int iconTop = it->rect.y() + qRound(6 * m_zoom);
        const QRect iconRect(iconLeft, iconTop, qRound(kIconSize * m_zoom), qRound(kIconSize * m_zoom));
        renderer.render(&painter, iconRect);

        painter.setPen(textColor());
        const QRect textRect(it->rect.x() + qRound(4 * m_zoom),
                             it->rect.y() + qRound(58 * m_zoom),
                             it->rect.width() - qRound(8 * m_zoom),
                             it->rect.height() - qRound(60 * m_zoom));
        painter.drawText(textRect, Qt::AlignHCenter | Qt::AlignTop, screen.name());

        if (showHandles) {
            const Screen::LinkDirection directions[] = {
                Screen::LinkRight, Screen::LinkLeft, Screen::LinkUp, Screen::LinkDown
            };
            for (unsigned int i = 0; i < sizeof(directions) / sizeof(directions[0]); ++i) {
                const Screen::LinkDirection direction = directions[i];
                const bool active =
                    (m_hoveredHandle.isValid() &&
                     m_hoveredHandle.index == it->index &&
                     m_hoveredHandle.direction == direction) ||
                    (m_linkSourceHandle.isValid() &&
                     m_linkSourceHandle.index == it->index &&
                     m_linkSourceHandle.direction == direction) ||
                    (m_linkTargetHandle.isValid() &&
                     m_linkTargetHandle.index == it->index &&
                     m_linkTargetHandle.direction == direction);
                painter.setBrush(handleFillColor(active));
                painter.setPen(QPen(handleStrokeColor(active), qMax(1, qRound(m_zoom))));
                painter.drawPolygon(handlePolygon(it->rect, direction));
            }
        }
    }
}

void
LayoutEditorWidget::mousePressEvent(QMouseEvent* event)
{
    m_lastMousePos = event->pos();

    if (event->button() == Qt::RightButton) {
        const HandleRef handle = handleAt(event->pos());
        if (handle.isValid()) {
            disconnectHandle(handle);
            updateHoverState(event->pos());
            update();
            return;
        }
    }

    if (event->button() != Qt::LeftButton) {
        return QWidget::mousePressEvent(event);
    }

    const HandleRef handle = handleAt(event->pos());
    if (handle.isValid()) {
        if (!isHandleFree(handle)) {
            m_originalLinkTargetHandle = linkedTargetFor(handle);
            disconnectHandle(handle);
            setSelectedIndex(handle.index);
            m_dragging = false;
            m_linkDragging = true;
            m_linkSourceHandle = handle;
            m_linkTargetHandle = HandleRef();
            updateHoverState(event->pos());
            update();
            return;
        }

        setSelectedIndex(handle.index);
        m_dragging = false;
        m_linkDragging = true;
        m_linkSourceHandle = handle;
        m_linkTargetHandle = HandleRef();
        m_originalLinkTargetHandle = HandleRef();
        updateHoverState(event->pos());
        update();
        return;
    }

    const int index = hitTest(event->pos());
    setSelectedIndex(index);
    if (index != -1) {
        m_dragging = true;
        m_dragOffset = logicalPoint(event->pos()) - m_screens[index].position();
    }
    updateHoverState(event->pos());
    update();
}

void
LayoutEditorWidget::mouseMoveEvent(QMouseEvent* event)
{
    m_lastMousePos = event->pos();

    if (m_linkDragging) {
        updateHoverState(event->pos());
        update();
        return;
    }

    if (m_dragging && m_selectedIndex != -1) {
        Screen& screen = m_screens[m_selectedIndex];
        QPoint position = logicalPoint(event->pos()) - m_dragOffset;
        position.setX(qMax(kCanvasPadding, position.x()));
        position.setY(qMax(kCanvasPadding, position.y()));
        screen.setPosition(position);
        updateHoverState(event->pos());
        update();
        return;
    }

    updateHoverState(event->pos());
    QWidget::mouseMoveEvent(event);
}

void
LayoutEditorWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && m_linkDragging) {
        HandleRef releaseHandle = bestLinkTargetAt(event->pos(), m_linkSourceHandle);

        if (m_linkSourceHandle.isValid() && isValidLinkTarget(m_linkSourceHandle, releaseHandle)) {
            connectHandles(m_linkSourceHandle, releaseHandle);
        }
        else if (m_linkSourceHandle.isValid() && m_originalLinkTargetHandle.isValid() &&
                 isValidLinkTarget(m_linkSourceHandle, m_originalLinkTargetHandle)) {
            connectHandles(m_linkSourceHandle, m_originalLinkTargetHandle);
        }
        m_linkDragging = false;
        m_linkSourceHandle = HandleRef();
        m_linkTargetHandle = HandleRef();
        m_originalLinkTargetHandle = HandleRef();
        updateHoverState(event->pos());
        update();
        return;
    }

    if (event->button() == Qt::LeftButton) {
        m_dragging = false;
    }
    QWidget::mouseReleaseEvent(event);
}

void
LayoutEditorWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && handleAt(event->pos()).isValid()) {
        return;
    }

    if (event->button() == Qt::LeftButton) {
        const LinkRef link = linkAt(event->pos());
        if (link.source.isValid()) {
            disconnectHandle(link.source);
            updateHoverState(event->pos());
            update();
            return;
        }
    }

    if (event->button() == Qt::LeftButton && hitTest(event->pos()) != -1) {
        editSelectedScreen();
        return;
    }

    QWidget::mouseDoubleClickEvent(event);
}

void
LayoutEditorWidget::leaveEvent(QEvent* event)
{
    if (!m_dragging && !m_linkDragging) {
        m_hoveredIndex = -1;
        m_hoveredHandle = HandleRef();
        m_linkTargetHandle = HandleRef();
        update();
    }
    QWidget::leaveEvent(event);
}

std::vector<LayoutEditorWidget::ScreenRef>
LayoutEditorWidget::visibleScreens() const
{
    std::vector<ScreenRef> result;
    int ordinal = 0;
    for (std::size_t i = 0; i < m_screens.size(); ++i) {
        const Screen& screen = m_screens[i];
        if (screen.isNull()) {
            continue;
        }

        ensureGeometry(const_cast<Screen&>(screen), ordinal++);
        result.push_back(ScreenRef{ static_cast<int>(i), contentRect(screen) });
    }
    return result;
}

std::vector<LayoutEditorWidget::LinkRef>
LayoutEditorWidget::visibleLinks() const
{
    std::vector<LinkRef> result;
    const std::vector<ScreenRef> screens = visibleScreens();
    std::set<std::pair<int, int> > seenPairs;

    for (std::vector<ScreenRef>::const_iterator it = screens.begin(); it != screens.end(); ++it) {
        const Screen& screen = m_screens[it->index];
        const Screen::LinkDirection directions[] = {
            Screen::LinkRight, Screen::LinkLeft, Screen::LinkUp, Screen::LinkDown
        };
        for (unsigned int i = 0; i < sizeof(directions) / sizeof(directions[0]); ++i) {
            const Screen::LinkDirection direction = directions[i];
            const QString targetName = screen.link(direction);
            if (targetName.isEmpty()) {
                continue;
            }

            int targetIndex = -1;
            QRect targetRect;
            for (std::vector<ScreenRef>::const_iterator jt = screens.begin(); jt != screens.end(); ++jt) {
                if (!m_screens[jt->index].isNull() &&
                    m_screens[jt->index].name().compare(targetName, Qt::CaseInsensitive) == 0) {
                    targetIndex = jt->index;
                    targetRect = jt->rect;
                    break;
                }
            }

            if (targetIndex == -1 || targetIndex == it->index) {
                continue;
            }

            const std::pair<int, int> pairKey =
                std::make_pair(qMin(it->index, targetIndex), qMax(it->index, targetIndex));
            if (seenPairs.find(pairKey) != seenPairs.end()) {
                continue;
            }

            Screen::LinkDirection targetDirection = Screen::LinkCount;
            const Screen& targetScreen = m_screens[targetIndex];
            for (int j = 0; j < static_cast<int>(Screen::LinkCount); ++j) {
                const Screen::LinkDirection candidateDirection = static_cast<Screen::LinkDirection>(j);
                if (targetScreen.link(candidateDirection).compare(screen.name(), Qt::CaseInsensitive) == 0) {
                    targetDirection = candidateDirection;
                    break;
                }
            }
            if (targetDirection == Screen::LinkCount) {
                continue;
            }

            LinkRef link;
            link.source.index = it->index;
            link.source.direction = direction;
            link.target.index = targetIndex;
            link.target.direction = targetDirection;
            link.line = QLine(handleCenter(it->rect, direction),
                              handleCenter(targetRect, targetDirection));
            result.push_back(link);
            seenPairs.insert(pairKey);
        }
    }

    return result;
}

int
LayoutEditorWidget::findFreeSlot() const
{
    for (std::size_t i = 0; i < m_screens.size(); ++i) {
        if (m_screens[i].isNull()) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int
LayoutEditorWidget::hitTest(const QPoint& position) const
{
    const std::vector<ScreenRef> screens = visibleScreens();
    for (std::vector<ScreenRef>::const_reverse_iterator it = screens.rbegin();
         it != screens.rend(); ++it) {
        if (it->rect.contains(position)) {
            return it->index;
        }
    }
    return -1;
}

LayoutEditorWidget::LinkRef
LayoutEditorWidget::linkAt(const QPoint& position) const
{
    const std::vector<LinkRef> links = visibleLinks();
    const qreal maxDistance = qMax<qreal>(6.0, 6.0 * m_zoom);

    for (std::vector<LinkRef>::const_reverse_iterator it = links.rbegin();
         it != links.rend(); ++it) {
        const QLineF line(it->line);
        if (line.length() <= 0.0) {
            continue;
        }

        const QPointF start = line.p1();
        const QPointF end = line.p2();
        const QPointF point(position);
        const QPointF delta = end - start;
        const qreal lengthSquared = delta.x() * delta.x() + delta.y() * delta.y();
        if (lengthSquared <= 0.0) {
            continue;
        }

        qreal projection = QPointF::dotProduct(point - start, delta) / lengthSquared;
        projection = qBound<qreal>(0.0, projection, 1.0);
        const QPointF nearest = start + projection * delta;
        if (QLineF(point, nearest).length() <= maxDistance) {
            return *it;
        }
    }

    return LinkRef();
}

LayoutEditorWidget::HandleRef
LayoutEditorWidget::handleAt(const QPoint& position) const
{
    const std::vector<ScreenRef> screens = visibleScreens();
    for (std::vector<ScreenRef>::const_reverse_iterator it = screens.rbegin();
         it != screens.rend(); ++it) {
        const bool handlesVisible = m_linkDragging || it->index == m_hoveredIndex || it->index == m_selectedIndex;
        if (!handlesVisible && !it->rect.contains(position)) {
            continue;
        }

        const Screen::LinkDirection directions[] = {
            Screen::LinkRight, Screen::LinkLeft, Screen::LinkUp, Screen::LinkDown
        };
        for (unsigned int i = 0; i < sizeof(directions) / sizeof(directions[0]); ++i) {
            const QPolygon polygon = handlePolygon(it->rect, directions[i]);
            if (polygon.containsPoint(position, Qt::OddEvenFill)) {
                HandleRef handle;
                handle.index = it->index;
                handle.direction = directions[i];
                handle.polygon = polygon;
                return handle;
            }
        }
    }
    return HandleRef();
}

LayoutEditorWidget::HandleRef
LayoutEditorWidget::bestLinkTargetAt(const QPoint& position, const HandleRef& source) const
{
    if (!source.isValid()) {
        return HandleRef();
    }

    const HandleRef directHandle = handleAt(position);
    if (isValidLinkTarget(source, directHandle)) {
        return directHandle;
    }

    const std::vector<ScreenRef> screens = visibleScreens();
    const qreal snapDistance = qMax<qreal>(28.0, 28.0 * m_zoom);
    qreal bestDistance = snapDistance;
    HandleRef bestHandle;

    for (std::vector<ScreenRef>::const_iterator it = screens.begin(); it != screens.end(); ++it) {
        if (it->index == source.index) {
            continue;
        }

        const QPoint center = it->rect.center();
        const int margin = qMax(18, qRound(18 * m_zoom));
        QRect expanded = it->rect.adjusted(-margin, -margin, margin, margin);
        if (!expanded.contains(position)) {
            const qreal centerDistance = QLineF(position, center).length();
            if (centerDistance > qMax(bestDistance * 2.0, snapDistance * 2.0)) {
                continue;
            }
        }

        const Screen::LinkDirection candidateDirections[] = {
            oppositeDirection(source.direction),
            Screen::LinkRight,
            Screen::LinkLeft,
            Screen::LinkUp,
            Screen::LinkDown
        };

        for (unsigned int i = 0; i < sizeof(candidateDirections) / sizeof(candidateDirections[0]); ++i) {
            HandleRef candidate;
            candidate.index = it->index;
            candidate.direction = candidateDirections[i];
            candidate.polygon = handlePolygon(it->rect, candidate.direction);

            if (!isValidLinkTarget(source, candidate)) {
                continue;
            }

            const QPoint targetPoint = handleCenter(it->rect, candidate.direction);
            const qreal distance = QLineF(position, targetPoint).length();
            if (distance <= bestDistance) {
                bestDistance = distance;
                bestHandle = candidate;
            }
        }
    }

    if (bestHandle.isValid()) {
        return bestHandle;
    }

    const int releaseIndex = hitTest(position);
    if (releaseIndex != -1 && releaseIndex != source.index) {
        const Screen::LinkDirection fallbackDirections[] = {
            oppositeDirection(source.direction),
            Screen::LinkRight,
            Screen::LinkLeft,
            Screen::LinkUp,
            Screen::LinkDown
        };
        for (unsigned int i = 0; i < sizeof(fallbackDirections) / sizeof(fallbackDirections[0]); ++i) {
            HandleRef fallbackHandle;
            fallbackHandle.index = releaseIndex;
            fallbackHandle.direction = fallbackDirections[i];
            if (isValidLinkTarget(source, fallbackHandle)) {
                return fallbackHandle;
            }
        }
    }

    return HandleRef();
}

LayoutEditorWidget::HandleRef
LayoutEditorWidget::linkedTargetFor(const HandleRef& source) const
{
    if (!source.isValid()) {
        return HandleRef();
    }

    const Screen& sourceScreen = m_screens[source.index];
    const QString targetName = sourceScreen.link(source.direction);
    if (targetName.isEmpty()) {
        return HandleRef();
    }

    for (std::size_t i = 0; i < m_screens.size(); ++i) {
        const Screen& targetScreen = m_screens[i];
        if (targetScreen.isNull() ||
            targetScreen.name().compare(targetName, Qt::CaseInsensitive) != 0) {
            continue;
        }

        for (int j = 0; j < static_cast<int>(Screen::LinkCount); ++j) {
            const Screen::LinkDirection direction = static_cast<Screen::LinkDirection>(j);
            if (targetScreen.link(direction).compare(sourceScreen.name(), Qt::CaseInsensitive) == 0) {
                HandleRef targetHandle;
                targetHandle.index = static_cast<int>(i);
                targetHandle.direction = direction;
                return targetHandle;
            }
        }
        break;
    }

    return HandleRef();
}

QRect
LayoutEditorWidget::screenRect(const Screen& screen) const
{
    return QRect(qRound(screen.position().x() * m_zoom),
                 qRound(screen.position().y() * m_zoom),
                 qRound(screen.size().width() * m_zoom),
                 qRound(screen.size().height() * m_zoom));
}

QRect
LayoutEditorWidget::contentRect(const Screen& screen) const
{
    const QRect baseRect = screenRect(screen);
    const int iconSize = qRound(kIconSize * m_zoom);
    const int labelTopGap = qRound(kLabelTopGap * m_zoom);
    const int paddingX = qRound(kContentPaddingX * m_zoom);
    const int paddingTop = qRound(kContentPaddingTop * m_zoom);
    const int paddingBottom = qRound(kContentPaddingBottom * m_zoom);

    QFontMetrics metrics(font());
    const QRect textBounds = metrics.boundingRect(screen.name());
    const int contentWidth = qMax(iconSize, textBounds.width()) + 2 * paddingX;
    const int contentHeight = paddingTop + iconSize + labelTopGap + textBounds.height() + paddingBottom;

    QRect content(baseRect.x() + (baseRect.width() - contentWidth) / 2,
                  baseRect.y() + (baseRect.height() - contentHeight) / 2,
                  contentWidth,
                  contentHeight);

    if (content.width() > baseRect.width()) {
        content.setWidth(baseRect.width());
        content.moveLeft(baseRect.x());
    }
    if (content.height() > baseRect.height()) {
        content.setHeight(baseRect.height());
        content.moveTop(baseRect.y());
    }

    return content;
}

QPolygon
LayoutEditorWidget::handlePolygon(const QRect& rect, Screen::LinkDirection direction) const
{
    const int inset = qRound(kHandleInset * m_zoom);
    const int size = qMax(8, qRound(kHandleSize * m_zoom));
    const QPoint center = rect.center();

    switch (direction) {
    case Screen::LinkRight:
        return QPolygon() << QPoint(rect.right() + inset + size, center.y())
                          << QPoint(rect.right() + inset, center.y() - size)
                          << QPoint(rect.right() + inset, center.y() + size);
    case Screen::LinkLeft:
        return QPolygon() << QPoint(rect.left() - inset - size, center.y())
                          << QPoint(rect.left() - inset, center.y() - size)
                          << QPoint(rect.left() - inset, center.y() + size);
    case Screen::LinkUp:
        return QPolygon() << QPoint(center.x(), rect.top() - inset - size)
                          << QPoint(center.x() - size, rect.top() - inset)
                          << QPoint(center.x() + size, rect.top() - inset);
    case Screen::LinkDown:
        return QPolygon() << QPoint(center.x(), rect.bottom() + inset + size)
                          << QPoint(center.x() - size, rect.bottom() + inset)
                          << QPoint(center.x() + size, rect.bottom() + inset);
    case Screen::LinkCount:
        break;
    }

    return QPolygon();
}

QPoint
LayoutEditorWidget::handleCenter(const QRect& rect, Screen::LinkDirection direction) const
{
    return handlePolygon(rect, direction).boundingRect().center();
}

Screen::LinkDirection
LayoutEditorWidget::oppositeDirection(Screen::LinkDirection direction) const
{
    switch (direction) {
    case Screen::LinkRight:
        return Screen::LinkLeft;
    case Screen::LinkLeft:
        return Screen::LinkRight;
    case Screen::LinkUp:
        return Screen::LinkDown;
    case Screen::LinkDown:
        return Screen::LinkUp;
    case Screen::LinkCount:
        break;
    }
    return Screen::LinkLeft;
}

bool
LayoutEditorWidget::isHandleFree(const HandleRef& handle) const
{
    return handle.isValid() && m_screens[handle.index].link(handle.direction).isEmpty();
}

bool
LayoutEditorWidget::isValidLinkTarget(const HandleRef& source, const HandleRef& target) const
{
    return source.isValid() &&
           target.isValid() &&
           source.index != target.index &&
           isHandleFree(target);
}

void
LayoutEditorWidget::updateHoverState(const QPoint& position)
{
    const int hoveredIndex = hitTest(position);
    const HandleRef hoveredHandle = handleAt(position);

    m_hoveredIndex = hoveredIndex;
    m_hoveredHandle = hoveredHandle;
    if (m_linkDragging) {
        m_linkTargetHandle = bestLinkTargetAt(position, m_linkSourceHandle);
    }
    else {
        m_linkTargetHandle = HandleRef();
    }
}

void
LayoutEditorWidget::connectHandles(const HandleRef& source, const HandleRef& target)
{
    if (!isValidLinkTarget(source, target)) {
        return;
    }

    disconnectHandle(source);
    disconnectHandle(target);

    Screen& sourceScreen = m_screens[source.index];
    Screen& targetScreen = m_screens[target.index];
    sourceScreen.setLink(source.direction, targetScreen.name());
    targetScreen.setLink(target.direction, sourceScreen.name());
    m_config.setUseCustomLinks(true);
}

void
LayoutEditorWidget::disconnectHandle(const HandleRef& handle)
{
    if (!handle.isValid()) {
        return;
    }

    Screen& sourceScreen = m_screens[handle.index];
    const QString targetName = sourceScreen.link(handle.direction);
    if (targetName.isEmpty()) {
        return;
    }

    sourceScreen.setLink(handle.direction, QString());
    for (std::size_t i = 0; i < m_screens.size(); ++i) {
        Screen& targetScreen = m_screens[i];
        if (targetScreen.isNull() ||
            targetScreen.name().compare(targetName, Qt::CaseInsensitive) != 0) {
            continue;
        }

        if (targetScreen.link(oppositeDirection(handle.direction)).compare(sourceScreen.name(), Qt::CaseInsensitive) == 0) {
            targetScreen.setLink(oppositeDirection(handle.direction), QString());
        }
        break;
    }

    m_config.setUseCustomLinks(true);
}

void
LayoutEditorWidget::renameLinks(const QString& oldName, const QString& newName)
{
    if (oldName.isEmpty() || oldName.compare(newName, Qt::CaseInsensitive) == 0) {
        return;
    }

    for (std::vector<Screen>::iterator it = m_screens.begin(); it != m_screens.end(); ++it) {
        if (it->isNull()) {
            continue;
        }

        for (int i = 0; i < static_cast<int>(Screen::LinkCount); ++i) {
            const Screen::LinkDirection direction = static_cast<Screen::LinkDirection>(i);
            if (it->link(direction).compare(oldName, Qt::CaseInsensitive) == 0) {
                it->setLink(direction, newName);
            }
        }
    }
}

void
LayoutEditorWidget::removeLinksToScreen(const QString& name)
{
    if (name.isEmpty()) {
        return;
    }

    for (std::vector<Screen>::iterator it = m_screens.begin(); it != m_screens.end(); ++it) {
        if (it->isNull()) {
            continue;
        }

        for (int i = 0; i < static_cast<int>(Screen::LinkCount); ++i) {
            const Screen::LinkDirection direction = static_cast<Screen::LinkDirection>(i);
            if (it->link(direction).compare(name, Qt::CaseInsensitive) == 0) {
                it->setLink(direction, QString());
            }
        }

        if (it->name().compare(name, Qt::CaseInsensitive) == 0) {
            it->clearLinks();
        }
    }
}

QPoint
LayoutEditorWidget::logicalPoint(const QPoint& point) const
{
    return QPoint(qRound(point.x() / m_zoom), qRound(point.y() / m_zoom));
}

void
LayoutEditorWidget::ensureGeometry(Screen& screen, int ordinal) const
{
    if (screen.size().width() <= 0 || screen.size().height() <= 0) {
        screen.setSize(QSize(kCardWidth, kCardHeight));
    }

    if (screen.position() != QPoint(0, 0) || ordinal == 0) {
        if (screen.position() == QPoint(0, 0) && ordinal == 0) {
            screen.setPosition(QPoint(kCanvasPadding, kCanvasPadding));
        }
        return;
    }

    const int logicalWidth = qRound(width() / m_zoom);
    const int columns = qMax(1, (logicalWidth - 2 * kCanvasPadding) / (kCardWidth + kCardGap));
    const int x = kCanvasPadding + (ordinal % columns) * (kCardWidth + kCardGap);
    const int y = kCanvasPadding + (ordinal / columns) * (kCardHeight + kCardGap);
    screen.setPosition(QPoint(x, y));
}

void
LayoutEditorWidget::setSelectedIndex(int index)
{
    if (m_selectedIndex == index) {
        return;
    }

    m_selectedIndex = index;
    emit selectionChanged(index != -1);
}
