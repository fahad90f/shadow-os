// SPDX-License-Identifier: GPL-3.0-or-later
#include "launcher.h"

#include "interaction.h"
#include "util.h"

#include <command.h>

#include <KApplicationTrader>
#include <KIO/ApplicationLauncherJob>
#include <KIO/JobUiDelegateFactory>
#include <KProtocolInfo>
#include <KService>

#include <QClipboard>
#include <QGuiApplication>
#include <QJsonArray>
#include <QProcess>
#include <QRegularExpression>

using namespace Qt::StringLiterals;

namespace {

struct Protocol {
    const char *scheme;
    const char *label;
    int port;
    const char *package;    // Debian package that ships the KIO worker
};

const Protocol kProtocols[] = {
    {"sftp", QT_TRANSLATE_NOOP("Launcher", "SSH (SFTP)"), 22, "kio-extras"},
    {"smb", QT_TRANSLATE_NOOP("Launcher", "Windows share (SMB)"), 445, "kio-extras"},
    {"ftp", QT_TRANSLATE_NOOP("Launcher", "FTP"), 21, "kio6"},
    {"webdav", QT_TRANSLATE_NOOP("Launcher", "WebDAV (HTTP)"), 80, "kio6"},
    {"webdavs", QT_TRANSLATE_NOOP("Launcher", "Secure WebDAV (HTTPS)"), 443, "kio6"},
    {"nfs", QT_TRANSLATE_NOOP("Launcher", "NFS"), 2049, "kio-extras"},
    {"fish", QT_TRANSLATE_NOOP("Launcher", "SSH shell (FISH)"), 22, "kio-extras"},
};

QVariantMap describe(const KService::Ptr &service, bool isDefault)
{
    return {
        {u"id"_s, service->storageId()},
        {u"name"_s, service->name()},
        {u"icon"_s, service->icon()},
        {u"isDefault"_s, isDefault},
    };
}

void launch(const KService::Ptr &service, const QList<QUrl> &urls)
{
    auto *job = new KIO::ApplicationLauncherJob(service);
    job->setUrls(urls);
    job->setUiDelegate(KIO::createDefaultJobUiDelegate());
    const QString name = service->name();
    QObject::connect(job, &KJob::result, qApp, [name](KJob *j) {
        if (j->error() != KJob::NoError && j->error() != KJob::KilledJobError)
            Interaction::instance()->reportError(Launcher::tr("Could not start %1").arg(name), j->errorString());
    });
    job->start();
}

} // namespace

Launcher::Launcher(QObject *parent)
    : QObject(parent)
{
}

Launcher *Launcher::instance()
{
    static Launcher *self = new Launcher(qApp);
    return self;
}

Launcher *Launcher::create(QQmlEngine *, QJSEngine *)
{
    Launcher *self = instance();
    QQmlEngine::setObjectOwnership(self, QQmlEngine::CppOwnership);
    return self;
}

void Launcher::setStartup(const QList<QUrl> &locations, const QUrl &select)
{
    m_startLocations = locations;
    m_select = select;
}

bool Launcher::terminalAvailable() const
{
    static const bool found = Nexus::Command::available(u"nexus-terminal"_s)
        || Nexus::Command::available(u"x-terminal-emulator"_s);
    return found;
}

bool Launcher::protocolAvailable(const QString &scheme) const
{
    return KProtocolInfo::isKnownProtocol(scheme);
}

bool Launcher::networkAvailable() const
{
    return std::any_of(std::begin(kProtocols), std::end(kProtocols), [](const Protocol &p) {
        return KProtocolInfo::isKnownProtocol(QString::fromLatin1(p.scheme));
    });
}

QVariantList Launcher::protocols() const
{
    QVariantList list;
    for (const Protocol &p : kProtocols) {
        const QString scheme = QString::fromLatin1(p.scheme);
        list.append(QVariantMap{
            {u"scheme"_s, scheme},
            {u"label"_s, tr(p.label)},
            {u"available"_s, KProtocolInfo::isKnownProtocol(scheme)},
            {u"defaultPort"_s, p.port},
            {u"package"_s, QString::fromLatin1(p.package)},
        });
    }
    return list;
}

QJsonObject Launcher::protocolsJson() const
{
    QJsonArray remote;
    for (const Protocol &p : kProtocols) {
        const QString scheme = QString::fromLatin1(p.scheme);
        const bool available = KProtocolInfo::isKnownProtocol(scheme);
        remote.append(QJsonObject{
            {u"scheme"_s, scheme},
            {u"label"_s, QString::fromLatin1(p.label)},
            {u"available"_s, available},
            {u"defaultPort"_s, p.port},
            {u"package"_s, QString::fromLatin1(p.package)},
            {u"worker"_s, available ? KProtocolInfo::exec(scheme) : QString()},
        });
    }
    QStringList known = KProtocolInfo::protocols();
    known.sort();
    return {
        {u"remote"_s, remote},
        {u"trash"_s, KProtocolInfo::isKnownProtocol(u"trash"_s)},
        {u"known"_s, QJsonArray::fromStringList(known)},
    };
}

void Launcher::openFile(const QUrl &url, const QString &mimeType)
{
    const KService::Ptr service = KApplicationTrader::preferredService(mimeType);
    if (!service) {
        Q_EMIT openWithRequested(url, mimeType);
        return;
    }
    launch(service, {url});
}

QVariantList Launcher::applicationsFor(const QString &mimeType) const
{
    QVariantList list;
    if (mimeType.isEmpty())
        return list;
    const KService::Ptr preferred = KApplicationTrader::preferredService(mimeType);
    const KService::List services = KApplicationTrader::queryByMimeType(mimeType);
    for (const KService::Ptr &service : services) {
        if (service->noDisplay() || service->exec().isEmpty())
            continue;
        list.append(describe(service, preferred && service->storageId() == preferred->storageId()));
    }
    return list;
}

QVariantList Launcher::allApplications() const
{
    KService::List services = KApplicationTrader::query([](const KService::Ptr &service) {
        return !service->noDisplay() && !service->exec().isEmpty();
    });
    std::sort(services.begin(), services.end(), [](const KService::Ptr &a, const KService::Ptr &b) {
        return a->name().localeAwareCompare(b->name()) < 0;
    });
    QVariantList list;
    for (const KService::Ptr &service : std::as_const(services))
        list.append(describe(service, false));
    return list;
}

void Launcher::openWith(const QList<QUrl> &urls, const QString &applicationId, const QString &mimeType, bool makeDefault)
{
    const KService::Ptr service = KService::serviceByStorageId(applicationId);
    if (!service) {
        Interaction::instance()->reportError(tr("Could not open the file"),
                                             tr("The application “%1” is no longer installed.").arg(applicationId));
        return;
    }
    if (makeDefault && !mimeType.isEmpty())
        KApplicationTrader::setPreferredService(mimeType, service);
    launch(service, urls);
}

void Launcher::openTerminal(const QUrl &folder)
{
    if (!folder.isLocalFile()) {
        Interaction::instance()->reportError(tr("Could not open a terminal"),
                                             tr("A terminal can only be opened in a local folder."));
        return;
    }
    const QString path = folder.toLocalFile();
    bool started = false;
    if (Nexus::Command::available(u"nexus-terminal"_s))
        started = QProcess::startDetached(u"nexus-terminal"_s, {u"--workdir"_s, path}, path);
    else if (Nexus::Command::available(u"x-terminal-emulator"_s))
        started = QProcess::startDetached(u"x-terminal-emulator"_s, {}, path);
    if (!started) {
        Interaction::instance()->reportError(tr("Could not open a terminal"),
                                             tr("Neither nexus-terminal nor x-terminal-emulator could be started."));
    }
}

void Launcher::copyText(const QString &text)
{
    QGuiApplication::clipboard()->setText(text);
}

QUrl Launcher::serverUrl(const QString &scheme, const QString &host, int port, const QString &path,
                         const QString &user) const
{
    const QString trimmedHost = host.trimmed();
    static const QRegularExpression badHost(u"[\\s/@:?#]"_s);
    // IPv6 literals contain colons and are written in brackets.
    const bool ipv6 = trimmedHost.startsWith(u'[') && trimmedHost.endsWith(u']');
    if (scheme.isEmpty() || trimmedHost.isEmpty() || (!ipv6 && trimmedHost.contains(badHost)))
        return {};
    QUrl url;
    url.setScheme(scheme);
    url.setHost(trimmedHost);
    if (url.host().isEmpty())
        return {};
    for (const Protocol &p : kProtocols) {
        if (scheme == QLatin1String(p.scheme) && port > 0 && port != p.port)
            url.setPort(port);
    }
    if (!user.trimmed().isEmpty())
        url.setUserName(user.trimmed());
    QString folder = path.trimmed();
    if (!folder.startsWith(u'/'))
        folder.prepend(u'/');
    url.setPath(folder);
    return url.isValid() ? url : QUrl();
}
