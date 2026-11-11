// SPDX-License-Identifier: GPL-3.0-or-later
#include "appearance.h"

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QSettings>
#include <QStandardPaths>

namespace Nexus {

namespace {
const QStringList kAccents = {
    QStringLiteral("cyan"), QStringLiteral("blue"), QStringLiteral("violet"),
    QStringLiteral("emerald"), QStringLiteral("amber"), QStringLiteral("rose"),
};

QSettings settings()
{
    // Native format on Linux is ~/.config/nexus/appearance.conf, with automatic
    // fallback to /etc/xdg/nexus/appearance.conf — where the distribution
    // default is installed.
    return QSettings(QSettings::NativeFormat, QSettings::UserScope,
                     QStringLiteral("nexus"), QStringLiteral("appearance"));
}
} // namespace

Appearance *Appearance::instance()
{
    static Appearance *self = new Appearance;
    return self;
}

Appearance *Appearance::create(QQmlEngine *, QJSEngine *)
{
    Appearance *self = instance();
    QJSEngine::setObjectOwnership(self, QJSEngine::CppOwnership);
    return self;
}

bool Appearance::softwareRendering()
{
    // Asked once, on first use. QQuickWindow::sceneGraphBackend() resolves the
    // backend the way the first window will (QT_QUICK_BACKEND, an explicit
    // QQuickWindow::setSceneGraphBackend(), or the fallback when the platform
    // cannot do RHI rendering) and that choice is fixed for the process.
    // Without a QGuiApplication (--dump mode, unit tests of non-GUI code)
    // nothing is rendered at all.
    static const bool software = qobject_cast<QGuiApplication *>(QCoreApplication::instance())
        && QQuickWindow::sceneGraphBackend() == QLatin1String("software");
    return software;
}

QString Appearance::configPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
           + QStringLiteral("/nexus/appearance.conf");
}

Appearance::Appearance(QObject *parent)
    : QObject(parent)
{
    load();
    watch();
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { load(); watch(); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { load(); watch(); });
    connect(this, &Appearance::reduceMotionChanged, this, &Appearance::animateValuesChanged);
}

void Appearance::watch()
{
    // QSettings replaces the file atomically (rename), which drops a plain
    // file watch — so watch the directory as well and re-arm after each event.
    const QString path = configPath();
    const QString dir = QFileInfo(path).absolutePath();
    QDir().mkpath(dir);
    if (!m_watcher.directories().contains(dir))
        m_watcher.addPath(dir);
    if (QFileInfo::exists(path) && !m_watcher.files().contains(path))
        m_watcher.addPath(path);
}

void Appearance::load()
{
    QSettings s = settings();
    s.sync();
    s.beginGroup(QStringLiteral("Appearance"));

    const bool dark = s.value(QStringLiteral("ColorScheme"), QStringLiteral("dark")).toString()
                      != QStringLiteral("light");
    QString accent = s.value(QStringLiteral("Accent"), QStringLiteral("cyan")).toString();
    if (!kAccents.contains(accent))
        accent = QStringLiteral("cyan");
    const bool reduce = s.value(QStringLiteral("ReduceMotion"), false).toBool();
    const bool transparency = s.value(QStringLiteral("Transparency"), true).toBool();
    const qreal scale = qBound(0.8, s.value(QStringLiteral("FontScale"), 1.0).toDouble(), 1.6);

    if (dark != m_dark) { m_dark = dark; Q_EMIT darkChanged(); }
    if (accent != m_accent) { m_accent = accent; Q_EMIT accentChanged(); }
    if (reduce != m_reduceMotion) { m_reduceMotion = reduce; Q_EMIT reduceMotionChanged(); }
    if (transparency != m_transparency) { m_transparency = transparency; Q_EMIT transparencyChanged(); }
    if (!qFuzzyCompare(scale, m_fontScale)) { m_fontScale = scale; Q_EMIT fontScaleChanged(); }
}

void Appearance::store(const QString &key, const QVariant &value)
{
    QSettings s = settings();
    s.beginGroup(QStringLiteral("Appearance"));
    s.setValue(key, value);
    s.endGroup();
    s.sync();
    watch();
}

void Appearance::setDark(bool dark)
{
    if (dark == m_dark)
        return;
    m_dark = dark;
    store(QStringLiteral("ColorScheme"), dark ? QStringLiteral("dark") : QStringLiteral("light"));
    Q_EMIT darkChanged();
}

void Appearance::setAccent(const QString &accent)
{
    if (accent == m_accent || !kAccents.contains(accent))
        return;
    m_accent = accent;
    store(QStringLiteral("Accent"), accent);
    Q_EMIT accentChanged();
}

void Appearance::setReduceMotion(bool reduce)
{
    if (reduce == m_reduceMotion)
        return;
    m_reduceMotion = reduce;
    store(QStringLiteral("ReduceMotion"), reduce);
    Q_EMIT reduceMotionChanged();
}

void Appearance::setTransparency(bool on)
{
    if (on == m_transparency)
        return;
    m_transparency = on;
    store(QStringLiteral("Transparency"), on);
    Q_EMIT transparencyChanged();
}

void Appearance::setFontScale(qreal scale)
{
    scale = qBound(0.8, scale, 1.6);
    if (qFuzzyCompare(scale, m_fontScale))
        return;
    m_fontScale = scale;
    store(QStringLiteral("FontScale"), scale);
    Q_EMIT fontScaleChanged();
}

} // namespace Nexus
