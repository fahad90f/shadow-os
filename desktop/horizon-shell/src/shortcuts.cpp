// SPDX-License-Identifier: GPL-3.0-or-later
#include "shortcuts.h"

#include <KGlobalAccel>
#include <QAction>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QGuiApplication>
#include <QKeySequence>

Shortcuts::Shortcuts(QObject *parent)
    : QObject(parent)
{
    // Without a global-shortcut service (e.g. the shell started outside a
    // KWin session for testing) registration would block on D-Bus activation.
    QDBusConnectionInterface *bus = QDBusConnection::sessionBus().interface();
    m_available = QGuiApplication::platformName().startsWith(QLatin1String("wayland"))
                  && bus && bus->isServiceRegistered(QStringLiteral("org.kde.kglobalaccel"));
    if (!m_available)
        return;

    using K = QKeySequence;
    // Qt::META on its own is a "modifier-only" shortcut: it fires when the
    // Meta (Super/Windows) key is tapped without any other key or button.
    add("launcher", tr("Open the application launcher"), {K(Qt::META), K(Qt::ALT | Qt::Key_F1)});
    add("terminal", tr("Open a terminal"), {K(Qt::CTRL | Qt::ALT | Qt::Key_T), K(Qt::META | Qt::Key_Return)});
    add("files", tr("Open the file manager"), {K(Qt::META | Qt::Key_E)});
    add("settings", tr("Open the Control Center"), {K(Qt::META | Qt::Key_I)});
    add("quick-settings", tr("Show quick settings"), {K(Qt::META | Qt::Key_A)});
    add("notifications", tr("Show notifications"), {K(Qt::META | Qt::Key_N)});
    add("session-menu", tr("Show the power and session menu"), {K(Qt::CTRL | Qt::ALT | Qt::Key_Delete)});
    add("task-manager", tr("Open the task manager"), {K(Qt::CTRL | Qt::SHIFT | Qt::Key_Escape)});
    add("screenshot", tr("Take a screenshot"), {K(Qt::Key_Print)});
    add("screenshot-region", tr("Take a screenshot of a region"), {K(Qt::META | Qt::SHIFT | Qt::Key_S)});
    add("volume-up", tr("Increase volume"), {K(Qt::Key_VolumeUp)});
    add("volume-down", tr("Decrease volume"), {K(Qt::Key_VolumeDown)});
    add("volume-mute", tr("Mute"), {K(Qt::Key_VolumeMute)});
    add("mic-mute", tr("Mute the microphone"), {K(Qt::Key_MicMute)});
    add("brightness-up", tr("Increase screen brightness"), {K(Qt::Key_MonBrightnessUp)});
    add("brightness-down", tr("Decrease screen brightness"), {K(Qt::Key_MonBrightnessDown)});
    add("media-play", tr("Play or pause media"), {K(Qt::Key_MediaPlay), K(Qt::Key_MediaTogglePlayPause)});
    add("media-next", tr("Next track"), {K(Qt::Key_MediaNext)});
    add("media-previous", tr("Previous track"), {K(Qt::Key_MediaPrevious)});
}

void Shortcuts::add(const char *name, const QString &description, const QList<QKeySequence> &keys)
{
    auto *action = new QAction(this);
    action->setObjectName(QString::fromLatin1(name));
    action->setText(description);
    action->setProperty("componentName", QStringLiteral("nexus-horizon"));
    action->setProperty("componentDisplayName", QStringLiteral("NEXUS HORIZON"));
    KGlobalAccel::self()->setDefaultShortcut(action, keys);
    KGlobalAccel::self()->setShortcut(action, keys);      // keeps a binding the user changed
    const QString id = action->objectName();
    connect(action, &QAction::triggered, this, [this, id] { Q_EMIT triggered(id); });
    m_actions.append(action);
}

QVariantList Shortcuts::list() const
{
    QVariantList out;
    for (const QAction *a : m_actions) {
        QStringList keys;
        const QList<QKeySequence> seqs = KGlobalAccel::self()->shortcut(a);
        for (const QKeySequence &k : seqs)
            keys << k.toString(QKeySequence::NativeText);
        out << QVariantMap{{QStringLiteral("name"), a->objectName()}, {QStringLiteral("description"), a->text()},
                           {QStringLiteral("keys"), keys}};
    }
    return out;
}
