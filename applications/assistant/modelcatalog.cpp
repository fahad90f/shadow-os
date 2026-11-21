// SPDX-License-Identifier: GPL-3.0-or-later
#include "modelcatalog.h"

#include "assistantconfig.h"

#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

namespace {
constexpr int kListTimeoutMs = 20000;
constexpr int kCheckTimeoutMs = 30000;
// How long a learnt "cannot use tools / pictures" is believed.
constexpr qint64 kLearntSecs = 7 * 24 * 3600;

QString apiMessage(const QByteArray &body)
{
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (doc.isObject()) {
        const QJsonObject o = doc.object();
        const QJsonValue e = o.value(QStringLiteral("error"));
        if (e.isObject())
            return e.toObject().value(QStringLiteral("message")).toString();
        if (e.isString())
            return e.toString();
        for (const char *k : {"detail", "message", "title"}) {
            const QString v = o.value(QLatin1String(k)).toString();
            if (!v.isEmpty())
                return v;
        }
    }
    // The service answers some errors with a bare sentence.
    const QString text = QString::fromUtf8(body).trimmed();
    return text.size() <= 200 && !text.startsWith(u'<') ? text : QString();
}
} // namespace

ModelCatalog::ModelCatalog(AssistantConfig *config, QObject *parent)
    : QObject(parent), m_config(config)
{
    m_net = new QNetworkAccessManager(this);
    m_net->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
    loadCache();
    m_lastKeyHint = m_config->apiKeyHint();
    connect(m_config, &AssistantConfig::changed, this, &ModelCatalog::onConfigChanged);
    // The list is read once per start, a moment after it (never in the way of
    // the window appearing or of a command-line dump).
    if (qEnvironmentVariableIsEmpty("SHADOW_ASSISTANT_NO_MODEL_REFRESH"))
        QTimer::singleShot(0, this, &ModelCatalog::refresh);
}

QString ModelCatalog::shortName(const QString &id)
{
    const qsizetype slash = id.lastIndexOf(u'/');
    return slash < 0 ? id : id.mid(slash + 1);
}

QString ModelCatalog::publisher(const QString &id)
{
    const qsizetype slash = id.indexOf(u'/');
    return slash < 0 ? QString() : id.left(slash);
}

QString ModelCatalog::cachePath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
        + QStringLiteral("/shadow/assistant-models.json");
}

void ModelCatalog::loadCache()
{
    QFile f(cachePath());
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    // The list and what was learnt describe one endpoint.
    if (o.value(QStringLiteral("baseUrl")).toString() != m_config->baseUrl())
        return;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    auto learnt = [&](const QString &key, QHash<QString, qint64> *into) {
        const QJsonObject entries = o.value(key).toObject();
        for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
            const qint64 when = qint64(it.value().toDouble());
            if (when > 0 && now - when < kLearntSecs)
                into->insert(it.key(), when);
        }
    };
    learnt(QStringLiteral("noTools"), &m_noTools);
    learnt(QStringLiteral("noImages"), &m_noImages);
    m_ids.clear();
    for (const QJsonValue &v : o.value(QStringLiteral("ids")).toArray())
        m_ids << v.toString();
    m_idSet = QSet<QString>(m_ids.cbegin(), m_ids.cend());
    m_known = !m_ids.isEmpty();
    m_listUrl = m_config->baseUrl();
}

void ModelCatalog::saveCache() const
{
    const QString path = cachePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    auto learnt = [](const QHash<QString, qint64> &from) {
        QJsonObject entries;
        for (auto it = from.constBegin(); it != from.constEnd(); ++it)
            entries.insert(it.key(), double(it.value()));
        return entries;
    };
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return;
    f.write(QJsonDocument(QJsonObject{
        {QStringLiteral("baseUrl"), m_listUrl.isEmpty() ? m_config->baseUrl() : m_listUrl},
        {QStringLiteral("ids"), QJsonArray::fromStringList(m_ids)},
        {QStringLiteral("noTools"), learnt(m_noTools)},
        {QStringLiteral("noImages"), learnt(m_noImages)},
    }).toJson(QJsonDocument::Indented));
    f.commit();
}

void ModelCatalog::onConfigChanged()
{
    // Another service, endpoint or key: what was read no longer describes it.
    const bool otherEndpoint = m_known && m_listUrl != m_config->baseUrl();
    const bool otherKey = m_lastKeyHint != m_config->apiKeyHint();
    m_lastKeyHint = m_config->apiKeyHint();
    if (otherEndpoint || otherKey)
        m_keyState.clear(); // what was said about the previous key
    if (otherEndpoint) {
        m_known = false;
        m_ids.clear();
        m_idSet.clear();
        m_noTools.clear(); // learnt from the other endpoint's answers
        m_noImages.clear();
    }
    if (otherEndpoint || otherKey || (!m_known && m_config->nvidia()))
        refresh();
    Q_EMIT changed(); // the groups shown in Settings follow the configuration
}

void ModelCatalog::applyList(const QByteArray &body)
{
    const QJsonArray data = QJsonDocument::fromJson(body).object().value(QStringLiteral("data")).toArray();
    QStringList ids;
    for (const QJsonValue &v : data) {
        const QString id = v.toObject().value(QStringLiteral("id")).toString();
        if (!id.isEmpty())
            ids << id;
    }
    if (ids.isEmpty()) {
        m_error = tr("The service returned no model list.");
        return;
    }
    ids.sort();
    m_ids = ids;
    m_idSet = QSet<QString>(ids.cbegin(), ids.cend());
    m_known = true;
    m_listUrl = m_config->baseUrl();
    m_error.clear();
    saveCache();
}

void ModelCatalog::refresh()
{
    if (!m_config->nvidia() || m_refresh)
        return;
    QNetworkRequest req(QUrl(m_config->baseUrl() + QStringLiteral("/models")));
    req.setRawHeader("Accept", "application/json");
    req.setTransferTimeout(kListTimeoutMs);
    if (!m_config->apiKey().isEmpty())
        req.setRawHeader("Authorization", "Bearer " + m_config->apiKey().toUtf8());
    m_refresh = m_net->get(req);
    Q_EMIT changed();
    connect(m_refresh, &QNetworkReply::finished, this, [this]() {
        QNetworkReply *reply = m_refresh;
        m_refresh = nullptr;
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() == QNetworkReply::NoError && status == 200) {
            applyList(reply->readAll());
        } else if (status != 0) {
            m_error = tr("The model list could not be read (HTTP %1).").arg(status);
        } else {
            m_error = tr("The model list could not be read: %1").arg(reply->errorString());
        }
        Q_EMIT changed();
    });
}

bool ModelCatalog::refreshSync(int timeoutMs)
{
    refresh();
    if (!m_refresh)
        return m_known;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(this, &ModelCatalog::changed, &loop, [this, &loop]() {
        if (!m_refresh)
            loop.quit();
    });
    timer.start(timeoutMs);
    loop.exec();
    return m_known && m_error.isEmpty();
}

void ModelCatalog::checkKey()
{
    // A check still in flight was for the key in use when it started; its
    // answer must not be reported for the one in use now.
    if (m_check) {
        QNetworkReply *old = m_check;
        m_check = nullptr;
        old->disconnect(this);
        old->abort();
        old->deleteLater();
    }
    if (m_config->apiKey().isEmpty()) {
        Q_EMIT keyChecked(false, tr("No key is set."));
        return;
    }
    if (!m_config->nvidia()) {
        Q_EMIT keyChecked(true, tr("The key is used with %1; it is tried with your next message.")
                                    .arg(m_config->providerName()));
        return;
    }
    // The model list is public, so it says nothing about the key. One reply
    // of a single token from a small model does.
    QStringList candidates = members(QStringLiteral("chat"));
    const QString small = QStringLiteral("nvidia/nemotron-3.5-bolt-e4b");
    if (candidates.contains(small))
        candidates.move(candidates.indexOf(small), 0);
    const QString model = candidates.value(0, small);
    QNetworkRequest req(QUrl(m_config->baseUrl() + QStringLiteral("/chat/completions")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Authorization", "Bearer " + m_config->apiKey().toUtf8());
    req.setTransferTimeout(kCheckTimeoutMs);
    const QJsonObject body{
        {QStringLiteral("model"), model},
        {QStringLiteral("messages"), QJsonArray{QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                                            {QStringLiteral("content"), QStringLiteral("Say OK.")}}}},
        {QStringLiteral("max_tokens"), 4},
        {QStringLiteral("stream"), false},
    };
    m_check = m_net->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    Q_EMIT changed();
    QNetworkReply *started = m_check;
    connect(m_check, &QNetworkReply::finished, this, [this, model, started]() {
        if (m_check != started)
            return; // superseded
        QNetworkReply *reply = m_check;
        m_check = nullptr;
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString detail = apiMessage(reply->readAll());
        bool ok = false;
        QString message;
        if (status == 200) {
            ok = true;
            message = m_known ? tr("The key works. NVIDIA lists %1 models.").arg(m_ids.size())
                              : tr("The key works.");
        } else if (status == 401 || status == 403) {
            message = tr("NVIDIA did not accept this key (HTTP %1). Create a new one at build.nvidia.com and paste it again.")
                          .arg(status);
        } else if (status == 402) {
            message = tr("The key is valid but its free credits are used up (HTTP 402).");
        } else if (status == 429) {
            ok = true; // it was recognised: only a known key is rate limited
            message = tr("The key works, but NVIDIA is limiting requests right now (HTTP 429). Try again in a minute.");
        } else if (status == 404 || status == 400 || status == 422) {
            ok = true; // the key got past the door; the test model did not
            message = tr("The key was accepted, but the test model %1 was not (HTTP %2).").arg(shortName(model)).arg(status);
        } else if (status != 0) {
            message = tr("NVIDIA answered HTTP %1.").arg(status);
        } else {
            message = tr("NVIDIA could not be reached: %1").arg(reply->errorString());
        }
        if (!ok && !detail.isEmpty())
            message += QStringLiteral(" (") + detail + QStringLiteral(")");
        m_keyState = ok ? QStringLiteral("ok")
                   : (status == 401 || status == 402 || status == 403) ? QStringLiteral("rejected") : QString();
        Q_EMIT changed();
        Q_EMIT keyChecked(ok, message);
    });
}

void ModelCatalog::noteKey(bool accepted)
{
    const QString state = accepted ? QStringLiteral("ok") : QStringLiteral("rejected");
    if (state == m_keyState)
        return;
    m_keyState = state;
    Q_EMIT changed();
}

bool ModelCatalog::available(const QString &id) const
{
    return !m_known || m_idSet.contains(id);
}

void ModelCatalog::learnNoTools(const QString &id)
{
    if (id.isEmpty() || m_noTools.contains(id))
        return;
    m_noTools.insert(id, QDateTime::currentSecsSinceEpoch());
    saveCache();
    Q_EMIT changed();
}

void ModelCatalog::learnNoImages(const QString &id)
{
    if (id.isEmpty() || m_noImages.contains(id))
        return;
    m_noImages.insert(id, QDateTime::currentSecsSinceEpoch());
    saveCache();
    Q_EMIT changed();
}

QStringList ModelCatalog::members(const QString &kind) const
{
    QStringList out;
    for (const QString &id : m_config->teamGroup(kind)) {
        if (!available(id) || out.contains(id))
            continue;
        if (kind == QLatin1String("vision") && !supportsImages(id))
            continue;
        out << id;
    }
    return out;
}

QString ModelCatalog::lead(const QString &kind, const QStringList &exclude) const
{
    for (const QString &id : m_config->teamLeads(kind)) {
        if (available(id) && supportsTools(id) && !exclude.contains(id))
            return id;
    }
    // Every candidate is marked as unable to use tools. That mark is a
    // conclusion from an error's wording: it must not leave a kind of request
    // without a lead, so the candidates are tried again.
    for (const QString &id : m_config->teamLeads(kind)) {
        if (available(id) && !exclude.contains(id))
            return id;
    }
    return {};
}

void ModelCatalog::readAgain()
{
    if (!m_noTools.isEmpty() || !m_noImages.isEmpty()) {
        m_noTools.clear();
        m_noImages.clear();
        saveCache();
    }
    refresh();
    Q_EMIT changed();
}

QVariantList ModelCatalog::groups() const
{
    struct Kind { const char *kind; QString title; QString note; };
    const QList<Kind> kinds = {
        {"coding", tr("Code"), tr("writing, fixing and explaining code")},
        {"agents", tr("Actions"), tr("doing things on this computer with tools")},
        {"chat", tr("Conversation"), tr("questions and everyday talk")},
        {"vision", tr("Seeing"), tr("looking at the screen or an image")},
    };
    QVariantList out;
    for (const Kind &k : kinds) {
        const QString kind = QLatin1String(k.kind);
        const QString leadId = kind == QLatin1String("vision") ? QString() : lead(kind);
        QVariantList members;
        for (const QString &id : m_config->teamGroup(kind)) {
            const bool usable = available(id) && (kind != QLatin1String("vision") || supportsImages(id));
            members.append(QVariantMap{
                {QStringLiteral("id"), id},
                {QStringLiteral("name"), shortName(id)},
                {QStringLiteral("publisher"), publisher(id)},
                {QStringLiteral("available"), usable},
                {QStringLiteral("lead"), id == leadId},
            });
        }
        out.append(QVariantMap{
            {QStringLiteral("kind"), kind},
            {QStringLiteral("title"), k.title},
            {QStringLiteral("note"), k.note},
            {QStringLiteral("lead"), shortName(leadId)},
            {QStringLiteral("members"), members},
        });
    }
    return out;
}
