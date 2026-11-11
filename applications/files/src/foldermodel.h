// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <KFileItem>

#include <QAbstractListModel>
#include <QCollator>
#include <QJsonArray>
#include <QList>
#include <QPointer>
#include <QQmlEngine>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

class KCoreDirLister;
namespace KIO {
class Job;
}

// The contents of one folder — local or any KIO URL — as a sorted, filtered
// list for the grid and details views, plus what belongs to a view of it:
// navigation history, the selection, and the free space of its file system.
//
// Listing is done by KCoreDirLister, which keeps the model up to date when the
// folder changes on disk (KDirWatch) or through KIO jobs (KDirNotify).
class FolderModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QUrl url READ url NOTIFY urlChanged)
    Q_PROPERTY(QString location READ location NOTIFY urlChanged)
    Q_PROPERTY(QString title READ title NOTIFY urlChanged)
    Q_PROPERTY(QString symbol READ symbol NOTIFY urlChanged)
    Q_PROPERTY(QVariantList crumbs READ crumbs NOTIFY urlChanged)
    Q_PROPERTY(bool local READ isLocal NOTIFY urlChanged)
    Q_PROPERTY(bool trash READ isTrash NOTIFY urlChanged)
    Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY historyChanged)
    Q_PROPERTY(bool canGoForward READ canGoForward NOTIFY historyChanged)
    Q_PROPERTY(bool canGoUp READ canGoUp NOTIFY urlChanged)

    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(QString errorDetail READ errorDetail NOTIFY errorChanged)
    Q_PROPERTY(bool writable READ isWritable NOTIFY writableChanged)

    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int folderCount READ folderCount NOTIFY countChanged)
    Q_PROPERTY(int fileCount READ fileCount NOTIFY countChanged)
    Q_PROPERTY(int hiddenCount READ hiddenCount NOTIFY countChanged)
    Q_PROPERTY(int totalCount READ totalCount NOTIFY countChanged)

    Q_PROPERTY(bool showHidden READ showHidden WRITE setShowHidden NOTIFY showHiddenChanged)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    Q_PROPERTY(QString sortRole READ sortRole WRITE setSortRole NOTIFY sortChanged)
    Q_PROPERTY(bool sortAscending READ sortAscending WRITE setSortAscending NOTIFY sortChanged)
    Q_PROPERTY(bool foldersFirst READ foldersFirst WRITE setFoldersFirst NOTIFY sortChanged)
    Q_PROPERTY(bool foldersOnly READ foldersOnly WRITE setFoldersOnly NOTIFY foldersOnlyChanged)
    Q_PROPERTY(bool thumbnails READ thumbnails WRITE setThumbnails NOTIFY thumbnailsChanged)

    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY selectionChanged)
    Q_PROPERTY(int selectedFolders READ selectedFolders NOTIFY selectionChanged)
    Q_PROPERTY(double selectedSize READ selectedSize NOTIFY selectionChanged)
    Q_PROPERTY(QList<QUrl> selectedUrls READ selectedUrls NOTIFY selectionChanged)

    Q_PROPERTY(bool spaceKnown READ spaceKnown NOTIFY spaceChanged)
    Q_PROPERTY(double freeSpace READ freeSpace NOTIFY spaceChanged)
    Q_PROPERTY(double totalSpace READ totalSpace NOTIFY spaceChanged)

public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        UrlRole,
        IsDirRole,
        IsLinkRole,
        IsHiddenRole,
        SizeRole,
        MimeTypeRole,
        TypeNameRole,
        IconNameRole,
        ModifiedRole,
        ModifiedTextRole,
        PermissionsRole,
        OwnerRole,
        GroupRole,
        SelectedRole,
        ThumbnailRole,
        ReadableRole,
    };

    explicit FolderModel(QObject *parent = nullptr);
    ~FolderModel() override;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QUrl url() const { return m_url; }
    QString location() const;
    QString title() const;
    QString symbol() const;
    QVariantList crumbs() const;
    bool isLocal() const { return m_url.isLocalFile(); }
    bool isTrash() const { return m_url.scheme() == QLatin1String("trash"); }
    bool canGoBack() const { return !m_back.isEmpty(); }
    bool canGoForward() const { return !m_forward.isEmpty(); }
    bool canGoUp() const;

    bool loading() const { return m_loading; }
    QString error() const { return m_error; }
    QString errorDetail() const { return m_errorDetail; }
    bool isWritable() const { return m_writable; }

    int count() const { return int(m_rows.size()); }
    int folderCount() const { return m_folderCount; }
    int fileCount() const { return count() - m_folderCount; }
    int hiddenCount() const { return m_hiddenCount; }
    int totalCount() const { return m_totalCount; }

    bool showHidden() const { return m_showHidden; }
    void setShowHidden(bool show);
    QString filter() const { return m_filter; }
    void setFilter(const QString &filter);
    QString sortRole() const;
    void setSortRole(const QString &role);
    bool sortAscending() const { return m_ascending; }
    void setSortAscending(bool ascending);
    bool foldersFirst() const { return m_foldersFirst; }
    void setFoldersFirst(bool first);
    bool foldersOnly() const { return m_foldersOnly; }
    void setFoldersOnly(bool only);
    bool thumbnails() const { return m_thumbnails; }
    void setThumbnails(bool enabled);

    int selectedCount() const { return int(m_selected.size()); }
    int selectedFolders() const;
    double selectedSize() const;
    QList<QUrl> selectedUrls() const;

    bool spaceKnown() const { return m_spaceKnown; }
    double freeSpace() const { return double(m_freeSpace); }
    double totalSpace() const { return double(m_totalSpace); }

    // ---- navigation ------------------------------------------------------------
    // Go to a folder (recorded in the history).
    Q_INVOKABLE void open(const QUrl &url);
    // Go to what the user typed: a path, "~", or a URL. A file is revealed in
    // its folder.
    Q_INVOKABLE void openLocation(const QString &text);
    // Show the folder containing `url` and select the item.
    Q_INVOKABLE void reveal(const QUrl &url);
    Q_INVOKABLE void goBack();
    Q_INVOKABLE void goForward();
    Q_INVOKABLE void goUp();
    Q_INVOKABLE void reload();

    // ---- selection ---------------------------------------------------------------
    Q_INVOKABLE bool isSelected(int row) const;
    Q_INVOKABLE void selectOnly(int row);
    Q_INVOKABLE void toggle(int row);
    Q_INVOKABLE void setSelected(int row, bool selected);
    Q_INVOKABLE void selectRange(int from, int to, bool keepOthers = false);
    // Replace the selection with these rows (rubber band).
    Q_INVOKABLE void selectRows(const QList<int> &rows);
    Q_INVOKABLE QList<int> selectedRows() const;
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE void clearSelection();
    // Select these items as soon as the lister reports them (after a paste,
    // rename or "new folder"); the first one is revealed.
    Q_INVOKABLE void selectWhenAvailable(const QList<QUrl> &urls);

    // ---- lookups -------------------------------------------------------------------
    Q_INVOKABLE QVariantMap get(int row) const;
    Q_INVOKABLE QUrl urlAt(int row) const;
    Q_INVOKABLE bool isDirAt(int row) const;
    Q_INVOKABLE int indexOfUrl(const QUrl &url) const;
    // First row at or after `from` (wrapping) whose name starts with `prefix`.
    Q_INVOKABLE int findByPrefix(const QString &prefix, int from) const;
    // True if the folder has an item of that name, visible or not.
    Q_INVOKABLE bool nameExists(const QString &name) const;

    // Synchronous listing for --dump (spins the event loop until done).
    bool loadBlocking(const QUrl &url, int timeoutMs = 20000);
    QJsonArray toJson() const;

    static QString resolvedIconName(const KFileItem &item);

Q_SIGNALS:
    void urlChanged();
    void historyChanged();
    void loadingChanged();
    void errorChanged();
    void writableChanged();
    void countChanged();
    void showHiddenChanged();
    void filterChanged();
    void sortChanged();
    void foldersOnlyChanged();
    void thumbnailsChanged();
    void selectionChanged();
    void spaceChanged();
    // The view should make this row current and scroll to it.
    void revealRow(int row);
    // Listing of the current folder finished (successfully or not).
    void loaded();

private:
    enum SortRole { ByName, BySize, ByModified, ByType };

    struct Row {
        KFileItem item;
        QCollatorSortKey key;
        qulonglong size;
        qint64 mtime;
        bool dir;
        QString type;       // only filled while sorting by type
    };

    void load(const QUrl &url);
    void clearRows();
    void setError(const QString &error, const QString &detail = QString());
    void setLoading(bool loading);
    bool accepts(const KFileItem &item) const;
    Row makeRow(const KFileItem &item) const;
    bool lessThan(const Row &a, const Row &b) const;
    void rebuild();
    void addItems(const KFileItemList &items);
    void removeItems(const KFileItemList &items);
    void refreshItems(const QList<QPair<KFileItem, KFileItem>> &items);
    void insertRow(const Row &row);
    void recount();
    void pruneSelection();
    void emitSelection(int first, int last);
    void applyPendingSelection();
    void updateWritable();
    void refreshSpace();
    void checkStillExists();
    void onThumbnailReady(const QUrl &url, int size);

    KCoreDirLister *m_lister;
    QUrl m_url;
    QList<QUrl> m_back;
    QList<QUrl> m_forward;
    QList<Row> m_rows;
    QSet<QUrl> m_selected;
    QCollator m_collator;

    bool m_listed = false;      // the lister holds m_url
    bool m_restarting = false;  // the lister is being stopped or re-opened by this object
    bool m_loading = false;
    QString m_error;
    QString m_errorDetail;
    bool m_writable = false;
    int m_folderCount = 0;
    int m_hiddenCount = 0;
    int m_totalCount = 0;

    bool m_showHidden = false;
    QString m_filter;
    SortRole m_sort = ByName;
    bool m_ascending = true;
    bool m_foldersFirst = true;
    bool m_foldersOnly = false;
    bool m_thumbnails = false;

    QList<QUrl> m_pendingSelection;
    bool m_pendingFresh = false;
    QTimer m_pendingTimer;

    bool m_spaceKnown = false;
    qulonglong m_freeSpace = 0;
    qulonglong m_totalSpace = 0;
    QTimer m_spaceTimer;
    QTimer m_existsTimer;
    QTimer m_refreshTimer;
};
