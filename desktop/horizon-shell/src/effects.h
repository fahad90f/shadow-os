// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>

class QWindow;

// Compositor effects for shell surfaces. The only one used is "blur behind":
// translucent ("glass") surfaces are legible only when the compositor blurs
// what is underneath, so the shell asks whether blur is available and falls
// back to opaque surfaces when it is not (software rendering, effects off).
class ShellEffects : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool blurAvailable READ blurAvailable NOTIFY blurAvailableChanged)

public:
    explicit ShellEffects(QObject *parent = nullptr);

    bool blurAvailable() const { return m_blurAvailable; }

    // Re-reads whether the compositor offers blur (it can change at run time,
    // e.g. when compositing switches between OpenGL and software).
    Q_INVOKABLE void refresh();

    // Asks the compositor to blur what is behind the rounded rectangle
    // (x, y, w, h, radius) of the window, in window coordinates; enable=false
    // removes the request.
    Q_INVOKABLE void setBlurBehind(QWindow *window, bool enable, qreal x, qreal y, qreal w, qreal h, qreal radius);

Q_SIGNALS:
    void blurAvailableChanged();

private:
    bool m_blurAvailable = false;
};
