// SPDX-License-Identifier: GPL-3.0-or-later
#include "effects.h"

#include <KWindowEffects>

#include <QGuiApplication>
#include <QPainterPath>
#include <QRegion>
#include <QTimer>
#include <QWindow>

ShellEffects::ShellEffects(QObject *parent)
    : QObject(parent)
{
    // Outside a Wayland session (the headless smoke test) there is no
    // compositor to ask.
    if (QGuiApplication::platformName() != QLatin1String("wayland"))
        return;
    refresh();
    // The compositor announces its blur support shortly after the connection
    // is made and may withdraw it later; poll cheaply rather than miss it.
    auto *timer = new QTimer(this);
    timer->setInterval(5000);
    connect(timer, &QTimer::timeout, this, &ShellEffects::refresh);
    timer->start();
    QTimer::singleShot(500, this, &ShellEffects::refresh);
}

void ShellEffects::refresh()
{
    if (QGuiApplication::platformName() != QLatin1String("wayland"))
        return;
    const bool available = KWindowEffects::isEffectAvailable(KWindowEffects::BlurBehind);
    if (available != m_blurAvailable) {
        m_blurAvailable = available;
        Q_EMIT blurAvailableChanged();
    }
}

void ShellEffects::setBlurBehind(QWindow *window, bool enable, qreal x, qreal y, qreal w, qreal h, qreal radius)
{
    if (!window || QGuiApplication::platformName() != QLatin1String("wayland"))
        return;
    if (!enable || !m_blurAvailable || w <= 0 || h <= 0) {
        KWindowEffects::enableBlurBehind(window, false);
        return;
    }
    QRegion region;
    const QRectF rect(x, y, w, h);
    if (radius > 0.5) {
        QPainterPath path;
        path.addRoundedRect(rect, radius, radius);
        region = QRegion(path.toFillPolygon().toPolygon());
    } else {
        region = QRegion(rect.toAlignedRect());
    }
    KWindowEffects::enableBlurBehind(window, true, region);
}
