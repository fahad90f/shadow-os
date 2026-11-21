// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>

class AssistantConfig;
class QNetworkAccessManager;
class QNetworkReply;

// Which models the service offers right now, and what the assistant has
// learnt about them.
//
// NVIDIA's catalogue changes: models are added, renamed and retired (its own
// pages show "deprecation in 2 days" badges). The model team is therefore not
// a fixed list compiled into the program: the configured groups
// (AssistantConfig::teamGroup) are matched against what GET {base}/models
// returns, and a model the service does not list is left out. Until a list
// has been read (first start, no network) every configured model counts as
// available, so the assistant still tries.
//
// Two things cannot be read from the list and are learnt from use instead: a
// model that rejects a request with tools, or with an image, is remembered
// (noTools / noImages) so it is not asked that way again. The list and what
// was learnt are kept in ~/.cache/shadow/assistant-models.json.
class ModelCatalog : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("agent.catalog")
    // A model list has been read from the service (now or in an earlier run).
    Q_PROPERTY(bool known READ known NOTIFY changed)
    Q_PROPERTY(bool refreshing READ refreshing NOTIFY changed)
    // Why the last refresh failed; "" when it worked.
    Q_PROPERTY(QString error READ error NOTIFY changed)
    // How many models the service lists.
    Q_PROPERTY(int count READ count NOTIFY changed)
    // For Settings: [{kind, title, note, lead, members: [{id, name, publisher,
    // available, lead}]}] for "coding", "agents", "chat" and "vision".
    Q_PROPERTY(QVariantList groups READ groups NOTIFY changed)
    // The key is being tried (checkKey()).
    Q_PROPERTY(bool checkingKey READ checkingKey NOTIFY changed)
    // What trying the key in use said: "ok", "rejected", or "" when it has
    // not been tried (or the service could not be reached).
    Q_PROPERTY(QString keyState READ keyState NOTIFY changed)

public:
    explicit ModelCatalog(AssistantConfig *config, QObject *parent = nullptr);

    bool known() const { return m_known; }
    bool refreshing() const { return m_refresh != nullptr; }
    QString error() const { return m_error; }
    int count() const { return int(m_ids.size()); }
    QVariantList groups() const;
    bool checkingKey() const { return m_check != nullptr; }
    QString keyState() const { return m_keyState; }

    // Read the model list again (asynchronously). Does nothing unless the
    // service is NVIDIA.
    Q_INVOKABLE void refresh();
    // The same, waiting for the answer (command-line use). False on failure.
    bool refreshSync(int timeoutMs);
    // Try the key with one tiny request; keyChecked() carries the outcome.
    Q_INVOKABLE void checkKey();

    bool available(const QString &id) const;
    bool supportsTools(const QString &id) const { return !m_noTools.contains(id); }
    bool supportsImages(const QString &id) const { return !m_noImages.contains(id); }
    void learnNoTools(const QString &id);
    void learnNoImages(const QString &id);
    // A request said something about the key in use: it was accepted, or not.
    void noteKey(bool accepted);

    // The models of a group that can be used now, in the configured order.
    QStringList members(const QString &kind) const;
    // The model that leads a kind of request: the first preferred lead that is
    // available, can use tools and is not in 'exclude'. "" when there is none.
    QString lead(const QString &kind, const QStringList &exclude = {}) const;

    // "moonshotai/kimi-k3.1-pro" -> "kimi-k3.1-pro"; "…" -> "moonshotai".
    static QString shortName(const QString &id);
    static QString publisher(const QString &id);

Q_SIGNALS:
    void changed();
    // 'ok' is false when the service rejected the key or could not be reached;
    // 'message' says which, in words for the Settings page.
    void keyChecked(bool ok, const QString &message);

private:
    QString cachePath() const;
    void loadCache();
    void saveCache() const;
    void applyList(const QByteArray &body);
    void onConfigChanged();

    AssistantConfig *m_config;
    QNetworkAccessManager *m_net;
    QNetworkReply *m_refresh = nullptr;
    QNetworkReply *m_check = nullptr;
    bool m_known = false;
    QString m_error;
    QString m_listUrl;      // the base URL the list was read from
    QString m_lastKeyHint;  // refresh once when the key changes
    QString m_keyState;
    QStringList m_ids;
    QSet<QString> m_idSet;
    QSet<QString> m_noTools, m_noImages;
};
