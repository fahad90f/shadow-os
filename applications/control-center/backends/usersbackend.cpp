// SPDX-License-Identifier: GPL-3.0-or-later
#include "usersbackend.h"

#include "dumpregistry.h"

#include <command.h>

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTemporaryFile>

#include <algorithm>
#include <optional>

#include <grp.h>
#include <pwd.h>
#include <unistd.h>

NEXUS_CC_DUMP("users", [] {
    UsersBackend backend;
    if (!backend.ready()) {
        QEventLoop loop;
        QObject::connect(&backend, &UsersBackend::changed, &loop, [&] { if (backend.ready()) loop.quit(); });
        QTimer::singleShot(30000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return QJsonValue(backend.toJson());
})

namespace {
const QString kService = QStringLiteral("org.freedesktop.Accounts");
const QString kPath = QStringLiteral("/org/freedesktop/Accounts");
const QString kUserInterface = QStringLiteral("org.freedesktop.Accounts.User");
const QString kProperties = QStringLiteral("org.freedesktop.DBus.Properties");
// A polkit password prompt may be open for a while.
const int kAuthTimeoutMs = 5 * 60 * 1000;

QDBusPendingCall call(const QString &service, const QString &path, const QString &interface, const QString &method,
                      const QVariantList &arguments = {}, int timeoutMs = 25000)
{
    QDBusMessage message = QDBusMessage::createMethodCall(service, path, interface, method);
    message.setArguments(arguments);
    return QDBusConnection::systemBus().asyncCall(message, timeoutMs);
}

QString errorText(const QDBusError &error)
{
    const QString name = error.name();
    if (name.endsWith(QLatin1String(".PermissionDenied")) || name.endsWith(QLatin1String(".AccessDenied"))
        || name.endsWith(QLatin1String(".NotAuthorized")))
        return UsersBackend::tr("the request was not authorised");
    if (error.type() == QDBusError::NoReply || error.type() == QDBusError::Timeout)
        return UsersBackend::tr("AccountsService did not answer");
    // glib prefixes remote errors with "GDBus.Error:<name>: ".
    static const QRegularExpression prefix(QStringLiteral("^GDBus\\.Error:[^ ]+: "));
    QString message = error.message();
    message.remove(prefix);
    return message.isEmpty() ? name : message;
}

QString helperPath()
{
    const QString dir = qEnvironmentVariable("NEXUS_CC_HELPER_DIR");
    return (dir.isEmpty() ? QStringLiteral("/usr/libexec/nexus") : dir) + QStringLiteral("/users-helper");
}

bool hasControlCharacters(const QString &text)
{
    for (const QChar c : text) {
        if (c.unicode() < 32 || c.unicode() == 127)
            return true;
    }
    return false;
}

// [Autologin] User of one SDDM configuration file; no value when the file does not set it.
std::optional<QString> autologinUserIn(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    std::optional<QString> value;
    QString section;
    const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
            section = line.mid(1, line.size() - 2).trimmed();
        } else if (section == QLatin1String("Autologin")) {
            static const QRegularExpression user(QStringLiteral("^User\\s*=(.*)$"));
            const QRegularExpressionMatch match = user.match(line);
            if (match.hasMatch())
                value = match.captured(1).trimmed();
        }
    }
    return value;
}
} // namespace

UsersBackend::UsersBackend(QObject *parent)
    : QObject(parent)
{
    if (const passwd *entry = getpwuid(getuid()))
        m_currentUser = QString::fromLocal8Bit(entry->pw_name);

    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setInterval(250);
    connect(&m_refreshTimer, &QTimer::timeout, this, &UsersBackend::refresh);

    QDBusConnection bus = QDBusConnection::systemBus();
    if (bus.isConnected()) {
        bus.connect(kService, kPath, kService, QStringLiteral("UserAdded"), this, SLOT(scheduleRefresh()));
        bus.connect(kService, kPath, kService, QStringLiteral("UserDeleted"), this, SLOT(scheduleRefresh()));
        bus.connect(kService, QString(), kUserInterface, QStringLiteral("Changed"), this, SLOT(scheduleRefresh()));
    }
    refresh();
}

void UsersBackend::scheduleRefresh()
{
    m_refreshTimer.start();
}

bool UsersBackend::sddmInstalled() const
{
    return QFile::exists(QStringLiteral("/usr/bin/sddm"));
}

int UsersBackend::adminCount() const
{
    int count = 0;
    for (const QVariant &user : m_users) {
        const QVariantMap map = user.toMap();
        if (map.value(QStringLiteral("admin")).toBool() && !map.value(QStringLiteral("locked")).toBool())
            ++count;
    }
    return count;
}

void UsersBackend::loadAutoLogin()
{
    // The order in which SDDM reads its configuration; a later file wins.
    QStringList files;
    for (const QString &dir : {QStringLiteral("/usr/lib/sddm/sddm.conf.d"), QStringLiteral("/etc/sddm.conf.d")}) {
        for (const QString &name : QDir(dir).entryList({QStringLiteral("*.conf")}, QDir::Files, QDir::Name))
            files << dir + QLatin1Char('/') + name;
    }
    files << QStringLiteral("/etc/sddm.conf");
    m_autoLoginUser.clear();
    for (const QString &path : files) {
        if (const std::optional<QString> value = autologinUserIn(path))
            m_autoLoginUser = *value;
    }
}

void UsersBackend::refresh()
{
    const int generation = ++m_generation;
    loadAutoLogin();
    m_collecting.clear();
    if (!QDBusConnection::systemBus().isConnected()) {
        m_available = false;
        m_statusMessage = tr("The system message bus is not reachable, so the user accounts cannot be read.");
        finishRefresh();
        return;
    }
    m_pending = 2;

    auto *list = new QDBusPendingCallWatcher(call(kService, kPath, kService, QStringLiteral("ListCachedUsers")), this);
    connect(list, &QDBusPendingCallWatcher::finished, this, [this, generation](QDBusPendingCallWatcher *watcher) {
        watcher->deleteLater();
        if (generation != m_generation)
            return;
        const QDBusPendingReply<QList<QDBusObjectPath>> reply = *watcher;
        if (reply.isError()) {
            m_available = false;
            m_statusMessage = tr("AccountsService (org.freedesktop.Accounts) is not available: %1").arg(reply.error().message());
        } else {
            m_available = true;
            m_statusMessage.clear();
            const QList<QDBusObjectPath> paths = reply.value();
            for (const QDBusObjectPath &path : paths) {
                ++m_pending;
                auto *properties = new QDBusPendingCallWatcher(
                    call(kService, path.path(), kProperties, QStringLiteral("GetAll"), {kUserInterface}), this);
                connect(properties, &QDBusPendingCallWatcher::finished, this, [this, generation, path](QDBusPendingCallWatcher *w) {
                    w->deleteLater();
                    if (generation != m_generation)
                        return;
                    const QDBusPendingReply<QVariantMap> user = *w;
                    if (!user.isError() && !user.value().value(QStringLiteral("SystemAccount")).toBool()) {
                        const QVariantMap p = user.value();
                        const QString userName = p.value(QStringLiteral("UserName")).toString();
                        const QString realName = p.value(QStringLiteral("RealName")).toString();
                        const QString iconFile = p.value(QStringLiteral("IconFile")).toString();
                        m_collecting.append(QVariantMap{
                            {QStringLiteral("path"), path.path()},
                            {QStringLiteral("uid"), p.value(QStringLiteral("Uid")).toULongLong()},
                            {QStringLiteral("userName"), userName},
                            {QStringLiteral("realName"), realName},
                            {QStringLiteral("displayName"), realName.isEmpty() ? userName : realName},
                            {QStringLiteral("admin"), p.value(QStringLiteral("AccountType")).toInt() == 1},
                            {QStringLiteral("locked"), p.value(QStringLiteral("Locked")).toBool()},
                            {QStringLiteral("passwordMode"), p.value(QStringLiteral("PasswordMode")).toInt()},
                            {QStringLiteral("iconFile"), QFile::exists(iconFile) ? iconFile : QString()},
                            {QStringLiteral("home"), p.value(QStringLiteral("HomeDirectory")).toString()},
                            {QStringLiteral("local"), p.value(QStringLiteral("LocalAccount")).toBool()},
                        });
                    }
                    if (--m_pending == 0)
                        finishRefresh();
                });
            }
        }
        if (--m_pending == 0)
            finishRefresh();
    });

    // Who is signed in (any session, graphical or not).
    auto *sessions = new QDBusPendingCallWatcher(
        call(QStringLiteral("org.freedesktop.login1"), QStringLiteral("/org/freedesktop/login1"),
             QStringLiteral("org.freedesktop.login1.Manager"), QStringLiteral("ListUsers")), this);
    connect(sessions, &QDBusPendingCallWatcher::finished, this, [this, generation](QDBusPendingCallWatcher *watcher) {
        watcher->deleteLater();
        if (generation != m_generation)
            return;
        const QDBusMessage reply = watcher->reply();
        m_loggedIn.clear();
        m_sessionsKnown = reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty();
        if (m_sessionsKnown) {
            const QDBusArgument argument = reply.arguments().constFirst().value<QDBusArgument>();
            argument.beginArray();
            while (!argument.atEnd()) {
                uint uid = 0;
                QString name;
                QDBusObjectPath path;
                argument.beginStructure();
                argument >> uid >> name >> path;
                argument.endStructure();
                m_loggedIn.insert(uid);
            }
            argument.endArray();
        }
        if (--m_pending == 0)
            finishRefresh();
    });
}

void UsersBackend::finishRefresh()
{
    std::sort(m_collecting.begin(), m_collecting.end(), [](const QVariantMap &a, const QVariantMap &b) {
        return a.value(QStringLiteral("uid")).toULongLong() < b.value(QStringLiteral("uid")).toULongLong();
    });
    m_users.clear();
    for (QVariantMap user : std::as_const(m_collecting)) {
        const QString name = user.value(QStringLiteral("userName")).toString();
        user.insert(QStringLiteral("current"), name == m_currentUser);
        user.insert(QStringLiteral("loggedIn"), m_loggedIn.contains(user.value(QStringLiteral("uid")).toULongLong()));
        user.insert(QStringLiteral("autoLogin"), !m_autoLoginUser.isEmpty() && name == m_autoLoginUser);
        m_users.append(user);
    }
    m_collecting.clear();
    m_ready = true;
    Q_EMIT changed();
}

QVariantMap UsersBackend::user(const QString &userName) const
{
    for (const QVariant &user : m_users) {
        const QVariantMap map = user.toMap();
        if (map.value(QStringLiteral("userName")).toString() == userName)
            return map;
    }
    return {};
}

QString UsersBackend::pathOf(const QString &userName) const
{
    return user(userName).value(QStringLiteral("path")).toString();
}

void UsersBackend::setBusy(bool on)
{
    m_busy += on ? 1 : -1;
    Q_EMIT busyChanged();
}

// ---- validation -------------------------------------------------------------------------

QString UsersBackend::userNameProblem(const QString &userName) const
{
    static const QRegularExpression pattern(QStringLiteral("^[a-z_][a-z0-9_-]{0,31}$"));
    if (userName.isEmpty())
        return tr("Enter a user name.");
    if (!pattern.match(userName).hasMatch())
        return tr("Use lower-case letters, digits, “-” and “_”, starting with a letter; at most 32 characters.");
    const QByteArray name = userName.toLatin1();
    if (getpwnam(name.constData()))
        return tr("An account named “%1” already exists.").arg(userName);
    if (getgrnam(name.constData()))
        return tr("A group named “%1” already exists.").arg(userName);
    return QString();
}

QString UsersBackend::realNameProblem(const QString &realName) const
{
    if (realName.size() > 120)
        return tr("The name is too long.");
    if (realName.contains(QLatin1Char(':')) || realName.contains(QLatin1Char(',')) || hasControlCharacters(realName))
        return tr("The name cannot contain “:”, “,” or control characters.");
    return QString();
}

QString UsersBackend::passwordProblem(const QString &password, const QString &repeated) const
{
    if (password.isEmpty())
        return tr("Enter a password.");
    if (password.size() < 8)
        return tr("Use at least 8 characters.");
    if (password.toUtf8().size() > 256)
        return tr("The password is too long.");
    if (hasControlCharacters(password))
        return tr("The password cannot contain control characters.");
    if (password != repeated)
        return tr("The two passwords are not the same.");
    return QString();
}

QString UsersBackend::removeBlockReason(const QString &userName) const
{
    const QVariantMap account = user(userName);
    if (account.isEmpty())
        return tr("This account no longer exists.");
    if (account.value(QStringLiteral("current")).toBool())
        return tr("This is the account you are signed in with. Sign in as another administrator to remove it.");
    if (account.value(QStringLiteral("loggedIn")).toBool())
        return tr("%1 is signed in on this computer and has to sign out first.").arg(account.value(QStringLiteral("displayName")).toString());
    if (account.value(QStringLiteral("admin")).toBool() && !account.value(QStringLiteral("locked")).toBool() && adminCount() <= 1)
        return tr("This is the only administrator. Without one, nobody could manage this computer.");
    return QString();
}

QString UsersBackend::demoteBlockReason(const QString &userName) const
{
    const QVariantMap account = user(userName);
    if (account.value(QStringLiteral("admin")).toBool() && !account.value(QStringLiteral("locked")).toBool() && adminCount() <= 1)
        return tr("This is the only administrator. Make another account an administrator first.");
    return QString();
}

bool UsersBackend::needsCurrentPassword(const QString &userName) const
{
    return geteuid() != 0 && userName == m_currentUser;
}

// ---- changes ----------------------------------------------------------------------------

void UsersBackend::callUser(const QString &userName, const QString &method, const QVariantList &arguments,
                            const QString &successMessage, const QString &failurePrefix)
{
    const QString path = pathOf(userName);
    if (path.isEmpty()) {
        Q_EMIT failed(tr("The account “%1” no longer exists.").arg(userName));
        return;
    }
    setBusy(true);
    auto *watcher = new QDBusPendingCallWatcher(call(kService, path, kUserInterface, method, arguments, kAuthTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, successMessage, failurePrefix](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        setBusy(false);
        if (w->isError())
            Q_EMIT failed(failurePrefix.arg(errorText(w->error())));
        else if (!successMessage.isEmpty())
            Q_EMIT succeeded(successMessage);
        refresh();
    });
}

void UsersBackend::runHelper(const QStringList &arguments, const QByteArray &stdinData,
                             std::function<void(bool ok, const QString &error)> done)
{
    const QString helper = helperPath();
    if (!QFile::exists(helper)) {
        done(false, tr("the helper %1 is not installed").arg(helper));
        return;
    }
    const auto finished = [done](const Nexus::CommandResult &result) {
        const QJsonObject reply = QJsonDocument::fromJson(result.out).object();
        if (result.ok() && reply.value(QStringLiteral("ok")).toBool()) {
            done(true, QString());
            return;
        }
        QString error = reply.value(QStringLiteral("error")).toString();
        if (error.isEmpty())
            error = result.exitCode == 126 ? UsersBackend::tr("the request was not authorised")
                  : result.exitCode == 127 ? UsersBackend::tr("authentication failed, or no authentication agent is running in this session")
                  : result.exitCode == -1 ? UsersBackend::tr("pkexec could not be started")
                  : result.exitCode == -2 ? UsersBackend::tr("the helper did not finish in time")
                  : result.errText().trimmed();
        done(false, error);
    };
    if (geteuid() == 0)
        Nexus::Command::run(this, helper, arguments, finished, kAuthTimeoutMs, stdinData);
    else
        Nexus::Command::run(this, QStringLiteral("pkexec"), QStringList{helper} + arguments, finished, kAuthTimeoutMs, stdinData);
}

void UsersBackend::addUser(const QString &userName, const QString &realName, bool administrator, const QString &password)
{
    const QString name = realName.trimmed();
    QString problem = userNameProblem(userName);
    if (problem.isEmpty())
        problem = realNameProblem(name);
    if (problem.isEmpty())
        problem = passwordProblem(password, password);
    if (!problem.isEmpty()) {
        Q_EMIT failed(problem);
        return;
    }
    setBusy(true);
    auto *watcher = new QDBusPendingCallWatcher(
        call(kService, kPath, kService, QStringLiteral("CreateUser"), {userName, name, administrator ? 1 : 0}, kAuthTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, userName, password](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        if (w->isError()) {
            setBusy(false);
            Q_EMIT failed(tr("The account was not created: %1").arg(errorText(w->error())));
            refresh();
            return;
        }
        runHelper({QStringLiteral("set-password")}, (userName + QLatin1Char(':') + password + QLatin1Char('\n')).toUtf8(),
                  [this, userName](bool ok, const QString &error) {
            setBusy(false);
            if (ok)
                Q_EMIT succeeded(tr("The account “%1” was created.").arg(userName));
            else
                Q_EMIT failed(tr("The account “%1” was created, but its password could not be set (%2). "
                                 "Nobody can sign in with it until a password is set.").arg(userName, error));
            refresh();
        });
    });
}

void UsersBackend::removeUser(const QString &userName, bool removeFiles)
{
    const QString block = removeBlockReason(userName);
    if (!block.isEmpty()) {
        Q_EMIT failed(block);
        return;
    }
    const QVariantMap account = user(userName);
    const bool wasAutoLogin = account.value(QStringLiteral("autoLogin")).toBool();
    setBusy(true);
    auto *watcher = new QDBusPendingCallWatcher(
        call(kService, kPath, kService, QStringLiteral("DeleteUser"),
             {QVariant::fromValue<qlonglong>(account.value(QStringLiteral("uid")).toLongLong()), removeFiles}, kAuthTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, userName, wasAutoLogin](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        setBusy(false);
        if (w->isError()) {
            Q_EMIT failed(tr("The account was not removed: %1").arg(errorText(w->error())));
        } else {
            Q_EMIT succeeded(tr("The account “%1” was removed.").arg(userName));
            // SDDM must not try to log a removed account in.
            if (wasAutoLogin)
                setAutoLogin(userName, false);
        }
        refresh();
    });
}

void UsersBackend::setRealName(const QString &userName, const QString &realName)
{
    const QString name = realName.trimmed();
    const QString problem = realNameProblem(name);
    if (!problem.isEmpty()) {
        Q_EMIT failed(problem);
        return;
    }
    callUser(userName, QStringLiteral("SetRealName"), {name}, tr("The name was changed."), tr("The name was not changed: %1"));
}

void UsersBackend::setAdministrator(const QString &userName, bool administrator)
{
    if (!administrator) {
        const QString block = demoteBlockReason(userName);
        if (!block.isEmpty()) {
            Q_EMIT failed(block);
            return;
        }
    }
    callUser(userName, QStringLiteral("SetAccountType"), {administrator ? 1 : 0},
             administrator ? tr("%1 is now an administrator.").arg(userName) : tr("%1 is now a standard user.").arg(userName),
             tr("The account type was not changed: %1"));
}

void UsersBackend::setPassword(const QString &userName, const QString &password, const QString &currentPassword)
{
    const QString problem = passwordProblem(password, password);
    if (!problem.isEmpty() || user(userName).isEmpty()) {
        Q_EMIT failed(problem.isEmpty() ? tr("The account “%1” no longer exists.").arg(userName) : problem);
        return;
    }
    setBusy(true);
    if (needsCurrentPassword(userName)) {
        if (currentPassword.isEmpty() || hasControlCharacters(currentPassword)) {
            setBusy(false);
            Q_EMIT failed(tr("Enter your current password."));
            return;
        }
        // passwd asks: current password, new password, new password again.
        const QByteArray input = (currentPassword + QLatin1Char('\n') + password + QLatin1Char('\n') + password + QLatin1Char('\n')).toUtf8();
        Nexus::Command::run(this, QStringLiteral("passwd"), {}, [this](const Nexus::CommandResult &result) {
            setBusy(false);
            if (result.ok()) {
                Q_EMIT succeeded(tr("Your password was changed."));
            } else {
                const QStringList lines = result.errText().trimmed().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
                Q_EMIT failed(tr("Your password was not changed: %1")
                                  .arg(result.exitCode == -1 ? tr("passwd could not be started")
                                       : lines.isEmpty() ? tr("the current password is wrong or the new one was refused") : lines.constLast()));
            }
            refresh();
        }, 60000, input);
        return;
    }
    runHelper({QStringLiteral("set-password")}, (userName + QLatin1Char(':') + password + QLatin1Char('\n')).toUtf8(),
              [this, userName](bool ok, const QString &error) {
        setBusy(false);
        if (ok)
            Q_EMIT succeeded(tr("The password of “%1” was changed.").arg(userName));
        else
            Q_EMIT failed(tr("The password was not changed: %1").arg(error));
        refresh();
    });
}

void UsersBackend::setAutoLogin(const QString &userName, bool on)
{
    if (on && user(userName).isEmpty()) {
        Q_EMIT failed(tr("The account “%1” no longer exists.").arg(userName));
        return;
    }
    setBusy(true);
    runHelper(on ? QStringList{QStringLiteral("set-autologin"), userName} : QStringList{QStringLiteral("clear-autologin")}, {},
              [this, userName, on](bool ok, const QString &error) {
        setBusy(false);
        if (ok)
            Q_EMIT succeeded(on ? tr("“%1” will be signed in automatically when the computer starts.").arg(userName)
                                : tr("Automatic sign-in is off."));
        else
            Q_EMIT failed(tr("Automatic sign-in was not changed: %1").arg(error));
        refresh();
    });
}

void UsersBackend::setAvatar(const QString &userName, const QUrl &imageFile)
{
    const QImage source(imageFile.toLocalFile());
    if (source.isNull()) {
        Q_EMIT failed(tr("%1 is not a picture that can be read.").arg(imageFile.toLocalFile()));
        return;
    }
    // AccountsService accepts small files only: store a 256 px square cut from the centre.
    const int side = qMin(source.width(), source.height());
    const QImage picture = source.copy((source.width() - side) / 2, (source.height() - side) / 2, side, side)
                               .scaled(256, 256, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QTemporaryFile file(QDir::tempPath() + QStringLiteral("/nexus-avatar-XXXXXX.png"));
    file.setAutoRemove(false);
    if (!file.open() || !picture.save(&file, "PNG")) {
        Q_EMIT failed(tr("The picture could not be prepared."));
        return;
    }
    const QString path = file.fileName();
    file.close();

    const QString account = pathOf(userName);
    if (account.isEmpty()) {
        QFile::remove(path);
        Q_EMIT failed(tr("The account “%1” no longer exists.").arg(userName));
        return;
    }
    setBusy(true);
    auto *watcher = new QDBusPendingCallWatcher(call(kService, account, kUserInterface, QStringLiteral("SetIconFile"), {path}, kAuthTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, path](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        QFile::remove(path);
        setBusy(false);
        if (w->isError())
            Q_EMIT failed(tr("The picture was not changed: %1").arg(errorText(w->error())));
        else
            Q_EMIT succeeded(tr("The picture was changed."));
        ++m_iconRevision;
        refresh();
    });
}

QJsonObject UsersBackend::toJson() const
{
    QJsonArray users;
    for (const QVariant &user : m_users) {
        QJsonObject object = QJsonObject::fromVariantMap(user.toMap());
        object.insert(QStringLiteral("uid"), user.toMap().value(QStringLiteral("uid")).toLongLong());
        users.append(object);
    }
    return {
        {QStringLiteral("available"), m_available},
        {QStringLiteral("statusMessage"), m_statusMessage},
        {QStringLiteral("currentUser"), m_currentUser},
        {QStringLiteral("administrators"), adminCount()},
        {QStringLiteral("sessionsKnown"), m_sessionsKnown},
        {QStringLiteral("sddmInstalled"), sddmInstalled()},
        {QStringLiteral("autoLoginUser"), m_autoLoginUser},
        {QStringLiteral("users"), users},
    };
}
