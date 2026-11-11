// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "trayicons.h"

#include <QDBusError>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantMap>

#include <functional>

class QWindow;
class TrayHost;
class TrayMenu;
Q_MOC_INCLUDE("traymenu.h")

// One StatusNotifierItem of the session: a proxy for the object another
// program exports at <service><path> with the interface
// org.kde.StatusNotifierItem.
//
// Every call to the item is asynchronous. Its properties are read with one
// Properties.GetAll when the item appears and again whenever it announces a
// change (NewIcon, NewStatus, ...); announcements are coalesced, so an item
// cannot make the shell fetch more often than TrayLimits::RefreshIntervalMs.
class TrayItem : public QObject
{
    Q_OBJECT
    // Identity
    Q_PROPERTY(int serial READ serial CONSTANT)
    Q_PROPERTY(QString itemId READ itemId NOTIFY identityChanged)
    Q_PROPERTY(QString title READ title NOTIFY identityChanged)
    Q_PROPERTY(QString category READ category NOTIFY identityChanged)
    // "Passive" | "Active" | "NeedsAttention"
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool shown READ shown NOTIFY statusChanged)                  // not Passive
    Q_PROPERTY(bool needsAttention READ needsAttention NOTIFY statusChanged)
    // image://nxtray/item/... (without colour parameters); changes whenever the picture does
    Q_PROPERTY(QString iconSource READ iconSource NOTIFY iconChanged)
    Q_PROPERTY(bool hasIcon READ hasIcon NOTIFY iconChanged)
    // What a tool tip shows: a title (ToolTip title, else Title, else Id) and optional further lines
    Q_PROPERTY(QString toolTipTitle READ toolTipTitle NOTIFY toolTipChanged)
    Q_PROPERTY(QString toolTipText READ toolTipText NOTIFY toolTipChanged)
    // The item exports a com.canonical.dbusmenu menu.
    Q_PROPERTY(bool hasMenu READ hasMenu NOTIFY menuChanged)
    Q_PROPERTY(TrayMenu *menu READ menu NOTIFY menuChanged)
    // The item has nothing but its menu: the primary click opens it too.
    Q_PROPERTY(bool itemIsMenu READ itemIsMenu NOTIFY menuChanged)

public:
    TrayItem(TrayHost *host, int serial, const QString &service, const QString &path);
    ~TrayItem() override;

    // Resolves the owner of the service and reads the properties.
    void start();

    int serial() const { return m_serial; }
    QString service() const { return m_service; }
    QString owner() const { return m_owner; }
    QString path() const { return m_path; }
    QString registration() const { return m_service + m_path; }
    // All properties have been read at least once.
    bool ready() const { return m_ready; }

    QString itemId() const { return m_id; }
    QString title() const { return m_title; }
    QString category() const { return m_category; }
    QString status() const { return m_status; }
    bool shown() const { return m_status != QLatin1String("Passive"); }
    bool needsAttention() const { return m_status == QLatin1String("NeedsAttention"); }
    QString iconSource() const;
    bool hasIcon() const { return m_hasIcon; }
    QString toolTipTitle() const;
    QString toolTipText() const { return m_toolTipText; }
    bool hasMenu() const { return !m_menuPath.isEmpty(); }
    TrayMenu *menu();
    TrayMenu *existingMenu() const { return m_menu; }
    bool itemIsMenu() const { return m_itemIsMenu; }

    // The item's picture (attention icon while it needs attention, overlay
    // icon on top) as a square of 'side' device pixels; null if it has none.
    QImage image(int side, const QColor &foreground, const QColor &background) const;

    // A signal of the item's interface arrived.
    void handleSignal(const QString &member, const QVariantList &arguments);

    // Calls the item with an activation token for 'origin' (the window the
    // user clicked in), so the application may bring its window to the front.
    // 'then' runs once the token has been handed over, or cannot be had.
    void provideActivationToken(QWindow *origin, const std::function<void()> &then);

    // Diagnostics (--dump tray).
    QJsonObject toJson() const;

    // x, y: position of the icon on the screen, for the application to place
    // what it opens. origin: the window the click happened in.
    Q_INVOKABLE void activate(int x, int y, QWindow *origin);
    Q_INVOKABLE void secondaryActivate(int x, int y, QWindow *origin);
    Q_INVOKABLE void contextMenu(int x, int y, QWindow *origin);
    // Wheel movement in the units of QWheelEvent::angleDelta (120 per notch).
    Q_INVOKABLE void scroll(int deltaX, int deltaY);

Q_SIGNALS:
    void readyChanged();
    void ownerResolved();
    void identityChanged();
    void statusChanged();
    void iconChanged();
    void toolTipChanged();
    void menuChanged();
    // Activate is not something this item does (it answered with an error):
    // the caller may offer the menu instead.
    void activateUnsupported();
    // The first read of the properties failed for good.
    void failed();

private:
    void resolveOwner();
    void refresh();
    void fetch();
    void fetchSingly();
    void fetchFinished(qint64 parseMs);
    void fetchFailed(const QDBusError &error);
    void apply(const QVariantMap &properties);
    void send(const QString &method, const QVariantList &arguments);
    void positionCall(const QString &method, int x, int y, QWindow *origin, bool reportUnsupported);
    QImage baseImage(int side) const;

    TrayHost *m_host;
    const int m_serial;
    const QString m_service;        // as registered: a well-known or a unique bus name
    const QString m_path;
    QString m_owner;                // unique name of the connection that owns m_service

    bool m_ready = false;
    bool m_fetching = false;
    bool m_dirty = false;           // a change was announced while a fetch was running
    bool m_singly = false;          // GetAll does not work for this item; use Get per property
    int m_failures = 0;
    QElapsedTimer m_clock;
    qint64 m_nextFetch = 0;         // m_clock time before which no fetch starts
    QTimer m_refreshTimer;

    QString m_id, m_title, m_category;
    QString m_status = QStringLiteral("Active");
    QString m_iconThemePath;
    TrayIconSource m_icon, m_attentionIcon, m_overlayIcon;
    int m_iconGeneration = 0;
    bool m_hasIcon = false;
    mutable QHash<int, QImage> m_baseImages;    // by side, for the current generation
    QString m_toolTipTitle, m_toolTipText;
    QString m_menuPath;
    bool m_itemIsMenu = false;
    TrayMenu *m_menu = nullptr;

    int m_scrollX = 0, m_scrollY = 0;           // wheel movement not yet passed on
};
