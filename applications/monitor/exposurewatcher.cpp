// SPDX-License-Identifier: GPL-3.0-or-later
#include "exposurewatcher.h"

#include <QEvent>

ExposureWatcher::ExposureWatcher(QObject *parent)
    : QObject(parent)
{
}

void ExposureWatcher::setWindow(QQuickWindow *window)
{
    if (m_window == window)
        return;
    if (m_window)
        m_window->removeEventFilter(this);
    m_window = window;
    if (m_window)
        m_window->installEventFilter(this);
    Q_EMIT windowChanged();
    update();
}

bool ExposureWatcher::eventFilter(QObject *watched, QEvent *event)
{
    switch (event->type()) {
    case QEvent::Expose:
    case QEvent::Show:
    case QEvent::Hide:
    case QEvent::WindowStateChange:
        // Decide after the window has processed the event itself.
        QMetaObject::invokeMethod(this, &ExposureWatcher::update, Qt::QueuedConnection);
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

void ExposureWatcher::update()
{
    const bool exposed = m_window && m_window->isVisible() && m_window->isExposed()
        && m_window->visibility() != QWindow::Minimized;
    if (exposed == m_exposed)
        return;
    m_exposed = exposed;
    Q_EMIT exposedChanged();
}
