// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QQuickWindow>

// Tells whether a window is actually on screen.
//
// Window.visible stays true for a minimised window, and a Wayland client is
// not told about minimisation at all — but in both cases the window stops
// being exposed, which is what this type reports. The monitor samples only
// while its window is exposed.
class ExposureWatcher : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickWindow *window READ window WRITE setWindow NOTIFY windowChanged)
    Q_PROPERTY(bool exposed READ exposed NOTIFY exposedChanged)

public:
    explicit ExposureWatcher(QObject *parent = nullptr);

    QQuickWindow *window() const { return m_window; }
    void setWindow(QQuickWindow *window);
    bool exposed() const { return m_exposed; }

    bool eventFilter(QObject *watched, QEvent *event) override;

Q_SIGNALS:
    void windowChanged();
    void exposedChanged();

private:
    void update();

    QPointer<QQuickWindow> m_window;
    bool m_exposed = false;
};
