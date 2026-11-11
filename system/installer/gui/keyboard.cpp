// SPDX-License-Identifier: GPL-3.0-or-later
#include "keyboard.h"

#include "locales.h"
#include "options.h"

#include <QCollator>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QFile>
#include <QJsonArray>
#include <QRegularExpression>
#include <QTimer>

#include <algorithm>

#ifdef NEXUS_HAVE_KCONFIG
#include <KConfig>
#include <KConfigGroup>
#endif

namespace {
const QString kKeyboardService = QStringLiteral("org.kde.keyboard");
const QString kLayoutsPath = QStringLiteral("/Layouts");
const QString kLayoutsInterface = QStringLiteral("org.kde.KeyboardLayouts");

// How long KWin gets to switch before an attempt counts as failed.
constexpr int kApplyPollMs = 300;
constexpr int kApplyAttempts = 10;
} // namespace

Keyboard::Keyboard(QObject *parent)
    : QObject(parent)
{
    loadCatalog();

    QDBusConnection session = QDBusConnection::sessionBus();
    if (session.isConnected()) {
        auto *watcher = new QDBusServiceWatcher(kKeyboardService, session,
                                                QDBusServiceWatcher::WatchForOwnerChange, this);
        connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, &Keyboard::querySession);
        session.connect(kKeyboardService, kLayoutsPath, kLayoutsInterface, QStringLiteral("layoutListChanged"),
                        this, SLOT(querySession()));
        session.connect(kKeyboardService, kLayoutsPath, kLayoutsInterface, QStringLiteral("layoutChanged"),
                        this, SLOT(querySession()));
        querySession();
    }
}

void Keyboard::loadCatalog()
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
    if (!file.isOpen()) {
        m_problem = tr("The keyboard layout catalogue %1 is missing (package “xkb-data”). The installed system "
                       "will use the layout “us”; it can be changed afterwards in the Control Center.")
                        .arg(candidates.constFirst());
        return;
    }

    // answers.py: XKB_RE — names the engine accepts for layout and variant.
    static const QRegularExpression engineName(QStringLiteral("\\A[A-Za-z0-9_-]{1,32}\\z"));
    static const QRegularExpression entry(QStringLiteral("^\\s+(\\S+)\\s+(.*)$"));
    static const QRegularExpression variantEntry(QStringLiteral("^(\\S+):\\s*(.*)$"));

    QString section;
    while (!file.atEnd()) {
        QString line = QString::fromUtf8(file.readLine());
        while (line.endsWith(QLatin1Char('\n')) || line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        if (line.startsWith(QLatin1Char('!'))) {
            section = line.mid(1).trimmed();
            continue;
        }
        const QRegularExpressionMatch m = entry.match(line);
        if (!m.hasMatch())
            continue;
        const QString name = m.captured(1);
        const QString description = m.captured(2).trimmed();
        if (!engineName.match(name).hasMatch())
            continue;
        if (section == QLatin1String("layout")) {
            m_layoutNames.insert(name, description);
        } else if (section == QLatin1String("variant")) {
            const QRegularExpressionMatch v = variantEntry.match(description);
            if (v.hasMatch())
                m_variants[v.captured(1)].append({name, v.captured(2).trimmed()});
        }
    }

    for (auto it = m_layoutNames.constBegin(); it != m_layoutNames.constEnd(); ++it) {
        QString search = it.value() + QLatin1Char(' ') + it.key();
        const auto variants = m_variants.value(it.key());
        for (const auto &variant : variants)
            search += QLatin1Char(' ') + variant.second;
        m_layouts.append(QVariantMap{
            {QStringLiteral("code"), it.key()},
            {QStringLiteral("name"), it.value()},
            {QStringLiteral("search"), search.toCaseFolded()},
        });
    }
    QCollator collator(QLocale(QLocale::English));
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(m_layouts.begin(), m_layouts.end(), [&collator](const QVariant &a, const QVariant &b) {
        return collator.compare(a.toMap().value(QStringLiteral("name")).toString(),
                                b.toMap().value(QStringLiteral("name")).toString()) < 0;
    });
    if (m_layouts.isEmpty())
        m_problem = tr("%1 contains no keyboard layouts.").arg(m_catalogFile);
}

QVariantList Keyboard::filteredLayouts(const QString &needle) const
{
    const QString folded = needle.trimmed().toCaseFolded();
    if (folded.isEmpty())
        return m_layouts;
    QVariantList out;
    for (const QVariant &item : m_layouts) {
        if (item.toMap().value(QStringLiteral("search")).toString().contains(folded))
            out.append(item);
    }
    return out;
}

QVariantList Keyboard::variantsFor(const QString &layout) const
{
    QVariantList out;
    if (!m_layoutNames.contains(layout))
        return out;
    out.append(QVariantMap{{QStringLiteral("code"), QString()},
                           {QStringLiteral("name"), tr("%1 — default").arg(m_layoutNames.value(layout))}});
    const auto variants = m_variants.value(layout);
    for (const auto &variant : variants)
        out.append(QVariantMap{{QStringLiteral("code"), variant.first}, {QStringLiteral("name"), variant.second}});
    return out;
}

bool Keyboard::hasVariant(const QString &layout, const QString &variant) const
{
    if (variant.isEmpty())
        return m_layoutNames.contains(layout);
    const auto variants = m_variants.value(layout);
    return std::any_of(variants.cbegin(), variants.cend(), [&variant](const auto &v) { return v.first == variant; });
}

QString Keyboard::describe(const QString &layout, const QString &variant) const
{
    if (!variant.isEmpty()) {
        const auto variants = m_variants.value(layout);
        for (const auto &v : variants) {
            if (v.first == variant)
                return v.second;
        }
        return QStringLiteral("%1 (%2)").arg(m_layoutNames.value(layout, layout), variant);
    }
    return m_layoutNames.value(layout, layout);
}

QVariantMap Keyboard::suggestionForLocale(const QString &localeCode) const
{
    const QString base = localeCode.section(QLatin1Char('@'), 0, 0).section(QLatin1Char('.'), 0, 0);
    const QString language = base.section(QLatin1Char('_'), 0, 0);
    const QString territory = Locales::territoryOf(localeCode).toLower();

    QString layout, variant;
    // XKB names most layouts after the country they are used in.
    if (!territory.isEmpty() && m_layoutNames.contains(territory)) {
        layout = territory;
        // Countries with several languages keep them as variants (ch: fr, ca/in: eng).
        if (hasVariant(layout, language) && !language.isEmpty())
            variant = language;
        else if (language == QLatin1String("en") && hasVariant(layout, QStringLiteral("eng"))
                 && !m_layoutNames.value(layout).startsWith(QLatin1String("English")))
            variant = QStringLiteral("eng");
    } else if (language == QLatin1String("es") && m_layoutNames.contains(QStringLiteral("latam"))) {
        layout = QStringLiteral("latam");
    } else if (language == QLatin1String("ar") && m_layoutNames.contains(QStringLiteral("ara"))) {
        layout = QStringLiteral("ara");
    }
    if (layout.isEmpty())
        return {};
    return {{QStringLiteral("layout"), layout}, {QStringLiteral("variant"), variant}};
}

// ---- running session --------------------------------------------------------------------

QString Keyboard::sessionLayoutCode() const
{
    return m_sessionCodes.value(m_sessionIndex);
}

QString Keyboard::sessionLayoutName() const
{
    const QString name = m_sessionNames.value(m_sessionIndex);
    return name.isEmpty() ? sessionLayoutCode() : name;
}

bool Keyboard::canApply() const
{
    return cannotApplyReason().isEmpty();
}

QString Keyboard::cannotApplyReason() const
{
    if (!Options::instance().liveSession)
        return tr("The installer changes the keyboard layout of the running session only in a NEXUS OS live session.");
#ifndef NEXUS_HAVE_KCONFIG
    return tr("This build of the installer cannot write the session's keyboard configuration (built without KConfig).");
#else
    if (!sessionKnown())
        return tr("The window manager (KWin) does not report its keyboard layout on the session bus, so the "
                  "layout of this session can neither be read nor changed from here.");
    return QString();
#endif
}

bool Keyboard::sessionMatches(const QString &layout, const QString &variant) const
{
    if (!sessionKnown() || sessionLayoutCode() != layout)
        return false;
    // KWin reports the layout's short name and the name of the active keymap
    // group, e.g. ("de", "German (no dead keys)"). The group name is compared
    // with the catalogue's descriptions to tell the variants apart.
    const QString name = m_sessionNames.value(m_sessionIndex);
    const auto variants = m_variants.value(layout);
    if (variant.isEmpty())
        return std::none_of(variants.cbegin(), variants.cend(), [&name](const auto &v) { return v.second == name; });
    if (name == describe(layout, variant))
        return true;
    // A name that is neither the default's nor another variant's: the requested
    // variant under a label that differs between xkb's rules and symbols files.
    return name != m_layoutNames.value(layout)
        && std::none_of(variants.cbegin(), variants.cend(), [&name](const auto &v) { return v.second == name; });
}

void Keyboard::querySession()
{
    QDBusConnection session = QDBusConnection::sessionBus();
    if (!session.isConnected())
        return;
    const QDBusMessage listCall = QDBusMessage::createMethodCall(kKeyboardService, kLayoutsPath, kLayoutsInterface,
                                                                 QStringLiteral("getLayoutsList"));
    auto *watcher = new QDBusPendingCallWatcher(session.asyncCall(listCall, 3000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, session](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        QStringList codes, names;
        const QDBusMessage reply = w->reply();
        if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()
            && reply.arguments().constFirst().canConvert<QDBusArgument>()) {
            // a(sss): short name, display name, long name
            const QDBusArgument arg = reply.arguments().constFirst().value<QDBusArgument>();
            arg.beginArray();
            while (!arg.atEnd()) {
                QString shortName, displayName, longName;
                arg.beginStructure();
                arg >> shortName >> displayName >> longName;
                arg.endStructure();
                codes.append(shortName);
                names.append(longName);
            }
            arg.endArray();
        }
        if (codes.isEmpty()) {
            if (!m_sessionCodes.isEmpty()) {
                m_sessionCodes.clear();
                m_sessionNames.clear();
                m_sessionIndex = 0;
                Q_EMIT sessionChanged();
            }
            return;
        }
        const QDBusMessage indexCall = QDBusMessage::createMethodCall(kKeyboardService, kLayoutsPath,
                                                                      kLayoutsInterface, QStringLiteral("getLayout"));
        auto *indexWatcher = new QDBusPendingCallWatcher(session.asyncCall(indexCall, 3000), this);
        connect(indexWatcher, &QDBusPendingCallWatcher::finished, this,
                [this, codes, names](QDBusPendingCallWatcher *iw) {
            iw->deleteLater();
            int index = 0;
            const QDBusMessage indexReply = iw->reply();
            if (indexReply.type() == QDBusMessage::ReplyMessage && !indexReply.arguments().isEmpty())
                index = indexReply.arguments().constFirst().toInt();
            if (index < 0 || index >= codes.size())
                index = 0;
            if (codes != m_sessionCodes || names != m_sessionNames || index != m_sessionIndex) {
                m_sessionCodes = codes;
                m_sessionNames = names;
                m_sessionIndex = index;
                Q_EMIT sessionChanged();
            }
        });
    });
}

void Keyboard::setApplyState(const QString &state, const QString &message)
{
    if (state == m_applyState && message == m_applyMessage)
        return;
    m_applyState = state;
    m_applyMessage = message;
    Q_EMIT applyStateChanged();
}

bool Keyboard::applyToSession(const QString &layout, const QString &variant)
{
    ++m_applySerial;
    if (!canApply()) {
        setApplyState(QStringLiteral("failed"), cannotApplyReason());
        return false;
    }
    if (!hasVariant(layout, variant)) {
        setApplyState(QStringLiteral("failed"), tr("“%1” is not a known keyboard layout.").arg(describe(layout, variant)));
        return false;
    }
    if (sessionMatches(layout, variant) && m_sessionCodes.size() == 1) {
        setApplyState(QStringLiteral("applied"), tr("This session already types with %1.").arg(sessionLayoutName()));
        return true;
    }

#ifdef NEXUS_HAVE_KCONFIG
    {
        KConfig xkb(QStringLiteral("kxkbrc"), KConfig::NoGlobals);
        KConfigGroup group = xkb.group(QStringLiteral("Layout"));
        group.writeEntry("Use", true);
        group.writeEntry("LayoutList", layout);
        group.writeEntry("VariantList", variant);
        if (!xkb.sync()) {
            setApplyState(QStringLiteral("failed"),
                          tr("The keyboard configuration of this session (kxkbrc) could not be written."));
            return false;
        }
    }
    // KWin re-reads kxkbrc when it receives this signal.
    QDBusConnection::sessionBus().send(
        QDBusMessage::createSignal(kLayoutsPath, kKeyboardService, QStringLiteral("reloadConfig")));

    m_pendingLayout = layout;
    m_pendingVariant = variant;
    setApplyState(QStringLiteral("applying"), tr("Switching this session to %1…").arg(describe(layout, variant)));
    checkApplied(1);
    return true;
#else
    return false;
#endif
}

// Ask KWin until it reports the requested layout or the attempts run out.
void Keyboard::checkApplied(int attempt)
{
    const quint64 serial = m_applySerial;
    QTimer::singleShot(kApplyPollMs, this, [this, serial, attempt] {
        if (serial != m_applySerial || m_applyState != QLatin1String("applying"))
            return;
        if (sessionMatches(m_pendingLayout, m_pendingVariant)) {
            setApplyState(QStringLiteral("applied"),
                          tr("This session now types with %1 (confirmed by the window manager).").arg(sessionLayoutName()));
            return;
        }
        if (attempt >= kApplyAttempts) {
            setApplyState(QStringLiteral("failed"),
                          tr("The window manager did not switch: it still reports %1. Typing in this session "
                             "uses that layout.").arg(sessionKnown() ? sessionLayoutName() : tr("no layout")));
            return;
        }
        querySession();
        checkApplied(attempt + 1);
    });
}

QJsonObject Keyboard::toJson() const
{
    QJsonArray layouts;
    for (const QVariant &item : m_layouts) {
        const QVariantMap map = item.toMap();
        const QString code = map.value(QStringLiteral("code")).toString();
        QJsonArray variants;
        const auto list = m_variants.value(code);
        for (const auto &variant : list)
            variants.append(QJsonObject{{QStringLiteral("code"), variant.first}, {QStringLiteral("name"), variant.second}});
        layouts.append(QJsonObject{
            {QStringLiteral("code"), code},
            {QStringLiteral("name"), map.value(QStringLiteral("name")).toString()},
            {QStringLiteral("variants"), variants},
        });
    }
    return {
        {QStringLiteral("catalog"), m_catalogFile},
        {QStringLiteral("problem"), m_problem},
        {QStringLiteral("layouts"), layouts},
        {QStringLiteral("sessionKnown"), sessionKnown()},
        {QStringLiteral("sessionLayouts"), QJsonArray::fromStringList(m_sessionCodes)},
        {QStringLiteral("sessionLayoutNames"), QJsonArray::fromStringList(m_sessionNames)},
        {QStringLiteral("sessionLayoutIndex"), m_sessionIndex},
        {QStringLiteral("canApply"), canApply()},
        {QStringLiteral("cannotApplyReason"), cannotApplyReason()},
    };
}
