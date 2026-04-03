/*
 * EtherWaver free-form layout editor
 */

#pragma once

#include <QWidget>

#include "Screen.h"

#include <vector>

class QMouseEvent;
class QPaintEvent;
class QEvent;
class ServerConfig;

class LayoutEditorWidget : public QWidget
{
    Q_OBJECT

public:
    explicit LayoutEditorWidget(ServerConfig& config, QWidget* parent = NULL);

    QSize minimumSizeHint() const override;

public slots:
    void addScreen(const QString& defaultName = QString());
    void editSelectedScreen();
    void removeSelectedScreen();
    void autoLayout();
    void zoomIn();
    void zoomOut();

signals:
    void selectionChanged(bool hasSelection);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    struct ScreenRef {
        int index;
        QRect rect;
    };

    struct HandleRef {
        int index;
        Screen::LinkDirection direction;
        QPolygon polygon;

        HandleRef() : index(-1), direction(Screen::LinkRight), polygon() {}
        bool isValid() const { return index != -1; }
    };

    struct LinkRef {
        HandleRef source;
        HandleRef target;
        QLine line;
    };

    std::vector<ScreenRef> visibleScreens() const;
    std::vector<LinkRef> visibleLinks() const;
    int findFreeSlot() const;
    int hitTest(const QPoint& position) const;
    LinkRef linkAt(const QPoint& position) const;
    HandleRef handleAt(const QPoint& position) const;
    HandleRef bestLinkTargetAt(const QPoint& position, const HandleRef& source) const;
    HandleRef linkedTargetFor(const HandleRef& source) const;
    QRect screenRect(const Screen& screen) const;
    QRect contentRect(const Screen& screen) const;
    QPolygon handlePolygon(const QRect& rect, Screen::LinkDirection direction) const;
    QPoint handleCenter(const QRect& rect, Screen::LinkDirection direction) const;
    Screen::LinkDirection oppositeDirection(Screen::LinkDirection direction) const;
    bool isHandleFree(const HandleRef& handle) const;
    bool isValidLinkTarget(const HandleRef& source, const HandleRef& target) const;
    void updateHoverState(const QPoint& position);
    void connectHandles(const HandleRef& source, const HandleRef& target);
    void disconnectHandle(const HandleRef& handle);
    void renameLinks(const QString& oldName, const QString& newName);
    void removeLinksToScreen(const QString& name);
    QPoint logicalPoint(const QPoint& point) const;
    void ensureGeometry(Screen& screen, int ordinal) const;
    void setSelectedIndex(int index);

private:
    ServerConfig& m_config;
    std::vector<Screen>& m_screens;
    int m_selectedIndex;
    bool m_dragging;
    bool m_linkDragging;
    QPoint m_dragOffset;
    int m_hoveredIndex;
    HandleRef m_hoveredHandle;
    HandleRef m_linkSourceHandle;
    HandleRef m_linkTargetHandle;
    HandleRef m_originalLinkTargetHandle;
    QPoint m_lastMousePos;
    qreal m_zoom;
};
