// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <KFileItem>

#include <QList>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QUrl>

class KJob;

// Everything the Properties dialog and the preview panel show about one item
// or a selection: type, sizes (recursive for folders, computed by
// KIO::DirectorySizeJob in the background), location, dates, ownership and
// permissions — plus, for the preview panel, a larger image preview and the
// first lines of text files.
//
// Permissions are changed with KIO::chmod; who may do that is decided by the
// file system, the dialog only says in advance when it cannot work.
class FileProperties : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QList<QUrl> urls READ urls WRITE setUrls NOTIFY urlsChanged)
    // Compute the recursive size of folders / the selection.
    Q_PROPERTY(bool computeSize READ computeSize WRITE setComputeSize NOTIFY computeSizeChanged)
    // Load the image preview and text snippet (preview panel).
    Q_PROPERTY(bool previews READ previews WRITE setPreviews NOTIFY previewsChanged)

    Q_PROPERTY(bool loading READ loading NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(QUrl url READ url NOTIFY changed)
    Q_PROPERTY(QString name READ name NOTIFY changed)
    Q_PROPERTY(QString iconName READ iconName NOTIFY changed)
    Q_PROPERTY(QString typeName READ typeName NOTIFY changed)
    Q_PROPERTY(QString mimeType READ mimeType NOTIFY changed)
    Q_PROPERTY(QString location READ location NOTIFY changed)
    Q_PROPERTY(bool isDir READ isDir NOTIFY changed)
    Q_PROPERTY(bool isLink READ isLink NOTIFY changed)
    Q_PROPERTY(QString linkTarget READ linkTarget NOTIFY changed)
    Q_PROPERTY(double size READ size NOTIFY changed)
    Q_PROPERTY(QString created READ created NOTIFY changed)
    Q_PROPERTY(QString modified READ modified NOTIFY changed)
    Q_PROPERTY(QString accessed READ accessed NOTIFY changed)
    Q_PROPERTY(QString owner READ owner NOTIFY changed)
    Q_PROPERTY(QString group READ group NOTIFY changed)
    Q_PROPERTY(int mode READ mode NOTIFY changed)
    Q_PROPERTY(QString permissionsText READ permissionsText NOTIFY changed)
    Q_PROPERTY(bool canChangePermissions READ canChangePermissions NOTIFY changed)
    Q_PROPERTY(QString permissionsNote READ permissionsNote NOTIFY changed)

    Q_PROPERTY(bool sizeBusy READ sizeBusy NOTIFY sizeChanged)
    Q_PROPERTY(double totalSize READ totalSize NOTIFY sizeChanged)
    Q_PROPERTY(int totalFiles READ totalFiles NOTIFY sizeChanged)
    Q_PROPERTY(int totalFolders READ totalFolders NOTIFY sizeChanged)
    Q_PROPERTY(QString sizeError READ sizeError NOTIFY sizeChanged)
    // "1.2 MiB in 14 files and 3 folders"; empty until the size is known.
    Q_PROPERTY(QString contentsText READ contentsText NOTIFY sizeChanged)

    Q_PROPERTY(bool chmodBusy READ chmodBusy NOTIFY chmodChanged)
    Q_PROPERTY(QString chmodError READ chmodError NOTIFY chmodChanged)

    Q_PROPERTY(QString imageSource READ imageSource NOTIFY previewChanged)
    Q_PROPERTY(QString imageInfo READ imageInfo NOTIFY previewChanged)
    Q_PROPERTY(QString textPreview READ textPreview NOTIFY previewChanged)

public:
    explicit FileProperties(QObject *parent = nullptr);

    QList<QUrl> urls() const { return m_urls; }
    void setUrls(const QList<QUrl> &urls);
    bool computeSize() const { return m_computeSize; }
    void setComputeSize(bool compute);
    bool previews() const { return m_previews; }
    void setPreviews(bool previews);

    bool loading() const { return m_loading; }
    QString error() const { return m_error; }
    int count() const { return int(m_urls.size()); }
    QUrl url() const;
    QString name() const;
    QString iconName() const;
    QString typeName() const;
    QString mimeType() const;
    QString location() const;
    bool isDir() const;
    bool isLink() const;
    QString linkTarget() const;
    double size() const;
    QString created() const;
    QString modified() const;
    QString accessed() const;
    QString owner() const;
    QString group() const;
    int mode() const;
    QString permissionsText() const;
    bool canChangePermissions() const;
    QString permissionsNote() const;

    bool sizeBusy() const { return m_sizeBusy; }
    double totalSize() const { return m_totalSize; }
    int totalFiles() const { return m_totalFiles; }
    int totalFolders() const { return m_totalFolders; }
    QString sizeError() const { return m_sizeError; }
    QString contentsText() const;

    bool chmodBusy() const { return m_chmodBusy; }
    QString chmodError() const { return m_chmodError; }

    QString imageSource() const { return m_imageSource; }
    QString imageInfo() const { return m_imageInfo; }
    QString textPreview() const { return m_textPreview; }

    // Set the rwx bits (0–0777) of the item with KIO::chmod. `recursive`
    // also applies them to everything inside a folder; files in it only get
    // an execute bit if they are executable already (like chmod's "X").
    Q_INVOKABLE void applyMode(int mode, bool recursive = false);
    Q_INVOKABLE void refresh();

Q_SIGNALS:
    void urlsChanged();
    void computeSizeChanged();
    void previewsChanged();
    void changed();
    void sizeChanged();
    void chmodChanged();
    void previewChanged();
    void permissionsApplied();

private:
    void statNext(int generation, int index);
    void itemsReady();
    void startSizeJob();
    void startPreviews();
    void resetPreview();
    const KFileItem &item() const;

    QList<QUrl> m_urls;
    KFileItemList m_items;
    int m_generation = 0;
    bool m_computeSize = false;
    bool m_previews = false;
    bool m_loading = false;
    QString m_error;

    bool m_sizeBusy = false;
    double m_totalSize = -1;
    int m_totalFiles = 0;
    int m_totalFolders = 0;
    QString m_sizeError;
    QPointer<KJob> m_sizeJob;

    bool m_chmodBusy = false;
    QString m_chmodError;

    QString m_imageSource;
    QString m_imageInfo;
    QString m_textPreview;
    QPointer<KJob> m_textJob;
};
