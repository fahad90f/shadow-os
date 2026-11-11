// SPDX-License-Identifier: GPL-3.0-or-later
#include "keyboardbackend.h"

#include "dumpregistry.h"
#include "networkcommon.h"

#include <QCollator>
#include <QDBusConnection>
#include <QDBusServiceWatcher>
#include <QFile>
#include <QJsonArray>
#include <QKeySequence>
#include <QRegularExpression>
#include <QTimer>

#include <memory>

#ifdef SHADOW_HAVE_KCONFIG
#include <KConfig>
#include <KConfigGroup>
#endif

using namespace NetHw;

SHADOW_CC_DUMP("keyboard", [] {
    KeyboardBackend backend;
    waitUntilReady(&backend);
    return QJsonValue(backend.toJson());
})

namespace {
const QString kKwinService = QStringLiteral("org.kde.KWin");
const QString kKeyboardService = QStringLiteral("org.kde.keyboard");
const QString kLayoutsPath = QStringLiteral("/Layouts");
const QString kLayoutsInterface = QStringLiteral("org.kde.KeyboardLayouts");

const QString kAccelService = QStringLiteral("org.kde.kglobalaccel");
const QString kAccelPath = QStringLiteral("/kglobalaccel");
const QString kAccelInterface = QStringLiteral("org.kde.KGlobalAccel");
const QString kAccelComponentInterface = QStringLiteral("org.kde.kglobalaccel.Component");

// KGlobalAccel sends each binding as the combined Qt key code (modifiers |
// key) of its first chord; 0 stands for "no key".
QString keyText(int combined)
{
    if (combined == 0)
        return QString();
    QString text = QKeySequence(QKeyCombination::fromCombined(combined)).toString(QKeySequence::PortableText);
    // Qt calls the logo key "Meta"; on a PC keyboard it is the Super key.
    text.replace(QLatin1String("Meta+"), QLatin1String("Super+"));
    if (text == QLatin1String("Meta"))
        text = QStringLiteral("Super");
    return text;
}

QStringList keyTexts(const QVariant &list)
{
    QStringList texts;
    const QVariantList keys = list.toList();
    for (const QVariant &key : keys) {
        const QString text = keyText(key.toInt());
        if (!text.isEmpty() && !texts.contains(text))
            texts.append(text);
    }
    return texts;
}

QVariantMap optionItem(const QString &value, const QString &text)
{
    return {{QStringLiteral("value"), value}, {QStringLiteral("text"), text}};
}

bool listHasValue(const QVariantList &items, const QString &value)
{
    for (const QVariant &item : items) {
        if (item.toMap().value(QStringLiteral("value")).toString() == value)
            return true;
    }
    return false;
}

// VariantList must have exactly one (possibly empty) entry per layout.
QStringList paddedVariants(QStringList variants, qsizetype count)
{
    while (variants.size() < count)
        variants.append(QString());
    return variants.mid(0, count);
}
} // namespace

KeyboardBackend::KeyboardBackend(QObject *parent)
    : QObject(parent)
{
    m_layoutModel = new RowModel({QStringLiteral("key"), QStringLiteral("layoutCode"), QStringLiteral("variantCode"),
                                  QStringLiteral("name"), QStringLiteral("position"), QStringLiteral("active")},
                                 QStringLiteral("key"), this);

    m_shortcutModel = new RowModel({QStringLiteral("key"), QStringLiteral("component"), QStringLiteral("componentName"),
                                    QStringLiteral("action"), QStringLiteral("name"), QStringLiteral("keys"),
                                    QStringLiteral("assigned"), QStringLiteral("custom")},
                                   QStringLiteral("key"), this);

    loadCatalog();
    loadConfig();

    QDBusConnection session = QDBusConnection::sessionBus();
    if (session.isConnected()) {
        auto *accelWatcher = new QDBusServiceWatcher(kAccelService, session, QDBusServiceWatcher::WatchForOwnerChange, this);
        connect(accelWatcher, &QDBusServiceWatcher::serviceRegistered, this, &KeyboardBackend::refreshShortcuts);
        connect(accelWatcher, &QDBusServiceWatcher::serviceUnregistered, this, &KeyboardBackend::refreshShortcuts);
        auto *watcher = new QDBusServiceWatcher(kKwinService, session, QDBusServiceWatcher::WatchForOwnerChange, this);
        connect(watcher, &QDBusServiceWatcher::serviceRegistered, this, [this] { setCompositorRunning(true); });
        connect(watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this] { setCompositorRunning(false); });
        session.connect(kKeyboardService, kLayoutsPath, kLayoutsInterface, QStringLiteral("layoutListChanged"),
                        this, SLOT(queryActiveLayouts()));
        session.connect(kKeyboardService, kLayoutsPath, kLayoutsInterface, QStringLiteral("layoutChanged"),
                        this, SLOT(queryActiveLayouts()));
        ++m_pendingSteps;
        dbusNameHasOwner(session, kKwinService, this, [this](bool owned) {
            setCompositorRunning(owned);
            markStep();
        });
    }

    // Counted before querySystemLayout(), which may finish synchronously.
    ++m_pendingSteps;
    refreshShortcuts();

    ++m_pendingSteps;
    querySystemLayout();
}

QAbstractItemModel *KeyboardBackend::layouts() const
{
    return m_layoutModel;
}

void KeyboardBackend::markStep()
{
    if (m_pendingSteps > 0 && --m_pendingSteps == 0 && !m_ready) {
        m_ready = true;
        Q_EMIT readyChanged();
    }
}

void KeyboardBackend::refresh()
{
    loadConfig();
    queryActiveLayouts();
    refreshShortcuts();
}

// ---- catalogue ------------------------------------------------------------------

void KeyboardBackend::loadCatalog()
{
    static const QStringList candidates = {
        QStringLiteral("/usr/share/X11/xkb/rules/evdev.lst"),
        QStringLiteral("/usr/share/X11/xkb/rules/base.lst"),
    };
    QFile file;
    for (const QString &path : candidates) {
        file.setFileName(path);
        if (file.open(QIODevice::ReadOnly)) {
            m_catalogFile = path;
            break;
        }
    }

    m_switchOptions = {optionItem(QString(), tr("None"))};
    m_composeOptions = {optionItem(QString(), tr("None"))};
    m_capsOptions = {optionItem(QString(), tr("Default"))};
    if (!file.isOpen())
        return;

    static const QRegularExpression entry(QStringLiteral("^\\s+(\\S+)\\s+(.*)$"));
    QString section;
    QList<QPair<QString, QString>> layouts;
    const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        if (line.startsWith(QLatin1Char('!'))) {
            section = line.mid(1).trimmed();
            continue;
        }
        const QRegularExpressionMatch match = entry.match(line);
        if (!match.hasMatch())
            continue;
        const QString name = match.captured(1);
        const QString description = match.captured(2).trimmed();
        if (section == QLatin1String("layout")) {
            if (name == QLatin1String("custom"))
                continue;               // needs a hand-written symbols file
            m_layoutNames.insert(name, description);
            layouts.append({name, description});
        } else if (section == QLatin1String("variant")) {
            // "chr             us: Cherokee"
            const qsizetype colon = description.indexOf(QLatin1Char(':'));
            if (colon <= 0)
                continue;
            m_variants[description.left(colon).trimmed()].append({name, description.mid(colon + 1).trimmed()});
        } else if (section == QLatin1String("option")) {
            if (name.startsWith(QLatin1String("grp:")))
                m_switchOptions.append(optionItem(name, description));
            else if (name.startsWith(QLatin1String("compose:")))
                m_composeOptions.append(optionItem(name, description));
            else if (name.startsWith(QLatin1String("caps:")))
                m_capsOptions.append(optionItem(name, description));
        }
    }

    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(layouts.begin(), layouts.end(), [&collator](const auto &a, const auto &b) {
        return collator.compare(a.second, b.second) < 0;
    });
    m_availableLayouts.clear();
    for (const auto &layout : std::as_const(layouts)) {
        m_availableLayouts.append(QVariantMap{
            {QStringLiteral("layout"), layout.first},
            {QStringLiteral("name"), layout.second},
            {QStringLiteral("variants"), int(m_variants.value(layout.first).size())},
        });
    }
    Q_EMIT catalogChanged();
}

QVariantList KeyboardBackend::variantsFor(const QString &layout) const
{
    QVariantList list;
    if (!m_layoutNames.contains(layout))
        return list;
    list.append(QVariantMap{{QStringLiteral("variant"), QString()},
                            {QStringLiteral("name"), tr("%1 (default)").arg(m_layoutNames.value(layout))}});
    const auto variants = m_variants.value(layout);
    for (const auto &variant : variants)
        list.append(QVariantMap{{QStringLiteral("variant"), variant.first}, {QStringLiteral("name"), variant.second}});
    return list;
}

QString KeyboardBackend::layoutName(const QString &layout, const QString &variant) const
{
    if (!variant.isEmpty()) {
        const auto variants = m_variants.value(layout);
        for (const auto &candidate : variants) {
            if (candidate.first == variant)
                return candidate.second;
        }
        return QStringLiteral("%1 (%2)").arg(m_layoutNames.value(layout, layout), variant);
    }
    return m_layoutNames.value(layout, layout);
}

// ---- configuration ----------------------------------------------------------------

void KeyboardBackend::loadConfig()
{
    QString state = QStringLiteral("ok");
    QString message;
    QStringList layouts, variants, options;
    int delay = 600, rate = 25;
    bool repeatConfigured = false;

#ifdef SHADOW_HAVE_KCONFIG
    {
        const KConfig xkb(QStringLiteral("kxkbrc"), KConfig::NoGlobals);
        const KConfigGroup group = xkb.group(QStringLiteral("Layout"));
        if (group.readEntry("Use", false)) {
            layouts = group.readEntry("LayoutList", QString()).split(QLatin1Char(','), Qt::SkipEmptyParts);
            variants = paddedVariants(group.readEntry("VariantList", QString()).split(QLatin1Char(',')), layouts.size());
        }
        // KWin ignores Options unless ResetOldOptions is set.
        if (group.readEntry("ResetOldOptions", false))
            options = group.readEntry("Options", QString()).split(QLatin1Char(','), Qt::SkipEmptyParts);

        const KConfig input(QStringLiteral("kcminputrc"), KConfig::NoGlobals);
        const KConfigGroup keyboard = input.group(QStringLiteral("Keyboard"));
        repeatConfigured = keyboard.hasKey("RepeatDelay") || keyboard.hasKey("RepeatRate");
        delay = keyboard.readEntry("RepeatDelay", 600);
        rate = qRound(keyboard.readEntry("RepeatRate", 25.0));
    }
    if (m_catalogFile.isEmpty()) {
        state = QStringLiteral("no-xkb-data");
        message = tr("The keyboard layout catalogue /usr/share/X11/xkb/rules/evdev.lst is missing. Install the xkb-data package.");
    }
#else
    state = QStringLiteral("unsupported-build");
    message = tr("This build of the Control Center has no KConfig support, so the keyboard settings of the "
                 "KWin session (kxkbrc, kcminputrc) cannot be read or changed.");
#endif

    for (QString &layout : layouts)
        layout = layout.trimmed();
    for (QString &option : options)
        option = option.trimmed();

    if (state != m_state || message != m_statusMessage) {
        m_state = state;
        m_statusMessage = message;
        Q_EMIT stateChanged();
    }
    if (layouts != m_layoutList || variants != m_variantList) {
        m_layoutList = layouts;
        m_variantList = variants;
        Q_EMIT layoutsChanged();
    }
    rebuildLayoutModel();
    if (options != m_options) {
        m_options = options;
        Q_EMIT optionsChanged();
    }
    if (delay != m_repeatDelay || rate != m_repeatRate || repeatConfigured != m_repeatConfigured) {
        m_repeatDelay = delay;
        m_repeatRate = rate;
        m_repeatConfigured = repeatConfigured;
        Q_EMIT repeatChanged();
    }
}

void KeyboardBackend::rebuildLayoutModel()
{
    QVariantList rows;
    // KWin's list mirrors ours once it has reloaded; only then is its index meaningful.
    const bool comparable = m_activeLayouts.size() == m_layoutList.size();
    for (int i = 0; i < m_layoutList.size(); ++i) {
        const QString layout = m_layoutList.at(i);
        const QString variant = m_variantList.value(i);
        rows.append(QVariantMap{
            {QStringLiteral("key"), layout + QLatin1Char(':') + variant},
            {QStringLiteral("layoutCode"), layout},
            {QStringLiteral("variantCode"), variant},
            {QStringLiteral("name"), layoutName(layout, variant)},
            {QStringLiteral("position"), i},
            {QStringLiteral("active"), comparable && m_layoutList.size() > 1 && i == m_activeLayoutIndex},
        });
    }
    m_layoutModel->setRows(rows);
}

bool KeyboardBackend::saveLayoutConfig()
{
#ifdef SHADOW_HAVE_KCONFIG
    KConfig xkb(QStringLiteral("kxkbrc"), KConfig::NoGlobals);
    KConfigGroup group = xkb.group(QStringLiteral("Layout"));
    group.writeEntry("Use", !m_layoutList.isEmpty());
    group.writeEntry("LayoutList", m_layoutList.join(QLatin1Char(',')));
    group.writeEntry("VariantList", paddedVariants(m_variantList, m_layoutList.size()).join(QLatin1Char(',')));
    group.writeEntry("Options", m_options.join(QLatin1Char(',')));
    group.writeEntry("ResetOldOptions", true);
    if (!xkb.sync()) {
        Q_EMIT failed(tr("The keyboard configuration (kxkbrc) could not be written."));
        return false;
    }
    notifyCompositor();
    return true;
#else
    Q_EMIT failed(m_statusMessage);
    return false;
#endif
}

void KeyboardBackend::notifyCompositor()
{
    QDBusConnection session = QDBusConnection::sessionBus();
    if (!session.isConnected())
        return;
    // KWin listens for this signal and re-reads kxkbrc.
    session.send(QDBusMessage::createSignal(kLayoutsPath, kKeyboardService, QStringLiteral("reloadConfig")));
    if (m_compositorRunning) {
        // Key repeat is part of KWin's input configuration.
        session.send(QDBusMessage::createMethodCall(kKwinService, QStringLiteral("/KWin"), kKwinService,
                                                    QStringLiteral("reconfigure")));
        QTimer::singleShot(400, this, &KeyboardBackend::queryActiveLayouts);
    }
}

bool KeyboardBackend::addLayout(const QString &layout, const QString &variant)
{
    if (!available()) {
        Q_EMIT failed(m_statusMessage);
        return false;
    }
    if (!m_layoutNames.contains(layout)) {
        Q_EMIT failed(tr("“%1” is not a known keyboard layout.").arg(layout));
        return false;
    }
    if (!variant.isEmpty()) {
        bool known = false;
        const auto variants = m_variants.value(layout);
        for (const auto &candidate : variants)
            known = known || candidate.first == variant;
        if (!known) {
            Q_EMIT failed(tr("“%1” is not a variant of the layout “%2”.").arg(variant, layout));
            return false;
        }
    }
    if (m_layoutList.size() >= maxLayouts()) {
        Q_EMIT failed(tr("No more than %1 layouts can be active at the same time.").arg(maxLayouts()));
        return false;
    }
    for (int i = 0; i < m_layoutList.size(); ++i) {
        if (m_layoutList.at(i) == layout && m_variantList.value(i) == variant) {
            Q_EMIT failed(tr("%1 is already in the list.").arg(layoutName(layout, variant)));
            return false;
        }
    }
    const QStringList layoutsBefore = m_layoutList, variantsBefore = m_variantList;
    m_layoutList.append(layout);
    m_variantList = paddedVariants(m_variantList, m_layoutList.size() - 1);
    m_variantList.append(variant);
    if (!saveLayoutConfig()) {
        m_layoutList = layoutsBefore;
        m_variantList = variantsBefore;
        return false;
    }
    Q_EMIT layoutsChanged();
    rebuildLayoutModel();
    return true;
}

bool KeyboardBackend::removeLayout(int index)
{
    if (!available() || index < 0 || index >= m_layoutList.size())
        return false;
    const QStringList layoutsBefore = m_layoutList, variantsBefore = m_variantList;
    m_variantList = paddedVariants(m_variantList, m_layoutList.size());
    m_layoutList.removeAt(index);
    m_variantList.removeAt(index);
    if (!saveLayoutConfig()) {
        m_layoutList = layoutsBefore;
        m_variantList = variantsBefore;
        return false;
    }
    Q_EMIT layoutsChanged();
    rebuildLayoutModel();
    return true;
}

bool KeyboardBackend::moveLayout(int from, int to)
{
    if (!available() || from < 0 || from >= m_layoutList.size() || to < 0 || to >= m_layoutList.size() || from == to)
        return false;
    const QStringList layoutsBefore = m_layoutList, variantsBefore = m_variantList;
    m_variantList = paddedVariants(m_variantList, m_layoutList.size());
    m_layoutList.move(from, to);
    m_variantList.move(from, to);
    if (!saveLayoutConfig()) {
        m_layoutList = layoutsBefore;
        m_variantList = variantsBefore;
        return false;
    }
    Q_EMIT layoutsChanged();
    rebuildLayoutModel();
    return true;
}

// ---- xkb options -------------------------------------------------------------------

QString KeyboardBackend::optionWithPrefix(const QString &prefix) const
{
    for (const QString &option : m_options) {
        if (option.startsWith(prefix))
            return option;
    }
    return QString();
}

QStringList KeyboardBackend::otherOptions() const
{
    QStringList other;
    for (const QString &option : m_options) {
        if (!option.startsWith(QLatin1String("grp:")) && !option.startsWith(QLatin1String("compose:"))
            && !option.startsWith(QLatin1String("caps:")))
            other.append(option);
    }
    return other;
}

bool KeyboardBackend::setOptionWithPrefix(const QString &prefix, const QString &option, const QVariantList &allowed)
{
    if (!available()) {
        Q_EMIT failed(m_statusMessage);
        return false;
    }
    if (!option.isEmpty() && (!option.startsWith(prefix) || !listHasValue(allowed, option))) {
        Q_EMIT failed(tr("“%1” is not a known keyboard option.").arg(option));
        return false;
    }
    const QStringList before = m_options;
    QStringList next;
    bool placed = false;
    for (const QString &existing : before) {
        if (!existing.startsWith(prefix)) {
            next.append(existing);
        } else if (!placed && !option.isEmpty()) {
            next.append(option);        // keep the position of the option it replaces
            placed = true;
        }
    }
    if (!placed && !option.isEmpty())
        next.append(option);
    if (next == before)
        return true;
    m_options = next;
    if (!saveLayoutConfig()) {
        m_options = before;
        return false;
    }
    Q_EMIT optionsChanged();
    return true;
}

bool KeyboardBackend::setSwitchOption(const QString &option)
{
    return setOptionWithPrefix(QStringLiteral("grp:"), option, m_switchOptions);
}

bool KeyboardBackend::setComposeOption(const QString &option)
{
    return setOptionWithPrefix(QStringLiteral("compose:"), option, m_composeOptions);
}

bool KeyboardBackend::setCapsOption(const QString &option)
{
    return setOptionWithPrefix(QStringLiteral("caps:"), option, m_capsOptions);
}

// ---- key repeat ----------------------------------------------------------------------

bool KeyboardBackend::setRepeat(int delayMs, int ratePerSecond)
{
    if (!available()) {
        Q_EMIT failed(m_statusMessage);
        return false;
    }
    if (delayMs < 100 || delayMs > 2000 || ratePerSecond < 1 || ratePerSecond > 100) {
        Q_EMIT failed(tr("The repeat delay must be between 100 and 2000 ms and the rate between 1 and 100 per second."));
        return false;
    }
    if (delayMs == m_repeatDelay && ratePerSecond == m_repeatRate && m_repeatConfigured)
        return true;
#ifdef SHADOW_HAVE_KCONFIG
    KConfig input(QStringLiteral("kcminputrc"), KConfig::NoGlobals);
    KConfigGroup keyboard = input.group(QStringLiteral("Keyboard"));
    keyboard.writeEntry("RepeatDelay", delayMs, KConfig::Notify);
    keyboard.writeEntry("RepeatRate", double(ratePerSecond), KConfig::Notify);
    if (!input.sync()) {
        Q_EMIT failed(tr("The input configuration (kcminputrc) could not be written."));
        return false;
    }
    m_repeatDelay = delayMs;
    m_repeatRate = ratePerSecond;
    m_repeatConfigured = true;
    Q_EMIT repeatChanged();
    QDBusConnection session = QDBusConnection::sessionBus();
    if (session.isConnected() && m_compositorRunning) {
        session.send(QDBusMessage::createMethodCall(kKwinService, QStringLiteral("/KWin"), kKwinService,
                                                    QStringLiteral("reconfigure")));
    }
    return true;
#else
    return false;
#endif
}

// ---- live state ------------------------------------------------------------------------

void KeyboardBackend::setCompositorRunning(bool running)
{
    if (running != m_compositorRunning) {
        m_compositorRunning = running;
        Q_EMIT compositorChanged();
    }
    queryActiveLayouts();
}

void KeyboardBackend::queryActiveLayouts()
{
    QDBusConnection session = QDBusConnection::sessionBus();
    if (!m_compositorRunning || !session.isConnected()) {
        if (!m_activeLayouts.isEmpty() || m_activeLayoutIndex != -1) {
            m_activeLayouts.clear();
            m_activeLayoutIndex = -1;
            Q_EMIT activeLayoutsChanged();
            rebuildLayoutModel();
        }
        return;
    }
    dbusCall(session, dbusMethod(kKeyboardService, kLayoutsPath, kLayoutsInterface, QStringLiteral("getLayoutsList")),
             this, [this, session](const QDBusMessage &reply) {
        QStringList names;
        if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
            // a(sss): short name, display name, description
            const QVariantList entries = dbusPlain(reply.arguments().constFirst()).toList();
            for (const QVariant &entry : entries) {
                const QVariantList fields = entry.toList();
                if (fields.size() >= 3)
                    names.append(fields.at(2).toString());
            }
        }
        dbusCall(session, dbusMethod(kKeyboardService, kLayoutsPath, kLayoutsInterface, QStringLiteral("getLayout")),
                 this, [this, names](const QDBusMessage &indexReply) {
            int index = -1;
            if (indexReply.type() == QDBusMessage::ReplyMessage && !indexReply.arguments().isEmpty())
                index = indexReply.arguments().constFirst().toInt();
            if (names != m_activeLayouts || index != m_activeLayoutIndex) {
                m_activeLayouts = names;
                m_activeLayoutIndex = index;
                Q_EMIT activeLayoutsChanged();
                rebuildLayoutModel();
            }
        });
    });
}

void KeyboardBackend::querySystemLayout()
{
    QDBusConnection system = QDBusConnection::systemBus();
    if (!system.isConnected()) {
        readDefaultKeyboardFile();
        markStep();
        return;
    }
    dbusGetAll(system, QStringLiteral("org.freedesktop.locale1"), QStringLiteral("/org/freedesktop/locale1"),
               QStringLiteral("org.freedesktop.locale1"), this,
               [this](bool ok, const QVariantMap &properties, const QString &) {
        const QString layout = properties.value(QStringLiteral("X11Layout")).toString();
        if (ok && !layout.isEmpty()) {
            const QStringList layouts = layout.split(QLatin1Char(','));
            const QStringList variants = properties.value(QStringLiteral("X11Variant")).toString().split(QLatin1Char(','));
            QStringList names;
            for (int i = 0; i < layouts.size(); ++i)
                names.append(layoutName(layouts.at(i).trimmed(), variants.value(i).trimmed()));
            m_systemLayout = names.join(QStringLiteral(", "));
            m_systemLayoutSource = QStringLiteral("systemd-localed");
            Q_EMIT systemLayoutChanged();
        } else {
            readDefaultKeyboardFile();
        }
        markStep();
    });
}

void KeyboardBackend::readDefaultKeyboardFile()
{
    // Debian keeps the console/X11 keyboard default here; systemd-localed
    // reads and writes the same file.
    QFile file(QStringLiteral("/etc/default/keyboard"));
    QString layout, variant;
    if (file.open(QIODevice::ReadOnly)) {
        static const QRegularExpression assignment(QStringLiteral("^\\s*(XKBLAYOUT|XKBVARIANT)\\s*=\\s*\"?([^\"#]*)\"?"));
        const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            const QRegularExpressionMatch match = assignment.match(line);
            if (!match.hasMatch())
                continue;
            if (match.captured(1) == QLatin1String("XKBLAYOUT"))
                layout = match.captured(2).trimmed();
            else
                variant = match.captured(2).trimmed();
        }
    }
    QString text, source;
    if (!layout.isEmpty()) {
        const QStringList layouts = layout.split(QLatin1Char(','));
        const QStringList variants = variant.split(QLatin1Char(','));
        QStringList names;
        for (int i = 0; i < layouts.size(); ++i)
            names.append(layoutName(layouts.at(i).trimmed(), variants.value(i).trimmed()));
        text = names.join(QStringLiteral(", "));
        source = QStringLiteral("/etc/default/keyboard");
    }
    if (text != m_systemLayout || source != m_systemLayoutSource) {
        m_systemLayout = text;
        m_systemLayoutSource = source;
        Q_EMIT systemLayoutChanged();
    }
}

// ---- global shortcuts (KGlobalAccel, read-only) --------------------------------------------

QAbstractItemModel *KeyboardBackend::shortcuts() const
{
    return m_shortcutModel;
}

int KeyboardBackend::shortcutCount() const
{
    return m_shortcutModel->count();
}

void KeyboardBackend::setShortcutFilter(const QString &filter)
{
    if (filter == m_shortcutFilter)
        return;
    m_shortcutFilter = filter;
    Q_EMIT shortcutFilterChanged();
    publishShortcuts();
}

void KeyboardBackend::setShowUnassignedShortcuts(bool show)
{
    if (show == m_showUnassigned)
        return;
    m_showUnassigned = show;
    Q_EMIT shortcutFilterChanged();
    publishShortcuts();
}

void KeyboardBackend::setShortcutsState(const QString &state, const QString &status)
{
    m_shortcutsState = state;
    m_shortcutsStatus = status;
    if (!m_shortcutsLoadedOnce && state != QLatin1String("loading")) {
        m_shortcutsLoadedOnce = true;
        markStep();
    }
}

void KeyboardBackend::publishShortcuts()
{
    const QString needle = m_shortcutFilter.trimmed();
    QVariantList rows;
    for (const QVariant &value : std::as_const(m_allShortcuts)) {
        const QVariantMap row = value.toMap();
        if (!m_showUnassigned && !row.value(QStringLiteral("assigned")).toBool())
            continue;
        if (!needle.isEmpty()
            && !row.value(QStringLiteral("name")).toString().contains(needle, Qt::CaseInsensitive)
            && !row.value(QStringLiteral("componentName")).toString().contains(needle, Qt::CaseInsensitive)
            && !row.value(QStringLiteral("keys")).toString().contains(needle, Qt::CaseInsensitive))
            continue;
        rows.append(row);
    }
    m_shortcutModel->setRows(rows);
    Q_EMIT shortcutsChanged();
}

void KeyboardBackend::finishShortcuts(quint64 generation, const QVariantList &rows)
{
    if (generation != m_shortcutGeneration)
        return;                 // a newer refresh is under way
    QVariantList sorted = rows;
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    collator.setNumericMode(true);
    std::sort(sorted.begin(), sorted.end(), [&collator](const QVariant &a, const QVariant &b) {
        const QVariantMap x = a.toMap(), y = b.toMap();
        const int byComponent = collator.compare(x.value(QStringLiteral("componentName")).toString(),
                                                 y.value(QStringLiteral("componentName")).toString());
        if (byComponent != 0)
            return byComponent < 0;
        return collator.compare(x.value(QStringLiteral("name")).toString(), y.value(QStringLiteral("name")).toString()) < 0;
    });
    int assigned = 0;
    for (const QVariant &row : std::as_const(sorted)) {
        if (row.toMap().value(QStringLiteral("assigned")).toBool())
            ++assigned;
    }
    m_allShortcuts = sorted;
    m_assignedShortcuts = assigned;
    publishShortcuts();
}

void KeyboardBackend::refreshShortcuts()
{
    const quint64 generation = ++m_shortcutGeneration;
    const auto unavailable = [this, generation](const QString &state, const QString &status) {
        if (generation != m_shortcutGeneration)
            return;
        m_allShortcuts.clear();
        m_assignedShortcuts = 0;
        setShortcutsState(state, status);
        publishShortcuts();
    };

    QDBusConnection session = QDBusConnection::sessionBus();
    if (!session.isConnected()) {
        // Answer through the event loop so the first result is always asynchronous.
        QTimer::singleShot(0, this, [unavailable] {
            unavailable(QStringLiteral("no-session-bus"),
                        KeyboardBackend::tr("There is no D-Bus session bus, so the global shortcut service cannot be asked."));
        });
        return;
    }

    dbusNameHasOwner(session, kAccelService, this, [this, session, generation, unavailable](bool owned) {
        if (generation != m_shortcutGeneration)
            return;
        if (!owned) {
            unavailable(QStringLiteral("no-service"),
                        tr("The global shortcut service (KGlobalAccel, org.kde.kglobalaccel) is not running in this "
                           "session. It is part of the KWin Wayland compositor."));
            return;
        }
        dbusCall(session, dbusMethod(kAccelService, kAccelPath, kAccelInterface, QStringLiteral("allComponents")), this,
                 [this, session, generation, unavailable](const QDBusMessage &reply) {
            if (generation != m_shortcutGeneration)
                return;
            if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
                unavailable(QStringLiteral("failed"),
                            tr("The global shortcut service did not list its components: %1").arg(dbusErrorText(reply)));
                return;
            }
            QStringList paths;
            const QVariant plain = dbusPlain(reply.arguments().constFirst());
            const QVariantList entries = plain.toList();
            for (const QVariant &entry : entries) {
                const QString path = entry.toString();
                if (path.startsWith(QLatin1String("/component/")))
                    paths.append(path);
            }
            if (paths.isEmpty()) {
                setShortcutsState(QStringLiteral("ok"), QString());
                finishShortcuts(generation, {});
                return;
            }
            auto pending = std::make_shared<int>(int(paths.size()));
            auto rows = std::make_shared<QVariantList>();
            auto failures = std::make_shared<QStringList>();
            for (const QString &path : std::as_const(paths)) {
                dbusCall(session, dbusMethod(kAccelService, path, kAccelComponentInterface, QStringLiteral("allShortcutInfos")),
                         this, [this, generation, path, pending, rows, failures](const QDBusMessage &infoReply) {
                    if (infoReply.type() == QDBusMessage::ReplyMessage && !infoReply.arguments().isEmpty()) {
                        // a(ssssssaiai): action, action name, component, component name,
                        // context, context name, keys, default keys
                        const QVariantList infos = dbusPlain(infoReply.arguments().constFirst()).toList();
                        for (const QVariant &info : infos) {
                            const QVariantList fields = info.toList();
                            if (fields.size() < 8)
                                continue;
                            const QString action = fields.at(0).toString();
                            const QString component = fields.at(2).toString();
                            if (action.isEmpty() || action == QLatin1String("_k_friendly_name"))
                                continue;       // KGlobalAccel's own bookkeeping entry
                            const QStringList keys = keyTexts(fields.at(6));
                            const QStringList defaults = keyTexts(fields.at(7));
                            QString name = fields.at(1).toString();
                            if (name.isEmpty())
                                name = action;
                            QString componentName = fields.at(3).toString();
                            if (componentName.isEmpty())
                                componentName = component;
                            rows->append(QVariantMap{
                                {QStringLiteral("key"), component + QLatin1Char('\n') + fields.at(4).toString()
                                                            + QLatin1Char('\n') + action},
                                {QStringLiteral("component"), component},
                                {QStringLiteral("componentName"), componentName},
                                {QStringLiteral("action"), action},
                                {QStringLiteral("name"), name},
                                {QStringLiteral("keys"), keys.join(QStringLiteral(", "))},
                                {QStringLiteral("assigned"), !keys.isEmpty()},
                                {QStringLiteral("custom"), keys != defaults},
                            });
                        }
                    } else {
                        failures->append(path.mid(11) + QStringLiteral(": ") + dbusErrorText(infoReply));
                    }
                    if (--*pending > 0 || generation != m_shortcutGeneration)
                        return;
                    if (rows->isEmpty() && !failures->isEmpty()) {
                        m_allShortcuts.clear();
                        m_assignedShortcuts = 0;
                        setShortcutsState(QStringLiteral("failed"),
                                          tr("The global shortcut service did not answer (%1).").arg(failures->join(QStringLiteral("; "))));
                        publishShortcuts();
                        return;
                    }
                    setShortcutsState(QStringLiteral("ok"),
                                      failures->isEmpty() ? QString()
                                                          : tr("Some components did not answer: %1").arg(failures->join(QStringLiteral("; "))));
                    finishShortcuts(generation, *rows);
                });
            }
        });
    });
}

QJsonObject KeyboardBackend::toJson() const
{
    QJsonArray shortcuts;
    for (const QVariant &value : m_allShortcuts) {
        QVariantMap row = value.toMap();
        row.remove(QStringLiteral("key"));
        shortcuts.append(QJsonObject::fromVariantMap(row));
    }
    QJsonArray layouts;
    for (int i = 0; i < m_layoutList.size(); ++i) {
        layouts.append(QJsonObject{
            {QStringLiteral("layout"), m_layoutList.at(i)},
            {QStringLiteral("variant"), m_variantList.value(i)},
            {QStringLiteral("name"), layoutName(m_layoutList.at(i), m_variantList.value(i))},
        });
    }
    return {
        {QStringLiteral("state"), m_state},
        {QStringLiteral("available"), available()},
        {QStringLiteral("statusMessage"), m_statusMessage},
        {QStringLiteral("compositorRunning"), m_compositorRunning},
        {QStringLiteral("catalogFile"), m_catalogFile},
        {QStringLiteral("availableLayoutCount"), int(m_availableLayouts.size())},
        {QStringLiteral("layouts"), layouts},
        {QStringLiteral("activeLayouts"), QJsonArray::fromStringList(m_activeLayouts)},
        {QStringLiteral("activeLayoutIndex"), m_activeLayoutIndex},
        {QStringLiteral("systemLayout"), m_systemLayout},
        {QStringLiteral("systemLayoutSource"), m_systemLayoutSource},
        {QStringLiteral("options"), QJsonArray::fromStringList(m_options)},
        {QStringLiteral("switchOption"), switchOption()},
        {QStringLiteral("composeOption"), composeOption()},
        {QStringLiteral("capsOption"), capsOption()},
        {QStringLiteral("switchOptionCount"), int(m_switchOptions.size()) - 1},
        {QStringLiteral("repeatDelay"), m_repeatDelay},
        {QStringLiteral("repeatRate"), m_repeatRate},
        {QStringLiteral("repeatConfigured"), m_repeatConfigured},
        {QStringLiteral("shortcutsState"), m_shortcutsState},
        {QStringLiteral("shortcutsStatus"), m_shortcutsStatus},
        {QStringLiteral("assignedShortcutCount"), m_assignedShortcuts},
        {QStringLiteral("shortcuts"), shortcuts},
    };
}
