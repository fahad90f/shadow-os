// SPDX-License-Identifier: GPL-3.0-or-later
#include "visionteam.h"

#include "assistantconfig.h"
#include "chatclient.h"
#include "modelcatalog.h"

#include <QBuffer>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QPainter>
#include <QTimer>

namespace {
const int kMaxSide = 1280;          // longest side sent, in pixels
const int kMaxImageBytes = 180000;  // JPEG bytes sent (before base64)
const int kWaitMs = 40000;          // how long the slowest model may take
const int kMaxAnswerChars = 2500;   // per model, in the tool result
const int kMaxLookers = 4;          // models that look at one picture
}

VisionTeam::VisionTeam(AssistantConfig *config, ModelCatalog *catalog, QObject *parent)
    : QObject(parent), m_config(config), m_catalog(catalog), m_deadline(new QTimer(this))
{
    m_deadline->setSingleShot(true);
    connect(m_deadline, &QTimer::timeout, this, [this]() {
        for (Member &m : m_members) {
            if (m.state != QLatin1String("working"))
                continue;
            m.state = QStringLiteral("late");
            m.error = tr("took too long");
            if (m.client)
                m.client->cancel();
        }
        finishIfComplete(m_generation);
    });
}

VisionTeam::~VisionTeam()
{
    cancel(); // requests in flight end while the clients and their pools exist
}

bool VisionTeam::usable() const
{
    if (!m_config->nvidia() || m_config->localMode() || !m_config->apiKeyConfigured())
        return false;
    for (const QString &id : m_catalog->members(QStringLiteral("vision")))
        if (m_catalog->supportsImages(id))
            return true;
    return false;
}

QByteArray VisionTeam::encodeImage(const QString &path, int maxBytes, QString *error)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    // Never decode a huge picture at full size: ask the decoder for a smaller one.
    const QSize full = reader.size();
    if (full.isValid() && qMax(full.width(), full.height()) > kMaxSide)
        reader.setScaledSize(full.scaled(kMaxSide, kMaxSide, Qt::KeepAspectRatio));
    QImage image = reader.read();
    if (image.isNull()) {
        if (error)
            *error = tr("%1 could not be read as a picture: %2").arg(path, reader.errorString());
        return {};
    }
    if (qMax(image.width(), image.height()) > kMaxSide)
        image = image.scaled(kMaxSide, kMaxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    // JPEG has no transparency: put the picture on white.
    if (image.hasAlphaChannel()) {
        QImage flat(image.size(), QImage::Format_RGB32);
        flat.fill(Qt::white);
        QPainter painter(&flat);
        painter.drawImage(0, 0, image);
        painter.end();
        image = flat;
    } else {
        image = image.convertToFormat(QImage::Format_RGB32);
    }

    // Lower the quality, then the size, until the picture is small enough.
    QByteArray data;
    for (int attempt = 0; attempt < 8; ++attempt) {
        static const int qualities[] = {82, 70, 58, 46};
        for (int quality : qualities) {
            data.clear();
            QBuffer buffer(&data);
            buffer.open(QIODevice::WriteOnly);
            if (!image.save(&buffer, "JPEG", quality)) {
                if (error)
                    *error = tr("The picture could not be prepared for sending (no JPEG support).");
                return {};
            }
            if (data.size() <= maxBytes)
                return QByteArrayLiteral("data:image/jpeg;base64,") + data.toBase64();
        }
        image = image.scaled(image.width() * 3 / 4, image.height() * 3 / 4, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        if (image.width() < 64 || image.height() < 64)
            break;
    }
    if (error)
        *error = tr("The picture could not be made small enough to send.");
    return {};
}

void VisionTeam::cancel()
{
    ++m_generation;
    m_deadline->stop();
    for (Member &m : m_members) {
        if (m.client) {
            m.client->disconnect(this);
            m.client->cancel();
            m.client->deleteLater();
            m.client = nullptr;
        }
    }
    m_members.clear();
    m_done = nullptr;
}

void VisionTeam::look(const QString &imagePath, const QString &question, Done done)
{
    cancel();
    QStringList models;
    for (const QString &id : m_catalog->members(QStringLiteral("vision")))
        if (m_catalog->supportsImages(id) && models.size() < kMaxLookers)
            models << id;
    if (!m_config->nvidia() || m_config->localMode()) {
        done(false, tr("Looking at pictures needs the NVIDIA service."));
        return;
    }
    if (models.isEmpty()) {
        done(false, tr("The service offers no model that can look at pictures right now."));
        return;
    }
    QString error;
    const QByteArray url = encodeImage(imagePath, kMaxImageBytes, &error);
    if (url.isEmpty()) {
        done(false, error);
        return;
    }

    const QString ask = question.trimmed().isEmpty()
        ? tr("Describe what is in this picture: what it shows, any text in it (quote it), and anything unusual.")
        : question.trimmed();
    const QJsonArray messages{
        QJsonObject{{QStringLiteral("role"), QStringLiteral("system")},
                    {QStringLiteral("content"),
                     QStringLiteral("You look at a picture for an assistant that cannot see it. Answer the question from "
                                    "what is actually visible. Quote visible text exactly. Say so when something cannot "
                                    "be made out; never guess. Be specific and brief: no introduction, no closing remarks.")}},
        QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                    {QStringLiteral("content"),
                     QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), ask}},
                                QJsonObject{{QStringLiteral("type"), QStringLiteral("image_url")},
                                            {QStringLiteral("image_url"),
                                             QJsonObject{{QStringLiteral("url"), QString::fromLatin1(url)}}}}}}},
    };
    ChatClient::Options options;
    options.maxTokens = 700;
    options.temperature = 0.2;
    options.idleTimeoutMs = kWaitMs + 5000;

    m_done = std::move(done);
    const int generation = m_generation;
    for (const QString &id : std::as_const(models)) {
        Member member;
        member.id = id;
        while (m_nets.size() <= m_members.size())
            m_nets.append(new QNetworkAccessManager(this));
        member.client = new ChatClient(m_config, this, m_nets.at(m_members.size()));
        m_members.append(member);
    }
    for (int i = 0; i < m_members.size(); ++i) {
        ChatClient *client = m_members.at(i).client;
        connect(client, &ChatClient::completed, this,
                [this, i, generation](const QString &content, const QJsonArray &, const QJsonArray &, const QString &) {
            if (generation != m_generation || i >= m_members.size() || m_members.at(i).state != QLatin1String("working"))
                return;
            const QString text = content.trimmed();
            m_members[i].state = text.isEmpty() ? QStringLiteral("failed") : QStringLiteral("done");
            m_members[i].text = text.size() > kMaxAnswerChars ? text.left(kMaxAnswerChars) + QStringLiteral(" […]") : text;
            if (text.isEmpty())
                m_members[i].error = tr("an empty answer");
            finishIfComplete(generation);
        });
        connect(client, &ChatClient::failed, this, [this, i, generation](const QString &code, const QString &message) {
            if (generation != m_generation || i >= m_members.size() || m_members.at(i).state != QLatin1String("working"))
                return;
            // A model that turns out not to take pictures is not asked again.
            if (code == QLatin1String("no_images"))
                m_catalog->learnNoImages(m_members.at(i).id);
            m_members[i].state = QStringLiteral("failed");
            m_members[i].error = message.section(u'\n', 0, 0);
            finishIfComplete(generation);
        });
        client->send(m_members.at(i).id, messages, QJsonArray{}, options);
    }
    m_deadline->start(kWaitMs);
}

void VisionTeam::finishIfComplete(int generation)
{
    if (generation != m_generation || !m_done)
        return;
    for (const Member &m : std::as_const(m_members))
        if (m.state == QLatin1String("working"))
            return;
    m_deadline->stop();

    QStringList answers, problems;
    for (const Member &m : std::as_const(m_members)) {
        if (m.state == QLatin1String("done"))
            answers << QStringLiteral("[%1]\n%2").arg(ModelCatalog::shortName(m.id), m.text);
        else
            problems << QStringLiteral("%1: %2").arg(ModelCatalog::shortName(m.id), m.error);
    }
    const Done done = std::move(m_done);
    cancel();
    if (answers.isEmpty()) {
        done(false, tr("No vision model could look at the picture (%1).").arg(problems.join(QStringLiteral("; "))));
        return;
    }
    if (answers.size() == 1) {
        done(true, tr("One vision model looked at the picture.\n\n") + answers.first());
        return;
    }
    done(true, tr("%1 vision models looked at the picture at the same time. Where they disagree, trust what most of "
                  "them report and say what is uncertain.\n\n").arg(answers.size())
                   + answers.join(QStringLiteral("\n\n")));
}
