// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QPointer>
#include <QQmlEngine>
#include <QUrl>

#include <atomic>
#include <memory>

class KJob;

// Recursive search by file name below a folder.
//
// Local folders are walked on a worker thread (std::filesystem), other
// locations through KIO::listRecursive() — neither blocks the UI, and both
// stop when cancel() is called. The query is a case-insensitive substring of
// the file name, or a wildcard pattern if it contains * or ?.
class SearchModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int scanned READ scanned NOTIFY scannedChanged)
    Q_PROPERTY(bool truncated READ truncated NOTIFY runningChanged)
    Q_PROPERTY(QString query READ query NOTIFY queryChanged)
    Q_PROPERTY(QUrl root READ root NOTIFY queryChanged)
    Q_PROPERTY(QString error READ error NOTIFY runningChanged)

public:
    enum Roles { NameRole = Qt::UserRole + 1, UrlRole, IsDirRole, FolderRole, IconNameRole, SizeRole, ModifiedTextRole };

    struct Hit {
        QString name;
        QUrl url;
        QString folder;     // containing folder, relative to the search root
        bool dir = false;
        qint64 size = -1;
        qint64 mtime = 0;   // seconds since the epoch, 0 if unknown
    };

    static constexpr int MaxResults = 5000;

    explicit SearchModel(QObject *parent = nullptr);
    ~SearchModel() override;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool running() const { return m_running; }
    int count() const { return int(m_hits.size()); }
    int scanned() const { return m_scanned; }
    bool truncated() const { return m_truncated; }
    QString query() const { return m_query; }
    QUrl root() const { return m_root; }
    QString error() const { return m_error; }

    Q_INVOKABLE void start(const QUrl &root, const QString &query, bool includeHidden);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void clear();
    Q_INVOKABLE QVariantMap get(int row) const;

Q_SIGNALS:
    void runningChanged();
    void countChanged();
    void scannedChanged();
    void queryChanged();

private:
    void startLocal(const QString &rootPath, const QString &query, bool includeHidden);
    void startRemote(const QUrl &root, const QString &query, bool includeHidden);
    void append(const QList<Hit> &hits, int scanned);
    void finish(bool truncated, const QString &error);
    void stopWorkers();

    QList<Hit> m_hits;
    QUrl m_root;
    QString m_query;
    QString m_error;
    bool m_running = false;
    bool m_truncated = false;
    int m_scanned = 0;
    int m_generation = 0;
    std::shared_ptr<std::atomic_bool> m_cancel;
    QPointer<KJob> m_job;
};
