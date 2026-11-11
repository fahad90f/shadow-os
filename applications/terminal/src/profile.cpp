// SPDX-License-Identifier: GPL-3.0-or-later
#include "profile.h"

#include "schemes.h"
#include "settings.h"
#include "shells.h"
#include "tokens.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QSettings>
#include <QUuid>

namespace {
const char kProfilesGroup[] = "Profiles";
const char kProfilePrefix[] = "Profile_";

QString tr(const char *text)
{
    return QCoreApplication::translate("Profile", text);
}
} // namespace

QStringList Profile::commandModes()
{
    return {QStringLiteral("login-shell"), QStringLiteral("shell"), QStringLiteral("custom")};
}

QStringList Profile::workingDirModes()
{
    return {QStringLiteral("inherit"), QStringLiteral("home"), QStringLiteral("custom")};
}

QStringList Profile::cursorShapes()
{
    return {QStringLiteral("block"), QStringLiteral("underline"), QStringLiteral("ibeam")};
}

QStringList Profile::bellModes()
{
    return {QStringLiteral("none"), QStringLiteral("visual"), QStringLiteral("sound")};
}

Profile Profile::builtinDefault()
{
    Profile p;
    p.id = QStringLiteral("default");
    p.name = tr("Default");
    p.fontFamily = QString::fromLatin1(Tokens::FontMono);
    return p;
}

void Profile::sanitize()
{
    name = name.simplified();
    if (name.isEmpty())
        name = tr("Profile");
    if (!commandModes().contains(commandMode))
        commandMode = QStringLiteral("login-shell");
    if (!workingDirModes().contains(workingDirMode))
        workingDirMode = QStringLiteral("inherit");
    if (!cursorShapes().contains(cursorShape))
        cursorShape = QStringLiteral("block");
    if (!bellModes().contains(bell))
        bell = QStringLiteral("visual");
    if (fontFamily.trimmed().isEmpty())
        fontFamily = QString::fromLatin1(Tokens::FontMono);
    fontSize = qBound(MinFontSize, fontSize, MaxFontSize);
    opacity = qBound(MinOpacity, opacity, 100);
    scrollbackLines = qBound(-1, scrollbackLines, MaxScrollback);
    if (colorScheme.isEmpty())
        colorScheme = QStringLiteral("auto");
    command = command.trimmed();
    workingDir = workingDir.trimmed();
}

QStringList Profile::resolveCommand(QString *error, QString *note) const
{
    if (commandMode == QLatin1String("custom")) {
        bool balanced = true;
        const QStringList argv = Shells::splitCommand(command, &balanced);
        if (argv.isEmpty()) {
            if (error)
                *error = balanced ? tr("Profile “%1” has no command to run.").arg(name)
                                  : tr("The command of profile “%1” has an unclosed quote.").arg(name);
            return {};
        }
        const QString program = Shells::resolveProgram(argv.first());
        if (program.isEmpty()) {
            if (error)
                *error = tr("“%1” was not found or is not executable.").arg(argv.first());
            return {};
        }
        QStringList resolved = argv;
        resolved[0] = program;
        return resolved;
    }

    QString program = Shells::loginShell();
    if (commandMode == QLatin1String("shell")) {
        if (Shells::isExecutable(shell)) {
            program = shell;
        } else if (note) {
            *note = tr("%1 is not available; using %2 instead.").arg(shell, program);
        }
    }
    QStringList argv{program};
    if (loginShell)
        argv << QStringLiteral("-l");
    return argv;
}

QString Profile::commandSummary() const
{
    if (commandMode == QLatin1String("custom"))
        return command.isEmpty() ? tr("No command") : command;
    const QString program = commandMode == QLatin1String("shell") ? shell : Shells::loginShell();
    const QString shellName = Shells::displayName(program);
    if (commandMode == QLatin1String("login-shell"))
        return tr("%1 (login shell of this account)").arg(shellName);
    return shellName;
}

QJsonObject Profile::toJson() const
{
    QString error, note;
    const QStringList argv = resolveCommand(&error, &note);
    return QJsonObject{
        {QStringLiteral("id"), id},
        {QStringLiteral("name"), name},
        {QStringLiteral("commandMode"), commandMode},
        {QStringLiteral("shell"), shell},
        {QStringLiteral("command"), command},
        {QStringLiteral("loginShell"), loginShell},
        {QStringLiteral("resolvedCommand"), QJsonArray::fromStringList(argv)},
        {QStringLiteral("resolveError"), error},
        {QStringLiteral("resolveNote"), note},
        {QStringLiteral("workingDirMode"), workingDirMode},
        {QStringLiteral("workingDir"), workingDir},
        {QStringLiteral("fontFamily"), fontFamily},
        {QStringLiteral("fontSize"), fontSize},
        {QStringLiteral("colorScheme"), colorScheme},
        {QStringLiteral("cursorShape"), cursorShape},
        {QStringLiteral("cursorBlink"), cursorBlink},
        {QStringLiteral("scrollbackLines"), scrollbackLines},
        {QStringLiteral("scrollbackUnlimited"), scrollbackLines < 0},
        {QStringLiteral("bell"), bell},
        {QStringLiteral("opacity"), opacity},
        {QStringLiteral("holdOnExit"), holdOnExit},
    };
}

void Profile::load(QSettings &s)
{
    const Profile d = builtinDefault();
    name = s.value(QStringLiteral("Name"), d.name).toString();
    commandMode = s.value(QStringLiteral("CommandMode"), d.commandMode).toString();
    shell = s.value(QStringLiteral("Shell")).toString();
    command = s.value(QStringLiteral("Command")).toString();
    loginShell = s.value(QStringLiteral("LoginShell"), d.loginShell).toBool();
    workingDirMode = s.value(QStringLiteral("WorkingDirMode"), d.workingDirMode).toString();
    workingDir = s.value(QStringLiteral("WorkingDir")).toString();
    fontFamily = s.value(QStringLiteral("FontFamily"), d.fontFamily).toString();
    fontSize = s.value(QStringLiteral("FontSize"), d.fontSize).toInt();
    colorScheme = s.value(QStringLiteral("ColorScheme"), d.colorScheme).toString();
    cursorShape = s.value(QStringLiteral("CursorShape"), d.cursorShape).toString();
    cursorBlink = s.value(QStringLiteral("CursorBlink"), d.cursorBlink).toBool();
    scrollbackLines = s.value(QStringLiteral("ScrollbackLines"), d.scrollbackLines).toInt();
    bell = s.value(QStringLiteral("Bell"), d.bell).toString();
    opacity = s.value(QStringLiteral("Opacity"), d.opacity).toInt();
    holdOnExit = s.value(QStringLiteral("HoldOnExit"), d.holdOnExit).toBool();
    sanitize();
}

void Profile::save(QSettings &s) const
{
    s.setValue(QStringLiteral("Name"), name);
    s.setValue(QStringLiteral("CommandMode"), commandMode);
    s.setValue(QStringLiteral("Shell"), shell);
    s.setValue(QStringLiteral("Command"), command);
    s.setValue(QStringLiteral("LoginShell"), loginShell);
    s.setValue(QStringLiteral("WorkingDirMode"), workingDirMode);
    s.setValue(QStringLiteral("WorkingDir"), workingDir);
    s.setValue(QStringLiteral("FontFamily"), fontFamily);
    s.setValue(QStringLiteral("FontSize"), fontSize);
    s.setValue(QStringLiteral("ColorScheme"), colorScheme);
    s.setValue(QStringLiteral("CursorShape"), cursorShape);
    s.setValue(QStringLiteral("CursorBlink"), cursorBlink);
    s.setValue(QStringLiteral("ScrollbackLines"), scrollbackLines);
    s.setValue(QStringLiteral("Bell"), bell);
    s.setValue(QStringLiteral("Opacity"), opacity);
    s.setValue(QStringLiteral("HoldOnExit"), holdOnExit);
}

// ---- ProfileManager -------------------------------------------------------------

ProfileManager *ProfileManager::instance()
{
    static ProfileManager *self = new ProfileManager(QCoreApplication::instance());
    return self;
}

ProfileManager::ProfileManager(QObject *parent)
    : QObject(parent)
{
    load();
}

void ProfileManager::load()
{
    QSettings &s = Settings::instance()->store();
    m_profiles.clear();

    s.beginGroup(QLatin1String(kProfilesGroup));
    const QStringList order = s.value(QStringLiteral("Order")).toStringList();
    const QString storedDefault = s.value(QStringLiteral("Default")).toString();
    s.endGroup();

    for (const QString &id : order) {
        if (id.isEmpty() || contains(id))
            continue;
        const QString group = QLatin1String(kProfilePrefix) + id;
        if (!s.childGroups().contains(group))
            continue;
        Profile p;
        p.id = id;
        s.beginGroup(group);
        p.load(s);
        s.endGroup();
        m_profiles.append(p);
    }

    if (m_profiles.isEmpty())
        m_profiles.append(Profile::builtinDefault());
    m_defaultId = contains(storedDefault) ? storedDefault : m_profiles.first().id;
}

bool ProfileManager::contains(const QString &id) const
{
    for (const Profile &p : m_profiles) {
        if (p.id == id)
            return true;
    }
    return false;
}

Profile ProfileManager::defaultProfile() const
{
    for (const Profile &p : m_profiles) {
        if (p.id == m_defaultId)
            return p;
    }
    return m_profiles.first();
}

Profile ProfileManager::profile(const QString &id) const
{
    for (const Profile &p : m_profiles) {
        if (p.id == id)
            return p;
    }
    return defaultProfile();
}

Profile ProfileManager::byName(const QString &name) const
{
    for (const Profile &p : m_profiles) {
        if (p.name.compare(name, Qt::CaseInsensitive) == 0 || p.id == name)
            return p;
    }
    return Profile();
}

void ProfileManager::setProfiles(const QList<Profile> &profiles, const QString &defaultId)
{
    QList<Profile> clean;
    for (Profile p : profiles) {
        p.sanitize();
        if (p.id.isEmpty())
            p.id = newId();
        clean.append(p);
    }
    if (clean.isEmpty())
        clean.append(Profile::builtinDefault());

    QSettings &s = Settings::instance()->store();
    // Drop the sections of profiles that no longer exist.
    const QStringList groups = s.childGroups();
    for (const QString &group : groups) {
        if (group.startsWith(QLatin1String(kProfilePrefix)))
            s.remove(group);
    }
    QStringList order;
    for (const Profile &p : std::as_const(clean)) {
        order << p.id;
        s.beginGroup(QLatin1String(kProfilePrefix) + p.id);
        p.save(s);
        s.endGroup();
    }
    m_profiles = clean;
    m_defaultId = contains(defaultId) ? defaultId : clean.first().id;

    s.beginGroup(QLatin1String(kProfilesGroup));
    s.setValue(QStringLiteral("Order"), order);
    s.setValue(QStringLiteral("Default"), m_defaultId);
    s.endGroup();
    s.sync();
    Q_EMIT changed();
}

QString ProfileManager::newId()
{
    return QUuid::createUuid().toString(QUuid::Id128).left(8);
}

QString ProfileManager::uniqueName(const QList<Profile> &profiles, const QString &base)
{
    auto taken = [&profiles](const QString &name) {
        for (const Profile &p : profiles) {
            if (p.name.compare(name, Qt::CaseInsensitive) == 0)
                return true;
        }
        return false;
    };
    if (!taken(base))
        return base;
    for (int n = 2;; ++n) {
        const QString candidate = QStringLiteral("%1 %2").arg(base).arg(n);
        if (!taken(candidate))
            return candidate;
    }
}

QJsonObject ProfileManager::toJson() const
{
    QJsonArray list;
    for (const Profile &p : m_profiles) {
        QJsonObject obj = p.toJson();
        obj.insert(QStringLiteral("isDefault"), p.id == m_defaultId);
        obj.insert(QStringLiteral("effectiveColorScheme"),
                   QJsonObject{
                       {QStringLiteral("dark"), Schemes::resolve(p.colorScheme, true)},
                       {QStringLiteral("light"), Schemes::resolve(p.colorScheme, false)},
                   });
        list.append(obj);
    }
    return QJsonObject{
        {QStringLiteral("file"), Settings::instance()->filePath()},
        {QStringLiteral("default"), m_defaultId},
        {QStringLiteral("profiles"), list},
    };
}
