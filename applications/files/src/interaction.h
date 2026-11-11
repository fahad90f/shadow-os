// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <KIO/AskUserActionInterface>
#include <KIO/JobUiDelegateFactory>
#include <KJobUiDelegate>

#include <QList>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QVariantMap>

#include <functional>

// Broker between KIO jobs that need a decision from the user (name conflicts,
// errors that can be skipped, confirmations, worker message boxes) and the QML
// dialogs of the window.
//
// KIO asks through KIO::AskUserActionInterface; each job owns an
// AskUserHandler (below) that turns the question into a request map, queues it
// here and answers the job once QML calls answer(). Requests are shown one at
// a time; `current` is the request the dialog has to display.
class Interaction : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(bool pending READ pending NOTIFY currentChanged)
    Q_PROPERTY(QVariantMap current READ current NOTIFY currentChanged)

public:
    using Callback = std::function<void(const QString &action, bool applyToAll, const QString &text)>;

    // Dynamic property set on a KIO job that the user cancelled (operations
    // list, "Stop" in a dialog) — to tell it apart from a job KIO gave up on.
    static constexpr const char *cancelledProperty = "shadowCancelledByUser";

    static Interaction *instance();
    static Interaction *create(QQmlEngine *, QJSEngine *);

    bool pending() const { return !m_queue.isEmpty(); }
    QVariantMap current() const;

    // Queue a question. `owner` is the object the answer goes to; its requests
    // are dropped when it is destroyed.
    void ask(QObject *owner, QVariantMap request, Callback callback);
    void forget(QObject *owner);

    // Answer the current request (called by the QML dialog).
    //   conflict: "overwrite" | "skip" | "rename" (text = new name) | "keepBoth" | "cancel"
    //   skip:     "skip" | "retry" | "cancel"
    //   message:  "primary" | "secondary" | "cancel"
    //   delete:   "confirm" | "cancel"
    Q_INVOKABLE void answer(int id, const QString &action, bool applyToAll = false, const QString &text = QString());

    // Errors and notices from any back-end; the window shows them as toasts.
    void reportError(const QString &title, const QString &message);
    void reportNotice(const QString &message, const QString &kind = QStringLiteral("info"));

Q_SIGNALS:
    void currentChanged();
    void errorOccurred(const QString &title, const QString &message);
    void notice(const QString &message, const QString &kind);

private:
    explicit Interaction(QObject *parent = nullptr);

    struct Request {
        int id = 0;
        QPointer<QObject> owner;
        QVariantMap map;
        Callback callback;
    };
    QList<Request> m_queue;
    int m_nextId = 1;
};

// KIO::AskUserActionInterface implemented with the QML dialogs.
class AskUserHandler : public KIO::AskUserActionInterface
{
    Q_OBJECT
public:
    explicit AskUserHandler(QObject *parent = nullptr);
    ~AskUserHandler() override;

    void askUserRename(KJob *job, const QString &title, const QUrl &src, const QUrl &dest,
                       KIO::RenameDialog_Options options, KIO::filesize_t sizeSrc, KIO::filesize_t sizeDest,
                       const QDateTime &ctimeSrc, const QDateTime &ctimeDest,
                       const QDateTime &mtimeSrc, const QDateTime &mtimeDest) override;
    void askUserSkip(KJob *job, KIO::SkipDialog_Options options, const QString &errorText) override;
    void askUserDelete(const QList<QUrl> &urls, DeletionType deletionType, ConfirmationType confirmationType,
                       QWidget *parent) override;
    void requestUserMessageBox(MessageDialogType type, const QString &text, const QString &title,
                               const QString &primaryActionText, const QString &secondatyActionText,
                               const QString &primaryActionIconName, const QString &secondatyActionIconName,
                               const QString &dontAskAgainName, const QString &details, QWidget *parent) override;
    void askIgnoreSslErrors(const QVariantMap &sslErrorData, QWidget *parent) override;
};

// UI delegate attached to every KIO job of this process: questions go to the
// QML dialogs, error messages to the window's toaster.
class JobUiDelegate : public KJobUiDelegate
{
    Q_OBJECT
public:
    explicit JobUiDelegate(KJobUiDelegate::Flags flags = AutoHandlingDisabled);
    void showErrorMessage() override;
};

class JobUiDelegateFactory : public KIO::JobUiDelegateFactory
{
public:
    JobUiDelegateFactory() = default;
    KJobUiDelegate *createDelegate() const override;
    KJobUiDelegate *createDelegate(KJobUiDelegate::Flags flags, QWidget *window) const override;

    // Replace the widget based delegate KIOWidgets installs when it is loaded.
    static void install();
};
