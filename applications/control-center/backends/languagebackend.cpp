// SPDX-License-Identifier: GPL-3.0-or-later
#include "languagebackend.h"

#include "dumpregistry.h"

#include <command.h>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDate>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QRegularExpression>
#include <QTime>
#include <QTimer>

SHADOW_CC_DUMP("language", [] {
    LanguageBackend backend;
    if (!backend.ready()) {
        QEventLoop loop;
        QObject::connect(&backend, &LanguageBackend::stateChanged, &loop, [&] { if (backend.ready()) loop.quit(); });
        QTimer::singleShot(8000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return QJsonValue(backend.toJson());
})

namespace {
const QString kService = QStringLiteral("org.freedesktop.locale1");
const QString kPath = QStringLiteral("/org/freedesktop/locale1");
const QString kInterface = QStringLiteral("org.freedesktop.locale1");
const QString kProperties = QStringLiteral("org.freedesktop.DBus.Properties");
const QString kHelper = QStringLiteral("/usr/libexec/shadow/language-helper");
const QString kSupported = QStringLiteral("/usr/share/i18n/SUPPORTED");
const QString kReadOnlyPolicy = QStringLiteral("/usr/share/dbus-1/system.d/systemd-localed-read-only.conf");
const QString kLang = QStringLiteral("LANG");
const int kChangeTimeoutMs = 5 * 60 * 1000;

const QStringList &formatCategories()
{
    static const QStringList list = {QStringLiteral("LC_TIME"), QStringLiteral("LC_NUMERIC"),
                                     QStringLiteral("LC_MONETARY"), QStringLiteral("LC_MEASUREMENT")};
    return list;
}

// "de_DE.UTF-8" and "de_DE.utf8" are the same locale.
QString normalised(const QString &locale)
{
    QString n = locale.toLower();
    n.replace(QStringLiteral("utf-8"), QStringLiteral("utf8"));
    return n;
}

// Same pattern as the helper accepts.
bool validName(const QString &locale)
{
    static const QRegularExpression re(QStringLiteral("^(C\\.UTF-8|[A-Za-z]{2,3}(_[A-Z]{2})?(\\.UTF-8)?(@[a-z0-9]{1,16})?)$"));
    return re.match(locale).hasMatch();
}

QLocale qtLocale(const QString &locale, bool *matched = nullptr)
{
    const QString bare = locale.section(QLatin1Char('@'), 0, 0).section(QLatin1Char('.'), 0, 0);
    const QLocale l(bare);
    if (matched)
        *matched = l.language() != QLocale::C
                   && l.name().section(QLatin1Char('_'), 0, 0) == bare.section(QLatin1Char('_'), 0, 0);
    return l;
}

QString errorText(const QDBusError &error)
{
    const QString name = error.name();
    if (name == QLatin1String("org.freedesktop.DBus.Error.AccessDenied")
        || name == QLatin1String("org.freedesktop.DBus.Error.InteractiveAuthorizationRequired")
        || name.endsWith(QLatin1String("NotAuthorized")))
        return LanguageBackend::tr("The change was not authorised.");
    if (name == QLatin1String("org.freedesktop.DBus.Error.ServiceUnknown")
        || name == QLatin1String("org.freedesktop.DBus.Error.NameHasNoOwner")
        || name.startsWith(QLatin1String("org.freedesktop.DBus.Error.Spawn.")))
        return LanguageBackend::tr("The locale service (systemd-localed) is not running and could not be started.");
    if (name == QLatin1String("org.freedesktop.DBus.Error.NoReply") || name == QLatin1String("org.freedesktop.DBus.Error.Timeout"))
        return LanguageBackend::tr("The locale service did not answer.");
    return error.message().isEmpty() ? name : error.message();
}

// KEY=value lines of a shell-style environment file.
QStringList readAssignments(const QString &path)
{
    QStringList out;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return out;
    const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0 || line.startsWith(QLatin1Char('#')))
            continue;
        QString value = line.mid(eq + 1).trimmed();
        if (value.size() >= 2 && (value.startsWith(QLatin1Char('"')) || value.startsWith(QLatin1Char('\''))))
            value = value.mid(1, value.size() - 2);
        out << line.left(eq).trimmed() + QLatin1Char('=') + value;
    }
    return out;
}
} // namespace

LanguageBackend::LanguageBackend(QObject *parent)
    : QObject(parent)
{
    loadLocales();
    m_localedReadOnly = QFileInfo::exists(kReadOnlyPolicy);
    QDBusConnection bus = QDBusConnection::systemBus();
    if (bus.isConnected()) {
        bus.connect(kService, kPath, kProperties, QStringLiteral("PropertiesChanged"), this,
                    SLOT(onPropertiesChanged(QString, QVariantMap, QStringList)));
    }
    refresh();
}

QString LanguageBackend::sessionLanguage() const
{
    for (const char *name : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const QString value = qEnvironmentVariable(name);
        if (!value.isEmpty())
            return value;
    }
    return QString();
}

bool LanguageBackend::helperAvailable() const
{
    return QFileInfo(kHelper).isExecutable() && Shadow::Command::available(QStringLiteral("pkexec"));
}

bool LanguageBackend::loginPending() const
{
    const QString session = qEnvironmentVariable("LANG");
    const QString system = systemLanguage();
    return m_ready && !system.isEmpty() && !session.isEmpty() && normalised(session) != normalised(system);
}

// ---- locale list ---------------------------------------------------------------------------

void LanguageBackend::loadLocales()
{
    QStringList ids;
    QFile supported(kSupported);
    m_supportedRead = supported.open(QIODevice::ReadOnly);
    if (m_supportedRead) {
        while (!supported.atEnd()) {
            const QStringList parts = QString::fromLatin1(supported.readLine()).simplified().split(QLatin1Char(' '));
            if (parts.size() == 2 && parts.at(1) == QLatin1String("UTF-8") && validName(parts.at(0)))
                ids << parts.at(0);
        }
    }

    // Generated locales. The command returns at once (it lists an archive).
    m_generated.clear();
    const Shadow::CommandResult result = Shadow::Command::runSync(QStringLiteral("locale"), {QStringLiteral("-a")}, 4000);
    QStringList generatedNames;
    if (result.ok()) {
        const QStringList lines = result.outText().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            m_generated.insert(normalised(line.trimmed()));
            generatedNames << line.trimmed();
        }
    }
    m_generated.insert(QStringLiteral("c.utf8"));       // built into glibc
    QSet<QString> have;
    for (const QString &id : std::as_const(ids))
        have.insert(normalised(id));
    // Generated but not in SUPPORTED (or SUPPORTED is missing): still usable.
    for (const QString &name : std::as_const(generatedNames)) {
        if (!name.contains(QLatin1String("utf8"), Qt::CaseInsensitive) || have.contains(normalised(name)))
            continue;
        QString id = name;
        id.replace(QStringLiteral(".utf8"), QStringLiteral(".UTF-8"));
        if (validName(id)) {
            ids << id;
            have.insert(normalised(id));
        }
    }
    if (!have.contains(QStringLiteral("c.utf8")))
        ids << QStringLiteral("C.UTF-8");

    QVariantList list;
    for (const QString &id : std::as_const(ids)) {
        bool matched = false;
        const QLocale l = qtLocale(id, &matched);
        QString english = id;
        if (matched) {
            english = QLocale::languageToString(l.language());
            if (id.contains(QLatin1Char('_')))
                english += QStringLiteral(" (") + QLocale::territoryToString(l.territory()) + QLatin1Char(')');
        } else if (id == QLatin1String("C.UTF-8")) {
            english = tr("No localisation (C, English messages)");
        }
        list << QVariantMap{
            {QStringLiteral("id"), id},
            {QStringLiteral("name"), displayName(id)},
            {QStringLiteral("englishName"), english},
            {QStringLiteral("generated"), isGenerated(id)},
        };
    }
    std::sort(list.begin(), list.end(), [](const QVariant &a, const QVariant &b) {
        return QString::localeAwareCompare(a.toMap().value(QStringLiteral("englishName")).toString(),
                                           b.toMap().value(QStringLiteral("englishName")).toString()) < 0;
    });
    m_known = ids;
    m_locales = list;
    Q_EMIT localesChanged();
}

QString LanguageBackend::displayName(const QString &locale) const
{
    if (locale.isEmpty())
        return QString();
    if (locale == QLatin1String("C.UTF-8") || locale == QLatin1String("C") || locale == QLatin1String("POSIX"))
        return tr("No localisation (C)");
    bool matched = false;
    const QLocale l = qtLocale(locale, &matched);
    if (!matched || l.nativeLanguageName().isEmpty())
        return locale;
    QString name = l.nativeLanguageName();
    if (!name.isEmpty())
        name[0] = name.at(0).toUpper();
    if (locale.contains(QLatin1Char('_')) && !l.nativeTerritoryName().isEmpty())
        name += QStringLiteral(" (") + l.nativeTerritoryName() + QLatin1Char(')');
    const QString modifier = locale.section(QLatin1Char('@'), 1, 1);
    if (!modifier.isEmpty())
        name += QStringLiteral(" [") + modifier + QLatin1Char(']');
    return name;
}

bool LanguageBackend::isGenerated(const QString &locale) const
{
    const QString n = normalised(locale);
    if (m_generated.contains(n))
        return true;
    // "sr_RS@latin" (UTF-8 by default) is listed by locale -a without a codeset.
    return m_generated.contains(QString(n).remove(QStringLiteral(".utf8")));
}

// ---- system state ------------------------------------------------------------------------------

void LanguageBackend::refresh()
{
    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        readWithoutService(tr("The system message bus is not reachable, so the locale service (systemd-localed) cannot be asked."));
        return;
    }
    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kProperties, QStringLiteral("Get"));
    message << kInterface << QStringLiteral("Locale");
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message, 25000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<QDBusVariant> reply = *w;
        if (reply.isError()) {
            readWithoutService(errorText(reply.error()));
            return;
        }
        m_available = true;
        m_statusMessage.clear();
        m_source = tr("systemd-localed");
        setSystem(reply.value().variant().toStringList());
    });
}

void LanguageBackend::readWithoutService(const QString &reason)
{
    m_available = false;
    m_statusMessage = reason;
    QStringList assignments;
    m_source.clear();
    for (const QString &path : {QStringLiteral("/etc/locale.conf"), QStringLiteral("/etc/default/locale")}) {
        if (QFileInfo::exists(path)) {
            assignments = readAssignments(path);
            m_source = path;
            break;
        }
    }
    setSystem(assignments);
}

void LanguageBackend::setSystem(const QStringList &assignments)
{
    QMap<QString, QString> system;
    for (const QString &assignment : assignments) {
        const int eq = assignment.indexOf(QLatin1Char('='));
        if (eq > 0)
            system.insert(assignment.left(eq), assignment.mid(eq + 1));
    }
    m_system = system;
    m_ready = true;
    if (!m_selectionTouched) {
        m_selected.clear();
        m_selected.insert(kLang, m_system.value(kLang));
        for (const QString &category : formatCategories()) {
            const QString value = m_system.value(category);
            m_selected.insert(category, value == m_system.value(kLang) ? QString() : value);
        }
    }
    Q_EMIT stateChanged();
    Q_EMIT selectionChanged();
}

void LanguageBackend::onPropertiesChanged(const QString &interface, const QVariantMap &changed, const QStringList &)
{
    if (interface != kInterface)
        return;
    if (changed.contains(QStringLiteral("Locale")) && m_available)
        setSystem(changed.value(QStringLiteral("Locale")).toStringList());
    else
        refresh();
}

// ---- selection -----------------------------------------------------------------------------------

QString LanguageBackend::example(const QString &category, const QString &locale) const
{
    bool matched = false;
    QLocale l = qtLocale(locale, &matched);
    if (!matched)
        l = QLocale::c();
    if (category == QLatin1String("LC_TIME"))
        return l.toString(QDate::currentDate(), QLocale::LongFormat) + QStringLiteral("  ·  ")
               + l.toString(QTime::currentTime(), QLocale::ShortFormat);
    if (category == QLatin1String("LC_NUMERIC"))
        return l.toString(1234567.891, 'f', 2);
    if (category == QLatin1String("LC_MONETARY"))
        return matched ? l.toCurrencyString(1234.56) : l.toString(1234.56, 'f', 2);
    if (category == QLatin1String("LC_MEASUREMENT")) {
        switch (l.measurementSystem()) {
        case QLocale::ImperialUSSystem: return tr("US units (miles, pounds, °F)");
        case QLocale::ImperialUKSystem: return tr("Imperial units (miles, pints)");
        default: return tr("Metric (kilometres, kilograms, °C)");
        }
    }
    return QString();
}

QVariantList LanguageBackend::formats() const
{
    static const QMap<QString, const char *> titles = {
        {QStringLiteral("LC_TIME"), QT_TR_NOOP("Dates and times")},
        {QStringLiteral("LC_NUMERIC"), QT_TR_NOOP("Numbers")},
        {QStringLiteral("LC_MONETARY"), QT_TR_NOOP("Currency")},
        {QStringLiteral("LC_MEASUREMENT"), QT_TR_NOOP("Measurement units")},
    };
    QVariantList out;
    for (const QString &category : formatCategories()) {
        const QString value = m_selected.value(category);
        const QString effective = value.isEmpty() ? m_selected.value(kLang) : value;
        const QString systemValue = m_system.value(category);
        out << QVariantMap{
            {QStringLiteral("category"), category},
            {QStringLiteral("title"), tr(titles.value(category))},
            {QStringLiteral("value"), value},
            {QStringLiteral("effective"), effective},
            {QStringLiteral("effectiveName"), displayName(effective)},
            {QStringLiteral("example"), effective.isEmpty() ? QString() : example(category, effective)},
            {QStringLiteral("generated"), value.isEmpty() || isGenerated(value)},
            {QStringLiteral("system"), systemValue.isEmpty() ? m_system.value(kLang) : systemValue},
        };
    }
    return out;
}

bool LanguageBackend::dirty() const
{
    if (!m_ready)
        return false;
    if (m_selected.value(kLang) != m_system.value(kLang))
        return true;
    for (const QString &category : formatCategories()) {
        const QString selected = m_selected.value(category).isEmpty() ? m_selected.value(kLang) : m_selected.value(category);
        const QString system = m_system.value(category).isEmpty() ? m_system.value(kLang) : m_system.value(category);
        if (selected != system)
            return true;
    }
    return false;
}

QStringList LanguageBackend::toGenerate() const
{
    QStringList out;
    for (const QString &value : m_selected) {
        if (!value.isEmpty() && !out.contains(value) && !isGenerated(value))
            out << value;
    }
    return out;
}

void LanguageBackend::selectLanguage(const QString &locale)
{
    if (!m_known.contains(locale) || locale == m_selected.value(kLang))
        return;
    m_selected.insert(kLang, locale);
    for (const QString &category : formatCategories()) {
        if (m_selected.value(category) == locale)
            m_selected.insert(category, QString());
    }
    m_selectionTouched = true;
    Q_EMIT selectionChanged();
}

void LanguageBackend::selectFormat(const QString &category, const QString &locale)
{
    if (!formatCategories().contains(category) || (!locale.isEmpty() && !m_known.contains(locale)))
        return;
    m_selected.insert(category, locale == m_selected.value(kLang) ? QString() : locale);
    m_selectionTouched = true;
    Q_EMIT selectionChanged();
}

void LanguageBackend::revert()
{
    m_selectionTouched = false;
    QStringList assignments;
    for (auto it = m_system.constBegin(); it != m_system.constEnd(); ++it)
        assignments << it.key() + QLatin1Char('=') + it.value();
    setSystem(assignments);
}

void LanguageBackend::setBusy(bool busy, const QString &text)
{
    m_busy = busy;
    m_busyText = text;
    Q_EMIT busyChanged();
}

void LanguageBackend::apply()
{
    if (m_busy || !dirty())
        return;
    if (!m_available) {
        Q_EMIT failed(m_statusMessage);
        return;
    }
    if (m_selected.value(kLang).isEmpty()) {
        Q_EMIT failed(tr("Choose a language first."));
        return;
    }
    const QStringList missing = toGenerate();
    for (const QString &name : missing) {
        if (!validName(name) || name == QLatin1String("C.UTF-8")) {
            Q_EMIT failed(tr("“%1” cannot be generated on this system.").arg(name));
            return;
        }
    }
    if ((!missing.isEmpty() || m_localedReadOnly) && !helperAvailable()) {
        Q_EMIT failed(missing.isEmpty()
                          ? tr("On this system the locale is written by the helper %1 through pkexec, which is not installed.").arg(kHelper)
                          : tr("%1 must be generated first, but the helper %2 or pkexec is not installed.")
                                .arg(missing.join(QStringLiteral(", ")), kHelper));
        return;
    }
    if (m_localedReadOnly)
        runHelper(missing, assignments());      // one authentication for both steps
    else if (!missing.isEmpty())
        runHelper(missing, {});                 // then SetLocale
    else
        sendSetLocale();
}

// The complete set to store. SetLocale and "update-locale --reset" both
// replace everything: keep what this page does not manage (LC_PAPER,
// LC_ADDRESS, …). LANGUAGE would override the message language chosen here,
// so it does not survive a language change.
QStringList LanguageBackend::assignments() const
{
    QMap<QString, QString> next = m_system;
    const QString lang = m_selected.value(kLang);
    if (next.value(kLang) != lang)
        next.remove(QStringLiteral("LANGUAGE"));
    next.insert(kLang, lang);
    for (const QString &category : formatCategories()) {
        const QString value = m_selected.value(category);
        if (value.isEmpty() || value == lang)
            next.remove(category);
        else
            next.insert(category, value);
    }
    QStringList out;
    for (auto it = next.constBegin(); it != next.constEnd(); ++it) {
        if (!it.value().isEmpty())
            out << it.key() + QLatin1Char('=') + it.value();
    }
    return out;
}

// pkexec language-helper [generate <locale>...] [set <assignment>...]
void LanguageBackend::runHelper(const QStringList &generate, const QStringList &set)
{
    QStringList arguments{kHelper};
    if (!generate.isEmpty())
        arguments << QStringLiteral("generate") << generate;
    if (!set.isEmpty())
        arguments << QStringLiteral("set") << set;
    setBusy(true, generate.isEmpty() ? tr("Applying…")
                                     : tr("Generating %1 — this can take a minute…").arg(generate.join(QStringLiteral(", "))));
    Shadow::Command::run(this, QStringLiteral("pkexec"), arguments, [this, generate, set](const Shadow::CommandResult &result) {
        const QJsonObject reply = QJsonDocument::fromJson(result.out).object();
        if (!generate.isEmpty()) {
            loadLocales();
            Q_EMIT selectionChanged();
        }
        if (!result.ok() || !reply.value(QStringLiteral("ok")).toBool()) {
            setBusy(false);
            QString reason = reply.value(QStringLiteral("error")).toString();
            if (result.exitCode == 126 || result.exitCode == 127)
                reason = tr("The change was not authorised.");       // pkexec: dismissed / not authorised
            else if (result.exitCode == -2)
                reason = tr("The helper did not finish in time.");
            else if (reason.isEmpty())
                reason = result.errText().trimmed().isEmpty() ? tr("The helper failed (exit status %1).").arg(result.exitCode)
                                                              : result.errText().trimmed();
            const QString detail = reply.value(QStringLiteral("output")).toString().trimmed();
            if (!detail.isEmpty())
                reason += QStringLiteral(" — ") + detail.section(QLatin1Char('\n'), -1);
            Q_EMIT failed(tr("The language settings could not be changed: %1").arg(reason));
            refresh();
            return;
        }
        if (set.isEmpty()) {
            sendSetLocale();
            return;
        }
        setBusy(false);
        m_selectionTouched = false;
        Q_EMIT applied(tr("Saved. The new language and formats are used from your next login."));
        refresh();
    }, 16 * 60 * 1000);
}

void LanguageBackend::sendSetLocale()
{
    setBusy(true, tr("Applying…"));
    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kInterface, QStringLiteral("SetLocale"));
    message << assignments() << true;
    message.setInteractiveAuthorizationAllowed(true);
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message, kChangeTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<> reply = *w;
        // The bus itself refusing to deliver the call ("Rejected send
        // message, … matched rules") is the distribution's policy, not a
        // missing authorisation: the helper writes the locale instead.
        if (reply.isError() && reply.error().name() == QLatin1String("org.freedesktop.DBus.Error.AccessDenied")
            && reply.error().message().contains(QLatin1String("matched rules"))) {
            m_localedReadOnly = true;
            Q_EMIT stateChanged();
            if (helperAvailable()) {
                runHelper({}, assignments());
            } else {
                setBusy(false);
                Q_EMIT failed(tr("This system does not allow systemd-localed to change the locale, and the helper %1 "
                                 "that writes it instead is not installed.").arg(kHelper));
            }
            return;
        }
        setBusy(false);
        if (reply.isError()) {
            Q_EMIT failed(errorText(reply.error()));
        } else {
            m_selectionTouched = false;
            Q_EMIT applied(tr("Saved. The new language and formats are used from your next login."));
        }
        refresh();
    });
}

QJsonObject LanguageBackend::toJson() const
{
    QJsonObject system;
    for (auto it = m_system.constBegin(); it != m_system.constEnd(); ++it)
        system.insert(it.key(), it.value());
    QJsonArray generated, offered;
    for (const QVariant &entry : m_locales) {
        const QVariantMap map = entry.toMap();
        offered << map.value(QStringLiteral("id")).toString();
        if (map.value(QStringLiteral("generated")).toBool())
            generated << map.value(QStringLiteral("id")).toString();
    }
    return {
        {QStringLiteral("serviceAvailable"), m_available},
        {QStringLiteral("statusMessage"), m_statusMessage},
        {QStringLiteral("source"), m_source},
        {QStringLiteral("system"), system},
        {QStringLiteral("sessionLanguage"), sessionLanguage()},
        {QStringLiteral("supportedListRead"), m_supportedRead},
        {QStringLiteral("offeredCount"), offered.size()},
        {QStringLiteral("generated"), generated},
        {QStringLiteral("helperAvailable"), helperAvailable()},
        {QStringLiteral("usesHelperToApply"), m_localedReadOnly},
    };
}
