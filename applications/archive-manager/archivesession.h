// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "formats.h"
#include "listing.h"
#include "safemerge.h"

#include <QHash>
#include <QObject>
#include <QProcess>
#include <QQmlEngine>
#include <QTemporaryDir>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <atomic>
#include <functional>
#include <memory>

// One archive and the operations on it. All work is done by bsdtar and 7z,
// run with argument vectors; this class builds the commands, parses their
// output, enforces the extraction rules (safemerge.h) and reports progress.
//
// Passwords are kept in memory for the lifetime of the open archive and are
// given to 7z on its standard input, never on a command line.
class ArchiveSession : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString archivePath READ archivePath NOTIFY archiveChanged)
    Q_PROPERTY(QString archiveName READ archiveName NOTIFY archiveChanged)
    Q_PROPERTY(bool isOpen READ isOpen NOTIFY archiveChanged)
    Q_PROPERTY(QString formatId READ formatId NOTIFY archiveChanged)
    Q_PROPERTY(QString formatLabel READ formatLabel NOTIFY archiveChanged)
    Q_PROPERTY(bool canModify READ canModify NOTIFY archiveChanged)
    Q_PROPERTY(QString readOnlyReason READ readOnlyReason NOTIFY archiveChanged)
    Q_PROPERTY(bool encrypted READ encrypted NOTIFY contentsChanged)
    Q_PROPERTY(int entryCount READ entryCount NOTIFY contentsChanged)
    Q_PROPERTY(qreal totalSize READ totalSize NOTIFY contentsChanged)
    Q_PROPERTY(qreal archiveSize READ archiveSize NOTIFY contentsChanged)
    Q_PROPERTY(int hazardCount READ hazardCount NOTIFY contentsChanged)
    Q_PROPERTY(bool bombSuspected READ bombSuspected NOTIFY contentsChanged)
    Q_PROPERTY(QString listWarning READ listWarning NOTIFY contentsChanged)
    Q_PROPERTY(int rootItemCount READ rootItemCount NOTIFY contentsChanged)
    Q_PROPERTY(QString suggestedFolderName READ suggestedFolderName NOTIFY archiveChanged)
    Q_PROPERTY(QString currentDir READ currentDir NOTIFY currentDirChanged)
    Q_PROPERTY(QVariantList entries READ entries NOTIFY entriesChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString jobKind READ jobKind NOTIFY busyChanged)
    Q_PROPERTY(qreal progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString progressText READ progressText NOTIFY progressChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)

public:
    explicit ArchiveSession(QObject *parent = nullptr);
    ~ArchiveSession() override;

    QString archivePath() const { return m_path; }
    QString archiveName() const;
    bool isOpen() const { return m_open; }
    QString formatId() const { return m_format.id; }
    QString formatLabel() const { return m_format.label; }
    bool canModify() const;
    QString readOnlyReason() const;
    bool encrypted() const { return m_encrypted; }
    int entryCount() const { return int(m_entries.size()); }
    qreal totalSize() const { return qreal(m_totalSize); }
    qreal archiveSize() const { return qreal(m_archiveSize); }
    int hazardCount() const { return m_hazardCount; }
    bool bombSuspected() const;
    QString listWarning() const { return m_listWarning; }
    int rootItemCount() const;
    QString suggestedFolderName() const;
    QString currentDir() const { return m_currentDir; }
    QVariantList entries() const { return m_view; }
    bool busy() const { return m_busy; }
    QString jobKind() const { return m_jobKind; }
    qreal progress() const { return m_progress; }
    QString progressText() const { return m_progressText; }
    QString error() const { return m_error; }

    Q_INVOKABLE void open(const QString &path);
    Q_INVOKABLE void close();
    Q_INVOKABLE void reload();
    Q_INVOKABLE void enter(const QString &treePath);
    Q_INVOKABLE void goUp();
    Q_INVOKABLE void sortBy(const QString &role, bool ascending);

    // After passwordRequired(): continue the interrupted operation, or drop it.
    Q_INVOKABLE void submitPassword(const QString &password);
    Q_INVOKABLE void cancelPassword();

    // { entries, expanded, free, enough, ratio, suspicious, hazards, encrypted }
    Q_INVOKABLE QVariantMap extractionEstimate(const QStringList &selection, const QString &destination) const;
    // selection: tree paths in the current folder; empty = whole archive.
    // options: { conflict: "skip"|"overwrite"|"rename" }
    Q_INVOKABLE void extract(const QString &destination, const QStringList &selection, const QVariantMap &options);
    Q_INVOKABLE void test();
    Q_INVOKABLE void addFiles(const QStringList &paths);
    Q_INVOKABLE void deleteEntries(const QStringList &selection);
    Q_INVOKABLE void preview(const QString &treePath);
    // options: { format, level: "store"|"fast"|"normal"|"best", password, encryptNames }
    Q_INVOKABLE void create(const QString &output, const QStringList &inputs, const QVariantMap &options);
    Q_INVOKABLE void cancel();

    QJsonArray toJson() const;      // full listing, for --list

Q_SIGNALS:
    void archiveChanged();
    void contentsChanged();
    void currentDirChanged();
    void entriesChanged();
    void busyChanged();
    void progressChanged();
    void errorChanged();
    void listed();
    void passwordRequired(bool wrong);
    // kind: "list" | "extract" | "test" | "add" | "delete" | "preview" | "create"
    void finished(const QString &kind, bool ok, const QString &message, const QStringList &details);

private:
    struct Node
    {
        QString name;
        int parent = -1;
        int entry = -1;             // index into m_entries; -1: folder implied by a path
        bool directory = false;
        QString rawPath;
        QHash<QString, int> children;
        qint64 size = 0;            // recursive
        int count = 0;              // entries below (and including) this node
        bool hazard = false;        // recursive
    };
    struct ToolRun
    {
        QString program;
        QStringList arguments;
        QByteArray stdinData;
        bool discardStdout = false;
        std::function<void(const QByteArray &)> onStdout;
        std::function<void(const QByteArray &)> onStderr;
        std::function<void(int exitCode)> done;     // -1: could not start / crashed
    };
    struct ExtractRequest
    {
        QString destination;
        QStringList selection;
        QString subPath;
        SafeMerge::Conflict conflict = SafeMerge::Conflict::Rename;
        bool preview = false;
        QString previewName;
    };

    void setBusy(const QString &kind);
    void setIdle();
    void setProgress(qreal value, const QString &text = QString());
    void setError(const QString &error);
    void finish(const QString &kind, bool ok, const QString &message, const QStringList &details = {});
    void startTool(const ToolRun &run);
    bool passwordProblem(const QByteArray &out, const QByteArray &err, bool *wrong) const;
    void askPassword(bool wrong);
    QByteArray passwordInput(int repeat = 1) const;
    void parseSevenZipProgress(const QByteArray &chunk);

    void runList();
    void buildTree();
    void rebuildView();
    int nodeFor(const QString &treePath) const;
    QString treePath(int node) const;
    QStringList rawPathsFor(const QStringList &selection, qint64 *size, int *count) const;

    void runExtract(const ExtractRequest &request);
    void finishExtract(const ExtractRequest &request, const QStringList &toolWarnings);
    void runTest();
    void runDelete(const QStringList &selection);
    void runAdd(const QStringList &paths);
    void runCreate(const QString &output, const QStringList &inputs, const QVariantMap &options);
    void replaceArchive(const QString &temporary, const QString &kind, const QString &message);
    QStringList tarWriteOptions(const Formats::Format &format, const QString &level) const;
    static QStringList tarInputArguments(const QStringList &paths);

    QString m_path;
    Formats::Format m_format;
    bool m_open = false;
    bool m_encrypted = false;
    bool m_headerEncrypted = false;
    QString m_password;
    QList<Listing::Entry> m_entries;
    QList<Node> m_nodes;
    qint64 m_totalSize = 0;
    qint64 m_archiveSize = 0;
    int m_hazardCount = 0;
    QString m_listWarning;
    QString m_currentDir;
    QVariantList m_view;
    QString m_sortRole = QStringLiteral("name");
    bool m_sortAscending = true;

    bool m_busy = false;
    QString m_jobKind;
    qreal m_progress = -1;
    QString m_progressText;
    QString m_error;

    QProcess *m_process = nullptr;
    QByteArray m_out, m_err;
    bool m_cancelRequested = false;
    std::shared_ptr<std::atomic_bool> m_cancelFlag;
    std::function<void()> m_retry;
    std::unique_ptr<QTemporaryDir> m_stage;
    std::unique_ptr<QTemporaryDir> m_previewDir;
    int m_previewSerial = 0;
    QTimer m_guard;
    qint64 m_guardStartFree = 0;
    qint64 m_guardExpected = 0;
    QString m_guardPath;
    QString m_guardAbort;
    int m_progressDone = 0;
    int m_progressTotal = 0;
    QString m_temporaryOutput;
};
