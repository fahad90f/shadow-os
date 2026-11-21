// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <functional>

class AssistantConfig;
class ChatClient;
class ModelCatalog;
class QNetworkAccessManager;
class QTimer;

// The vision models look at one picture at the same time (the look_at_image
// tool). Each gets the picture and the question and writes what it sees; the
// answers go back to the lead model as the tool's result, which combines them.
//
// The picture is made small first (longest side 1280 px, JPEG): a full-size
// screenshot would be megabytes of base64 in every request, and the models
// scale pictures down themselves anyway.
class VisionTeam : public QObject
{
    Q_OBJECT
public:
    VisionTeam(AssistantConfig *config, ModelCatalog *catalog, QObject *parent = nullptr);
    ~VisionTeam() override;

    // Whether there is anybody to ask (NVIDIA is the service and it offers a
    // vision model).
    bool usable() const;

    // 'ok' is false when the picture could not be read or no model answered;
    // 'text' is then the reason. 'done' is called exactly once for every
    // look(), also after cancel(). Looks asked for while one is running wait
    // their turn (a reply can ask for several pictures at once).
    using Done = std::function<void(bool ok, const QString &text)>;
    void look(const QString &imagePath, const QString &question, Done done);
    // Stop: the look in progress ends and the waiting ones are dropped; each
    // gets its answer ("stopped") and no picture is sent after this.
    void cancel();
    // Counts the Stops: work prepared before one (a screen capture still
    // running) compares it and does not send its picture afterwards.
    int epoch() const { return m_epoch; }

    // The picture as a "data:image/jpeg;base64,…" URL, small enough to send
    // ('maxBytes' of image data). "" (and 'error') when it cannot be read.
    static QByteArray encodeImage(const QString &path, int maxBytes, QString *error);

private:
    struct Member
    {
        QString id;
        QString state = QStringLiteral("working"); // "working" | "done" | "failed" | "late"
        QString text;
        QString error;
        ChatClient *client = nullptr;
    };
    struct Job
    {
        QString path;
        QString question;
        Done done;
    };
    // Start the job; false when it ended at once (its 'done' was called).
    bool start(const Job &job);
    void startNext();
    void endRequests();
    void finishIfComplete(int generation);

    AssistantConfig *m_config;
    ModelCatalog *m_catalog;
    QList<QNetworkAccessManager *> m_nets; // one connection pool per model, as in Council
    QTimer *m_deadline;
    QList<Member> m_members;
    QList<Job> m_queue;      // waiting for the one in progress
    Done m_done;             // of the one in progress
    bool m_running = false;
    int m_generation = 0;
    int m_epoch = 0;
};
