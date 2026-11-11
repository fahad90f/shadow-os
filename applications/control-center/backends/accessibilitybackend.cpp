// SPDX-License-Identifier: GPL-3.0-or-later
#include "accessibilitybackend.h"

#include "dumpregistry.h"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QEventLoop>
#include <QJsonArray>
#include <QKeySequence>
#include <QStandardPaths>
#include <QTimer>

#ifdef NEXUS_HAVE_KCONFIG
#include <KConfig>
#include <KConfigGroup>
#endif

NEXUS_CC_DUMP("accessibility", [] {
    AccessibilityBackend backend;
    if (!backend.ready()) {
        QEventLoop loop;
        QObject::connect(&backend, &AccessibilityBackend::changed, &loop, [&] { if (backend.ready()) loop.quit(); });
        QTimer::singleShot(8000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return QJsonValue(backend.toJson());
})

namespace {
const QString kKWin = QStringLiteral("org.kde.KWin");
const QString kEffectsPath = QStringLiteral("/Effects");
const QString kEffects = QStringLiteral("org.kde.kwin.Effects");
const QString kAccel = QStringLiteral("org.kde.kglobalaccel");
const QString kAccelComponentPath = QStringLiteral("/component/kwin");
const QString kAccelComponent = QStringLiteral("org.kde.kglobalaccel.Component");
const QString kProperties = QStringLiteral("org.freedesktop.DBus.Properties");

struct Action
{
    const char *name;       // kglobalaccel shortcut name of the kwin component
    const char *label;
};
struct EffectInfo
{
    const char *id;         // KWin effect (plugin) name
    const char *icon;
    const char *title;
    const char *description;
    QList<Action> actions;
    const char *excludes;   // effect that cannot be loaded at the same time
};

// The accessibility effects of KWin 6. Entries KWin does not report in
// listOfEffects are not shown.
const QList<EffectInfo> &effectTable()
{
    static const QList<EffectInfo> table = {
        {"zoom", "zoom-in", QT_TRANSLATE_NOOP("AccessibilityBackend", "Screen magnifier"),
         QT_TRANSLATE_NOOP("AccessibilityBackend", "Enlarges the whole screen; the view follows the pointer."),
         {{"view_zoom_in", QT_TRANSLATE_NOOP("AccessibilityBackend", "Zoom in")},
          {"view_zoom_out", QT_TRANSLATE_NOOP("AccessibilityBackend", "Zoom out")},
          {"view_actual_size", QT_TRANSLATE_NOOP("AccessibilityBackend", "Actual size")}},
         "magnifier"},
        {"magnifier", "search", QT_TRANSLATE_NOOP("AccessibilityBackend", "Magnifying glass"),
         QT_TRANSLATE_NOOP("AccessibilityBackend", "Shows an enlarged area around the pointer instead of zooming the whole screen."),
         {{"view_zoom_in", QT_TRANSLATE_NOOP("AccessibilityBackend", "Zoom in")},
          {"view_zoom_out", QT_TRANSLATE_NOOP("AccessibilityBackend", "Zoom out")},
          {"view_actual_size", QT_TRANSLATE_NOOP("AccessibilityBackend", "Actual size")}},
         "zoom"},
        {"invert", "eye", QT_TRANSLATE_NOOP("AccessibilityBackend", "Invert colours"),
         QT_TRANSLATE_NOOP("AccessibilityBackend", "Inverts the colours of the screen or of the active window when its shortcut is pressed."),
         {{"Invert", QT_TRANSLATE_NOOP("AccessibilityBackend", "Invert the screen")},
          {"InvertWindow", QT_TRANSLATE_NOOP("AccessibilityBackend", "Invert the active window")}},
         nullptr},
        {"colorblindnesscorrection", "palette", QT_TRANSLATE_NOOP("AccessibilityBackend", "Colour blindness correction"),
         QT_TRANSLATE_NOOP("AccessibilityBackend", "Shifts colours so that they are easier to tell apart."), {}, nullptr},
        {"shakecursor", "mouse", QT_TRANSLATE_NOOP("AccessibilityBackend", "Shake to find the pointer"),
         QT_TRANSLATE_NOOP("AccessibilityBackend", "The pointer grows for a moment when it is shaken."), {}, nullptr},
        {"trackmouse", "target", QT_TRANSLATE_NOOP("AccessibilityBackend", "Locate the pointer"),
         QT_TRANSLATE_NOOP("AccessibilityBackend", "Draws a marker around the pointer while Ctrl and Meta are held, or when its shortcut is pressed."),
         {{"TrackMouse", QT_TRANSLATE_NOOP("AccessibilityBackend", "Show or hide the marker")}}, nullptr},
        {"mouseclick", "circle", QT_TRANSLATE_NOOP("AccessibilityBackend", "Show mouse clicks"),
         QT_TRANSLATE_NOOP("AccessibilityBackend", "Animates a ring where a mouse button is pressed, once switched on with its shortcut."),
         {{"ToggleMouseClick", QT_TRANSLATE_NOOP("AccessibilityBackend", "Switch on or off")}}, nullptr},
    };
    return table;
}

const EffectInfo *effectInfo(const QString &id)
{
    for (const EffectInfo &info : effectTable()) {
        if (id == QLatin1String(info.id))
            return &info;
    }
    return nullptr;
}

QString trText(const char *text)
{
    return QCoreApplication::translate("AccessibilityBackend", text);
}

QDBusMessage effectsCall(const QString &method)
{
    return QDBusMessage::createMethodCall(kKWin, kEffectsPath, kEffects, method);
}
} // namespace

AccessibilityBackend::AccessibilityBackend(QObject *parent)
    : QObject(parent)
{
    loadCursor();
    loadStored();
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (bus.isConnected() && bus.interface()) {
        auto *watcher = new QDBusServiceWatcher(kKWin, bus, QDBusServiceWatcher::WatchForOwnerChange, this);
        connect(watcher, &QDBusServiceWatcher::serviceRegistered, this, [this] { refresh(); });
        connect(watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this] { setRunning(false); });
    }
    refresh();
}

bool AccessibilityBackend::canPersist() const
{
#ifdef NEXUS_HAVE_KCONFIG
    return true;
#else
    return false;
#endif
}

void AccessibilityBackend::setRunning(bool running)
{
    m_running = running;
    if (!running) {
        m_existing.clear();
        m_loaded.clear();
        m_supported.clear();
        m_shortcutKeys.clear();
        m_shortcutsAvailable = false;
        m_statusMessage = tr("The KWin compositor is not running in this session, so its accessibility effects cannot be "
                             "switched. They are available in the NEXUS HORIZON desktop session.");
    } else {
        m_statusMessage.clear();
    }
    m_ready = true;
    Q_EMIT changed();
}

void AccessibilityBackend::refresh()
{
    loadStored();
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected() || !bus.interface()) {
        setRunning(false);
        return;
    }
    // Asynchronous existence check (KWin is not D-Bus activatable).
    QDBusMessage message = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
                                                          QStringLiteral("org.freedesktop.DBus"), QStringLiteral("NameHasOwner"));
    message << kKWin;
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message, 5000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<bool> reply = *w;
        if (reply.isError() || !reply.value()) {
            setRunning(false);
            return;
        }
        fetchEffects();
        fetchShortcuts();
    });
}

void AccessibilityBackend::fetchEffects()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    QDBusMessage message = QDBusMessage::createMethodCall(kKWin, kEffectsPath, kProperties, QStringLiteral("GetAll"));
    message << kEffects;
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message, 8000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<QVariantMap> reply = *w;
        if (reply.isError()) {
            m_running = false;
            m_ready = true;
            m_statusMessage = tr("KWin did not answer on its effects interface: %1").arg(reply.error().message());
            Q_EMIT changed();
            return;
        }
        const QVariantMap properties = reply.value();
        m_existing = properties.value(QStringLiteral("listOfEffects")).toStringList();
        m_loaded = properties.value(QStringLiteral("loadedEffects")).toStringList();

        QStringList names;
        for (const EffectInfo &info : effectTable()) {
            if (m_existing.contains(QLatin1String(info.id)))
                names << QLatin1String(info.id);
        }
        QDBusMessage supported = effectsCall(QStringLiteral("areEffectsSupported"));
        supported << names;
        auto *second = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(supported, 8000), this);
        connect(second, &QDBusPendingCallWatcher::finished, this, [this, names](QDBusPendingCallWatcher *w2) {
            w2->deleteLater();
            const QDBusPendingReply<QList<bool>> reply2 = *w2;
            m_supported.clear();
            if (!reply2.isError()) {
                const QList<bool> flags = reply2.value();
                for (int i = 0; i < names.size() && i < flags.size(); ++i)
                    m_supported.insert(names.at(i), flags.at(i));
            } else {
                // Older KWin: assume what it lists can be loaded; loadEffect reports otherwise.
                for (const QString &name : names)
                    m_supported.insert(name, true);
            }
            // Which renderer the compositor uses decides whether effects can draw at all.
            QDBusMessage type = QDBusMessage::createMethodCall(kKWin, QStringLiteral("/Compositor"), kProperties, QStringLiteral("Get"));
            type << QStringLiteral("org.kde.kwin.Compositing") << QStringLiteral("compositingType");
            auto *third = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(type, 8000), this);
            connect(third, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w3) {
                w3->deleteLater();
                const QDBusPendingReply<QDBusVariant> reply3 = *w3;
                m_compositingType = reply3.isError() ? QString() : reply3.value().variant().toString();
                if (softwareRendering()) {
                    for (auto it = m_supported.begin(); it != m_supported.end(); ++it)
                        it.value() = false;
                }
                setRunning(true);
            });
        });
    });
}

void AccessibilityBackend::fetchShortcuts()
{
    QDBusMessage message = QDBusMessage::createMethodCall(kAccel, kAccelComponentPath, kAccelComponent, QStringLiteral("allShortcutInfos"));
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message, 8000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusMessage reply = w->reply();
        m_shortcutKeys.clear();
        m_shortcutsAvailable = reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty();
        if (m_shortcutsAvailable) {
            // a(ssssssaiai): action, action name, component, component name,
            // context, context name, keys, default keys
            const QDBusArgument argument = reply.arguments().constFirst().value<QDBusArgument>();
            argument.beginArray();
            while (!argument.atEnd()) {
                QString text[6];
                QList<int> keys, defaults;
                argument.beginStructure();
                for (QString &field : text)
                    argument >> field;
                argument >> keys >> defaults;
                argument.endStructure();
                QStringList sequences;
                for (int key : std::as_const(keys)) {
                    if (key != 0)
                        sequences << QKeySequence(key).toString(QKeySequence::NativeText);
                }
                m_shortcutKeys.insert(text[0], sequences.join(QStringLiteral(", ")));
            }
            argument.endArray();
        }
        Q_EMIT changed();
    });
}

void AccessibilityBackend::loadStored()
{
    m_stored.clear();
#ifdef NEXUS_HAVE_KCONFIG
    KConfig config(QStringLiteral("kwinrc"), KConfig::NoGlobals);
    const KConfigGroup plugins = config.group(QStringLiteral("Plugins"));
    for (const EffectInfo &info : effectTable()) {
        const QString key = QLatin1String(info.id) + QStringLiteral("Enabled");
        if (plugins.hasKey(key))
            m_stored.insert(QLatin1String(info.id), plugins.readEntry(key, false));
    }
    m_colorblindMode = config.group(QStringLiteral("Effect-colorblindnesscorrection")).readEntry("Mode", 0);
#endif
}

QVariantList AccessibilityBackend::effects() const
{
    QVariantList out;
    for (const EffectInfo &info : effectTable()) {
        const QString id = QLatin1String(info.id);
        if (!m_existing.contains(id))
            continue;
        QVariantList actions;
        for (const Action &action : info.actions) {
            const QString name = QLatin1String(action.name);
            if (m_shortcutsAvailable && !m_shortcutKeys.contains(name))
                continue;       // this KWin does not register the action
            actions << QVariantMap{
                {QStringLiteral("name"), name},
                {QStringLiteral("label"), trText(action.label)},
                {QStringLiteral("keys"), m_shortcutKeys.value(name)},
            };
        }
        out << QVariantMap{
            {QStringLiteral("id"), id},
            {QStringLiteral("icon"), QLatin1String(info.icon)},
            {QStringLiteral("title"), trText(info.title)},
            {QStringLiteral("description"), trText(info.description)},
            {QStringLiteral("supported"), m_supported.value(id, false)},
            {QStringLiteral("loaded"), m_loaded.contains(id)},
            {QStringLiteral("storedKnown"), m_stored.contains(id)},
            {QStringLiteral("stored"), m_stored.value(id, false)},
            {QStringLiteral("actions"), actions},
        };
    }
    return out;
}

bool AccessibilityBackend::store(const QString &id, bool enabled)
{
#ifdef NEXUS_HAVE_KCONFIG
    KConfig config(QStringLiteral("kwinrc"), KConfig::NoGlobals);
    KConfigGroup plugins = config.group(QStringLiteral("Plugins"));
    plugins.writeEntry(id + QStringLiteral("Enabled"), enabled);
    return config.sync();
#else
    Q_UNUSED(id)
    Q_UNUSED(enabled)
    return false;
#endif
}

void AccessibilityBackend::applyEffect(const QString &id, bool enabled, bool reportFailure)
{
    QDBusMessage message = effectsCall(enabled ? QStringLiteral("loadEffect") : QStringLiteral("unloadEffect"));
    message << id;
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message, 15000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, id, enabled, reportFailure](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusMessage reply = w->reply();
        const bool failed = reply.type() != QDBusMessage::ReplyMessage
                            || (enabled && !reply.arguments().isEmpty() && !reply.arguments().constFirst().toBool());
        if (failed && reportFailure) {
            const EffectInfo *info = effectInfo(id);
            Q_EMIT this->failed(tr("KWin could not %1 “%2”.")
                                    .arg(enabled ? tr("switch on") : tr("switch off"), info ? trText(info->title) : id));
            // Do not leave a setting stored that the compositor refused.
            if (enabled)
                store(id, false);
        }
        refresh();
    });
}

void AccessibilityBackend::setEffectEnabled(const QString &id, bool enabled)
{
    const EffectInfo *info = effectInfo(id);
    if (!info || !m_running || !m_existing.contains(id)) {
        Q_EMIT failed(m_running ? tr("This KWin has no effect named “%1”.").arg(id) : m_statusMessage);
        return;
    }
    if (enabled && !m_supported.value(id, false)) {
        Q_EMIT failed(tr("The compositor cannot run “%1” with its current renderer.").arg(trText(info->title)));
        return;
    }
    // Zoom and the magnifying glass use the same shortcuts and exclude each other.
    if (enabled && info->excludes) {
        const QString other = QLatin1String(info->excludes);
        if (m_loaded.contains(other) || m_stored.value(other, false)) {
            store(other, false);
            applyEffect(other, false, false);
        }
    }
    if (!store(id, enabled)) {
        Q_EMIT failed(canPersist() ? tr("The setting could not be saved to kwinrc; it applies only until you log out.")
                                   : tr("This build has no KConfig support: the setting applies only until you log out."));
    }
    applyEffect(id, enabled, true);
}

void AccessibilityBackend::trigger(const QString &shortcutName)
{
    bool known = false;
    for (const EffectInfo &info : effectTable()) {
        for (const Action &action : info.actions)
            known = known || shortcutName == QLatin1String(action.name);
    }
    if (!known || !m_running || softwareRendering())
        return;
    QDBusMessage message = QDBusMessage::createMethodCall(kAccel, kAccelComponentPath, kAccelComponent, QStringLiteral("invokeShortcut"));
    message << shortcutName;
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message, 8000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        if (w->isError())
            Q_EMIT failed(tr("The shortcut service did not accept the request: %1").arg(w->error().message()));
    });
}

void AccessibilityBackend::setColorblindMode(int mode)
{
    if (mode < 0 || mode > 2 || mode == m_colorblindMode)
        return;
#ifdef NEXUS_HAVE_KCONFIG
    KConfig config(QStringLiteral("kwinrc"), KConfig::NoGlobals);
    config.group(QStringLiteral("Effect-colorblindnesscorrection")).writeEntry("Mode", mode);
    if (!config.sync()) {
        Q_EMIT failed(tr("The setting could not be saved to kwinrc."));
        return;
    }
    m_colorblindMode = mode;
    if (m_running) {
        QDBusMessage message = effectsCall(QStringLiteral("reconfigureEffect"));
        message << QStringLiteral("colorblindnesscorrection");
        QDBusConnection::sessionBus().send(message);
    }
    Q_EMIT changed();
#else
    Q_EMIT failed(tr("This build has no KConfig support, so the correction mode cannot be changed."));
#endif
}

// ---- pointer size -------------------------------------------------------------------------

void AccessibilityBackend::loadCursor()
{
    int size = 24;
    QString theme;
#ifdef NEXUS_HAVE_KCONFIG
    const KConfig config(QStringLiteral("kcminputrc"), KConfig::NoGlobals);
    const KConfigGroup mouse = config.group(QStringLiteral("Mouse"));
    size = mouse.readEntry("cursorSize", 24);
    theme = mouse.readEntry("cursorTheme", QString());
#endif
    if (theme.isEmpty())
        theme = qEnvironmentVariable("XCURSOR_THEME");
    if (size != m_cursorSize || theme != m_cursorTheme) {
        m_cursorSize = size;
        m_cursorTheme = theme;
        Q_EMIT cursorChanged();
    }
}

QVariantList AccessibilityBackend::cursorSizes() const
{
    QList<int> sizes = {24, 32, 36, 48, 64, 72, 96};
    if (!sizes.contains(m_cursorSize))
        sizes << m_cursorSize;
    std::sort(sizes.begin(), sizes.end());
    QVariantList out;
    for (int size : std::as_const(sizes)) {
        out << QVariantMap{
            {QStringLiteral("value"), size},
            {QStringLiteral("text"), size == 24 ? tr("24 (standard)") : QString::number(size)},
        };
    }
    return out;
}

void AccessibilityBackend::setCursorSize(int size)
{
    if (size < 16 || size > 256 || size == m_cursorSize)
        return;
#ifdef NEXUS_HAVE_KCONFIG
    KConfig config(QStringLiteral("kcminputrc"), KConfig::NoGlobals);
    KConfigGroup mouse = config.group(QStringLiteral("Mouse"));
    mouse.writeEntry("cursorSize", size, KConfig::Notify);
    if (!config.sync()) {
        Q_EMIT failed(tr("The input configuration (kcminputrc) could not be written."));
        return;
    }
    // Applications of the KDE platform reload their cursors on this signal
    // (KGlobalSettings::CursorChanged = 5).
    QDBusMessage signal = QDBusMessage::createSignal(QStringLiteral("/KGlobalSettings"), QStringLiteral("org.kde.KGlobalSettings"),
                                                     QStringLiteral("notifyChange"));
    signal << 5 << 0;
    QDBusConnection::sessionBus().send(signal);
    loadCursor();
#else
    Q_EMIT failed(tr("This build has no KConfig support, so the pointer size cannot be changed."));
#endif
}

QJsonObject AccessibilityBackend::toJson() const
{
    QJsonArray effects;
    const QVariantList list = this->effects();
    for (const QVariant &effect : list)
        effects << QJsonObject::fromVariantMap(effect.toMap());
    return {
        {QStringLiteral("compositorRunning"), m_running},
        {QStringLiteral("statusMessage"), m_statusMessage},
        {QStringLiteral("canPersist"), canPersist()},
        {QStringLiteral("compositingType"), m_compositingType},
        {QStringLiteral("softwareRendering"), softwareRendering()},
        {QStringLiteral("effects"), effects},
        {QStringLiteral("loadedEffects"), QJsonArray::fromStringList(m_loaded)},
        {QStringLiteral("shortcutsAvailable"), m_shortcutsAvailable},
        {QStringLiteral("colorblindMode"), m_colorblindMode},
        {QStringLiteral("cursorSize"), m_cursorSize},
        {QStringLiteral("cursorTheme"), m_cursorTheme},
        {QStringLiteral("sessionCursorSize"), sessionCursorSize()},
    };
}
