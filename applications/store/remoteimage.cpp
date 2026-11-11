// SPDX-License-Identifier: GPL-3.0-or-later
#include "remoteimage.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QStandardPaths>

namespace {

constexpr qint64 kMaxBytes = 12 * 1024 * 1024;
constexpr int kTimeoutMs = 25000;

QNetworkAccessManager *network()
{
    static QNetworkAccessManager *manager = nullptr;
    if (!manager) {
        manager = new QNetworkAccessManager(QCoreApplication::instance());
        manager->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
        manager->setStrictTransportSecurityEnabled(true);
        manager->setTransferTimeout(kTimeoutMs);
    }
    return manager;
}

bool allowedFormat(const QByteArray &format)
{
    static const QList<QByteArray> formats = {"png", "jpg", "jpeg", "webp", "gif"};
    return formats.contains(format.toLower());
}

} // namespace

RemoteImage::RemoteImage(QObject *parent)
    : QObject(parent)
{
}

RemoteImage::~RemoteImage()
{
    if (m_reply)
        m_reply->abort();
}

QString RemoteImage::cacheDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/images");
}

QString RemoteImage::cachePath() const
{
    const QByteArray hash = QCryptographicHash::hash(m_source.toString(QUrl::FullyEncoded).toUtf8(), QCryptographicHash::Sha256);
    return cacheDirectory() + QLatin1Char('/') + QString::fromLatin1(hash.toHex().left(40));
}

void RemoteImage::setSource(const QUrl &source)
{
    if (source == m_source)
        return;
    m_source = source;
    Q_EMIT sourceChanged();
    update();
}

void RemoteImage::setActive(bool active)
{
    if (active == m_active)
        return;
    m_active = active;
    Q_EMIT activeChanged();
    if (m_active && m_status == QLatin1String("idle"))
        update();
}

void RemoteImage::retry()
{
    if (m_status == QLatin1String("error"))
        update();
}

void RemoteImage::setStatus(const QString &status)
{
    m_status = status;
    Q_EMIT statusChanged();
}

void RemoteImage::update()
{
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply.clear();
    }
    m_localSource.clear();
    m_error.clear();

    if (m_source.isEmpty()) {
        setStatus(QStringLiteral("idle"));
        return;
    }
    const QString scheme = m_source.scheme();
    if (!m_source.isValid() || scheme != QLatin1String("https") || m_source.host().isEmpty()) {
        // Pictures are only ever fetched over HTTPS.
        fail(tr("Not an HTTPS address, not downloaded: %1").arg(m_source.toString()));
        return;
    }
    // Already downloaded?
    const QString path = cachePath();
    if (QFileInfo::exists(path) && QFileInfo(path).size() > 0) {
        m_localSource = QUrl::fromLocalFile(path);
        setStatus(QStringLiteral("ready"));
        return;
    }
    if (!m_active) {
        setStatus(QStringLiteral("idle"));
        return;
    }
    fetch();
}

void RemoteImage::fetch()
{
    setStatus(QStringLiteral("loading"));
    QNetworkRequest request(m_source);
    request.setRawHeader("Accept", "image/png, image/jpeg, image/webp, image/gif");
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("nexus-store/%1").arg(QCoreApplication::applicationVersion()));
    request.setMaximumRedirectsAllowed(5);
    QNetworkReply *reply = network()->get(request);
    m_reply = reply;
    connect(reply, &QNetworkReply::downloadProgress, this, [this, reply](qint64 received, qint64 total) {
        if (received > kMaxBytes || total > kMaxBytes) {
            reply->disconnect(this);
            reply->abort();
            reply->deleteLater();
            m_reply.clear();
            fail(tr("The picture is larger than %1 MiB and was not downloaded").arg(kMaxBytes / (1024 * 1024)));
        }
    });
    connect(reply, &QNetworkReply::finished, this, &RemoteImage::finish);
}

void RemoteImage::finish()
{
    QNetworkReply *reply = m_reply;
    if (!reply)
        return;
    m_reply.clear();
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        fail(reply->errorString());
        return;
    }
    const QByteArray data = reply->readAll();
    if (data.isEmpty()) {
        fail(tr("The server sent no data"));
        return;
    }
    if (data.size() > kMaxBytes) {
        fail(tr("The picture is larger than %1 MiB and was not kept").arg(kMaxBytes / (1024 * 1024)));
        return;
    }

    // Only keep what really is a picture in one of the expected formats.
    QBuffer buffer;
    buffer.setData(data);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    if (!reader.canRead() || !allowedFormat(reader.format())) {
        fail(tr("The server did not send a PNG, JPEG, WebP or GIF picture"));
        return;
    }
    const QSize size = reader.size();
    if (size.isValid() && (size.width() > 16384 || size.height() > 16384)) {
        fail(tr("The picture is too large to display (%1 × %2 pixels)").arg(size.width()).arg(size.height()));
        return;
    }

    const QString path = cachePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        fail(tr("The picture could not be stored in %1: %2").arg(cacheDirectory(), file.errorString()));
        return;
    }
    m_localSource = QUrl::fromLocalFile(path);
    setStatus(QStringLiteral("ready"));
}

void RemoteImage::fail(const QString &message)
{
    m_localSource.clear();
    m_error = message;
    setStatus(QStringLiteral("error"));
}
