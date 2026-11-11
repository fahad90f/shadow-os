// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QString>
#include <QUrl>

class QNetworkReply;

// A picture that lives on a web server — an AppStream screenshot or the icon
// of an application that is not installed — fetched lazily and kept in the
// user's cache directory.
//
//   RemoteImage { id: shot; source: modelData.thumbnail; active: visible }
//   Image { source: shot.localSource; visible: shot.status === "ready" }
//   // … and a placeholder that says why while status is "loading" or "error"
//
// Nothing is requested until 'active' is true. Only https URLs are fetched
// (redirects may not leave https),
// only PNG/JPEG/WebP/GIF data that decodes as an image is accepted (no SVG),
// downloads are limited in size and time, and a failed download is a state
// with a message — never a broken image and never a substitute picture.
class RemoteImage : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QUrl localSource READ localSource NOTIFY statusChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY statusChanged)

public:
    explicit RemoteImage(QObject *parent = nullptr);
    ~RemoteImage() override;

    QUrl source() const { return m_source; }
    void setSource(const QUrl &source);
    bool active() const { return m_active; }
    void setActive(bool active);
    // "idle" | "loading" | "ready" | "error"
    QString status() const { return m_status; }
    // file:// URL of the cached picture once status is "ready"
    QUrl localSource() const { return m_localSource; }
    QString errorString() const { return m_error; }

    Q_INVOKABLE void retry();

    static QString cacheDirectory();

Q_SIGNALS:
    void sourceChanged();
    void activeChanged();
    void statusChanged();

private:
    void update();
    void fetch();
    void finish();
    void fail(const QString &message);
    void setStatus(const QString &status);
    QString cachePath() const;

    QUrl m_source;
    bool m_active = false;
    QString m_status = QStringLiteral("idle");
    QUrl m_localSource;
    QString m_error;
    QPointer<QNetworkReply> m_reply;
};
