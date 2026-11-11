// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <KJobTrackerInterface>

#include <QAbstractListModel>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QSet>
#include <QUrl>

#include <functional>

class KJob;
namespace KIO {
class Job;
}

// The running and recently failed file operations shown by the operations
// indicator. KIO jobs are added by OperationsTracker (registered as the KIO
// job tracker), other long-running work (bsdtar) through addExternal().
class OperationsModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int activeCount READ activeCount NOTIFY activeCountChanged)
    // Mean progress of the running operations, 0..1; -1 while none of them
    // knows its total yet.
    Q_PROPERTY(double overallProgress READ overallProgress NOTIFY progressChanged)

public:
    enum Roles { IdRole = Qt::UserRole + 1, TitleRole, DetailRole, ProgressRole, StateRole, ErrorRole, CancellableRole };

    explicit OperationsModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return int(m_ops.size()); }
    int activeCount() const;
    double overallProgress() const;

    Q_INVOKABLE void cancel(int id);
    Q_INVOKABLE void dismiss(int id);
    Q_INVOKABLE void clearFinished();

    // KIO jobs
    void addJob(KJob *job);
    void removeJob(KJob *job);
    void setJobTitle(KJob *job, const QString &title);
    void setJobDetail(KJob *job, const QString &detail);
    void setJobAmounts(KJob *job, qulonglong processed, qulonglong total);
    void setJobPercent(KJob *job, unsigned long percent);
    void setJobSpeed(KJob *job, unsigned long bytesPerSecond);
    void finishJob(KJob *job);

    // Other operations; returns the operation id.
    int addExternal(const QString &title, std::function<void()> cancel);
    void setExternalDetail(int id, const QString &detail);
    void finishExternal(int id, bool ok, const QString &error);

Q_SIGNALS:
    void countChanged();
    void activeCountChanged();
    void progressChanged();

private:
    struct Op {
        int id = 0;
        QPointer<KJob> job;
        QString title;
        QString detail;
        QString amounts;
        double progress = -1;
        qulonglong processed = 0;
        qulonglong total = 0;
        unsigned long speed = 0;
        QString state = QStringLiteral("running");   // running | error
        QString error;
        std::function<void()> cancel;
    };

    int rowOfJob(const KJob *job) const;
    int rowOfId(int id) const;
    void changed(int row, const QList<int> &roles);
    void removeRow(int row);
    void fail(int row, const QString &error);
    static QString amountText(const Op &op);

    QList<Op> m_ops;
    int m_nextId = 1;
};

// Receives every KIO job that is created without KIO::HideProgressInfo and
// mirrors its progress into the OperationsModel.
class OperationsTracker : public KJobTrackerInterface
{
    Q_OBJECT
public:
    explicit OperationsTracker(OperationsModel *model, QObject *parent = nullptr);

    void registerJob(KJob *job) override;
    void unregisterJob(KJob *job) override;

private:
    OperationsModel *m_model;
};

// File operations of the application, all executed as KIO jobs: copy, move,
// rename, trash, delete, restore, new folder / file, duplicate; the clipboard
// (cut / copy / paste), drops, and undo through KIO::FileUndoManager.
class FileOperations : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(FileOps)
    QML_SINGLETON
    Q_PROPERTY(OperationsModel *jobs READ jobs CONSTANT)
    Q_PROPERTY(bool undoAvailable READ undoAvailable NOTIFY undoChanged)
    Q_PROPERTY(QString undoText READ undoText NOTIFY undoChanged)
    Q_PROPERTY(bool clipboardHasUrls READ clipboardHasUrls NOTIFY clipboardChanged)
    Q_PROPERTY(bool clipboardIsCut READ clipboardIsCut NOTIFY clipboardChanged)
    Q_PROPERTY(int clipboardCount READ clipboardCount NOTIFY clipboardChanged)
    // Bumped whenever the set of cut URLs changes (delegates re-evaluate isCut()).
    Q_PROPERTY(int cutRevision READ cutRevision NOTIFY clipboardChanged)

public:
    static FileOperations *instance();
    static FileOperations *create(QQmlEngine *, QJSEngine *);

    OperationsModel *jobs() const { return m_jobs; }
    bool undoAvailable() const;
    QString undoText() const;
    bool clipboardHasUrls() const { return !m_clipUrls.isEmpty(); }
    bool clipboardIsCut() const { return m_clipCut; }
    int clipboardCount() const { return int(m_clipUrls.size()); }
    int cutRevision() const { return m_cutRevision; }

    Q_INVOKABLE void copyTo(const QList<QUrl> &urls, const QUrl &destDir);
    Q_INVOKABLE void moveTo(const QList<QUrl> &urls, const QUrl &destDir);
    Q_INVOKABLE void linkTo(const QList<QUrl> &urls, const QUrl &destDir);
    Q_INVOKABLE void rename(const QUrl &url, const QString &newName);
    Q_INVOKABLE void trash(const QList<QUrl> &urls);
    Q_INVOKABLE void deletePermanently(const QList<QUrl> &urls);
    Q_INVOKABLE void restore(const QList<QUrl> &trashUrls);
    Q_INVOKABLE void emptyTrash();
    Q_INVOKABLE void createFolder(const QUrl &parentDir, const QString &name);
    Q_INVOKABLE void createFile(const QUrl &parentDir, const QString &name);
    Q_INVOKABLE void duplicate(const QList<QUrl> &urls);
    Q_INVOKABLE void undo();

    Q_INVOKABLE void copyToClipboard(const QList<QUrl> &urls, bool cut);
    Q_INVOKABLE void paste(const QUrl &destDir);
    Q_INVOKABLE bool isCut(const QUrl &url) const;

    // A drop of `urls` on `destDir`. Ctrl copies, Shift moves, Ctrl+Shift
    // links; without a modifier items are moved within one local file system
    // and copied otherwise. Returns false if nothing was started.
    Q_INVOKABLE bool drop(const QList<QUrl> &urls, const QUrl &destDir);
    // What drop() would do: "copy" | "move" | "link" | "trash" | "" (nothing).
    Q_INVOKABLE QString dropAction(const QList<QUrl> &urls, const QUrl &destDir) const;

    // text/uri-list payload for a drag.
    Q_INVOKABLE QString uriList(const QList<QUrl> &urls) const;
    // Local path, or the URL without password for remote locations.
    Q_INVOKABLE QString displayPath(const QUrl &url) const;
    // Same folder, ignoring a trailing slash.
    Q_INVOKABLE bool sameLocation(const QUrl &a, const QUrl &b) const;

    // Empty string if `name` is a valid file name, otherwise the reason.
    Q_INVOKABLE QString validateName(const QString &name) const;
    Q_INVOKABLE QString suggestName(const QUrl &dir, const QString &name) const;

Q_SIGNALS:
    void undoChanged();
    void clipboardChanged();
    // Items appeared in `dir` as the result of an operation started here; the
    // pane showing that folder selects them.
    void itemsCreated(const QUrl &dir, const QList<QUrl> &urls);
    // A file operation (or an undo) has ended, successfully or not. Views of
    // locations that have no file system watcher (remote folders, the Trash)
    // refresh themselves on this.
    void operationFinished();

private:
    explicit FileOperations(QObject *parent = nullptr);
    void readClipboard();
    void watchResult(KJob *job, const QString &failureTitle);
    void startCopy(const QList<QUrl> &urls, const QUrl &destDir, bool move, bool autoRename, bool clearClipboardWhenDone);

    OperationsModel *m_jobs;
    QList<QUrl> m_clipUrls;
    QSet<QUrl> m_cutSet;
    bool m_clipCut = false;
    int m_cutRevision = 0;
};
