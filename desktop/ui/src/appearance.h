// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QQmlEngine>
#include <QString>

namespace Shadow {

// Process-wide appearance state shared by every SHADOW application.
//
// The state lives in ~/.config/shadow/appearance.conf (system default:
// /etc/xdg/shadow/appearance.conf). Every process watches that file, so a
// change made in the Control Center re-themes all running SHADOW apps and the
// shell immediately, without restarts or a session bus round-trip.
class Appearance : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(bool dark READ dark WRITE setDark NOTIFY darkChanged)
    Q_PROPERTY(QString accent READ accent WRITE setAccent NOTIFY accentChanged)
    Q_PROPERTY(bool reduceMotion READ reduceMotion WRITE setReduceMotion NOTIFY reduceMotionChanged)
    Q_PROPERTY(bool transparency READ transparency WRITE setTransparency NOTIFY transparencyChanged)
    Q_PROPERTY(qreal fontScale READ fontScale WRITE setFontScale NOTIFY fontScaleChanged)
    // True when Qt Quick renders this process with its software backend
    // (QT_QUICK_BACKEND=software — set by the session on machines without
    // usable 3D — or Qt's own fallback). Every animation frame is then
    // rasterised on the CPU, so components skip decorative value animations.
    Q_PROPERTY(bool softwareRendering READ softwareRendering CONSTANT)
    // What components should test before animating a value change:
    // false with "reduce motion" and with software rendering.
    Q_PROPERTY(bool animateValues READ animateValues NOTIFY animateValuesChanged)

public:
    static Appearance *instance();
    static Appearance *create(QQmlEngine *, QJSEngine *);

    bool dark() const { return m_dark; }
    QString accent() const { return m_accent; }
    bool reduceMotion() const { return m_reduceMotion; }
    bool transparency() const { return m_transparency; }
    qreal fontScale() const { return m_fontScale; }
    static bool softwareRendering();
    bool animateValues() const { return !m_reduceMotion && !softwareRendering(); }

    void setDark(bool dark);
    void setAccent(const QString &accent);
    void setReduceMotion(bool reduce);
    void setTransparency(bool on);
    void setFontScale(qreal scale);

    static QString configPath();

Q_SIGNALS:
    void darkChanged();
    void accentChanged();
    void reduceMotionChanged();
    void transparencyChanged();
    void fontScaleChanged();
    void animateValuesChanged();

private:
    explicit Appearance(QObject *parent = nullptr);
    void load();
    void store(const QString &key, const QVariant &value);
    void watch();

    QFileSystemWatcher m_watcher;
    bool m_dark = true;
    QString m_accent = QStringLiteral("cyan");
    bool m_reduceMotion = false;
    bool m_transparency = true;
    qreal m_fontScale = 1.0;
};

} // namespace Shadow
