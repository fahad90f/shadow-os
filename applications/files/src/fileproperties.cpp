// SPDX-License-Identifier: GPL-3.0-or-later
#include "fileproperties.h"

#include "foldermodel.h"
#include "thumbnails.h"
#include "util.h"

#include <KIO/ChmodJob>
#include <KIO/DirectorySizeJob>
#include <KIO/StatJob>
#include <KIO/TransferJob>
#include <KProtocolManager>

#include <QCoreApplication>
#include <QFileInfo>
#include <QImageReader>
#include <QLocale>
#include <QMimeType>
#include <QtConcurrentRun>

#include <unistd.h>

using namespace Qt::StringLiterals;

namespace {

constexpr int kPreviewSize = 512;
constexpr qsizetype kSnippetBytes = 4096;
constexpr int kSnippetLines = 40;

QString longDateTime(const QDateTime &dt)
{
    if (!dt.isValid() || dt.toSecsSinceEpoch() <= 0)
        return {};
    const QLocale locale;
    const QDateTime local = dt.toLocalTime();
    return locale.toString(local.date(), QLocale::LongFormat) + u", "_s + local.time().toString(u"HH:mm:ss"_s);
}

QString rwx(int mode)
{
    QString s;
    static const char flags[] = "rwxrwxrwx";
    for (int i = 0; i < 9; ++i)
        s += (mode & (0400 >> i)) ? QChar(QLatin1Char(flags[i])) : QChar(u'-');
    return s;
}

const KFileItem &nullItem()
{
    static const KFileItem item;
    return item;
}

} // namespace

FileProperties::FileProperties(QObject *parent)
    : QObject(parent)
{
    connect(ThumbnailStore::instance(), &ThumbnailStore::ready, this, [this](const QUrl &url, int size) {
        if (!m_previews || size != kPreviewSize || m_items.size() != 1 || url != m_items.first().url())
            return;
        const QString source = ThumbnailStore::instance()->lookup(m_items.first(), kPreviewSize);
        if (source != m_imageSource) {
            m_imageSource = source;
            Q_EMIT previewChanged();
        }
    });
}

const KFileItem &FileProperties::item() const
{
    return m_items.size() == 1 ? m_items.first() : nullItem();
}

void FileProperties::setUrls(const QList<QUrl> &urls)
{
    if (urls == m_urls)
        return;
    m_urls = urls;
    Q_EMIT urlsChanged();
    refresh();
}

void FileProperties::setComputeSize(bool compute)
{
    if (compute == m_computeSize)
        return;
    m_computeSize = compute;
    Q_EMIT computeSizeChanged();
    if (compute && !m_loading && !m_items.isEmpty())
        startSizeJob();
}

void FileProperties::setPreviews(bool previews)
{
    if (previews == m_previews)
        return;
    m_previews = previews;
    Q_EMIT previewsChanged();
    if (previews && !m_loading && !m_items.isEmpty())
        startPreviews();
    else if (!previews)
        resetPreview();
}

void FileProperties::resetPreview()
{
    if (m_textJob)
        m_textJob->kill(KJob::Quietly);
    m_textJob.clear();
    if (!m_imageSource.isEmpty() || !m_imageInfo.isEmpty() || !m_textPreview.isEmpty()) {
        m_imageSource.clear();
        m_imageInfo.clear();
        m_textPreview.clear();
        Q_EMIT previewChanged();
    }
}

void FileProperties::refresh()
{
    ++m_generation;
    if (m_sizeJob)
        m_sizeJob->kill(KJob::Quietly);
    m_sizeJob.clear();
    resetPreview();

    m_items.clear();
    m_error.clear();
    m_sizeBusy = false;
    m_totalSize = -1;
    m_totalFiles = 0;
    m_totalFolders = 0;
    m_sizeError.clear();
    if (!m_chmodError.isEmpty()) {
        m_chmodError.clear();
        Q_EMIT chmodChanged();
    }
    Q_EMIT sizeChanged();

    if (m_urls.isEmpty()) {
        m_loading = false;
        Q_EMIT changed();
        return;
    }
    m_loading = true;
    Q_EMIT changed();

    const bool allLocal = std::all_of(m_urls.cbegin(), m_urls.cend(), [](const QUrl &u) { return u.isLocalFile(); });
    if (m_urls.size() > 1 && allLocal) {
        // A selection only needs types and sizes; KFileItem stats local files itself.
        for (const QUrl &url : std::as_const(m_urls)) {
            if (QFileInfo::exists(url.toLocalFile()) || QFileInfo(url.toLocalFile()).isSymLink())
                m_items.append(KFileItem(url));
        }
        itemsReady();
        return;
    }
    statNext(m_generation, 0);
}

void FileProperties::statNext(int generation, int index)
{
    if (generation != m_generation)
        return;
    if (index >= m_urls.size()) {
        itemsReady();
        return;
    }
    const QUrl url = m_urls.at(index);
    KIO::StatJob *job = KIO::stat(url, KIO::StatJob::SourceSide, KIO::StatDefaultDetails, KIO::HideProgressInfo);
    connect(job, &KJob::result, this, [this, job, url, generation, index] {
        if (generation != m_generation)
            return;
        if (job->error() == KJob::NoError)
            m_items.append(KFileItem(job->statResult(), url));
        else if (m_error.isEmpty())
            m_error = job->errorString();
        statNext(generation, index + 1);
    });
}

void FileProperties::itemsReady()
{
    m_loading = false;
    if (m_items.isEmpty() && m_error.isEmpty())
        m_error = tr("The item no longer exists.");
    if (m_items.size() == 1) {
        const KFileItem &it = m_items.first();
        if (it.isLocalFile() && !it.isSlow())
            it.determineMimeType();
    }
    Q_EMIT changed();
    if (m_computeSize)
        startSizeJob();
    if (m_previews)
        startPreviews();
}

void FileProperties::startSizeJob()
{
    if (m_items.isEmpty() || m_sizeBusy)
        return;
    if (m_items.size() == 1 && !m_items.first().isDir()) {
        m_totalSize = double(m_items.first().size());
        m_totalFiles = 1;
        m_totalFolders = 0;
        Q_EMIT sizeChanged();
        return;
    }
    m_sizeBusy = true;
    m_sizeError.clear();
    Q_EMIT sizeChanged();
    KIO::DirectorySizeJob *job = KIO::directorySize(m_items);
    m_sizeJob = job;
    const int generation = m_generation;
    connect(job, &KJob::result, this, [this, job, generation] {
        if (generation != m_generation)
            return;
        m_sizeBusy = false;
        if (job->error() != KJob::NoError) {
            m_sizeError = job->errorString();
        } else {
            m_totalSize = double(job->totalSize());
            m_totalFiles = int(job->totalFiles());
            m_totalFolders = int(job->totalSubdirs());
        }
        Q_EMIT sizeChanged();
    });
}

void FileProperties::startPreviews()
{
    if (m_items.size() != 1 || m_items.first().isDir())
        return;
    const KFileItem it = m_items.first();
    const int generation = m_generation;

    if (ThumbnailStore::canPreview(it)) {
        m_imageSource = ThumbnailStore::instance()->lookup(it, kPreviewSize);
        if (!m_imageSource.isEmpty())
            Q_EMIT previewChanged();
        if (it.isLocalFile() && it.currentMimeType().name().startsWith("image/"_L1)) {
            const QString path = it.localPath();
            const QPointer<FileProperties> self(this);
            (void)QtConcurrent::run([self, path, generation] {
                const QSize size = QImageReader(path).size();
                QMetaObject::invokeMethod(qApp, [self, size, generation] {
                    if (!self || self->m_generation != generation || !size.isValid())
                        return;
                    self->m_imageInfo = tr("%1 × %2 pixels").arg(size.width()).arg(size.height());
                    Q_EMIT self->previewChanged();
                }, Qt::QueuedConnection);
            });
        }
    }

    if (it.size() > 0 && it.currentMimeType().inherits(u"text/plain"_s)) {
        // Only the beginning of the file is needed: stop the transfer once
        // enough has arrived.
        auto buffer = std::make_shared<QByteArray>();
        KIO::TransferJob *job = KIO::get(it.url(), KIO::NoReload, KIO::HideProgressInfo);
        m_textJob = job;
        const auto publish = [this, buffer, generation] {
            if (generation != m_generation)
                return;
            QByteArray bytes = buffer->left(kSnippetBytes);
            if (bytes.contains('\0'))
                return;                 // binary content despite the type
            QString text = QString::fromUtf8(bytes);
            QStringList lines = text.split(u'\n');
            if (buffer->size() > kSnippetBytes && lines.size() > 1)
                lines.removeLast();     // cut-off line
            if (lines.size() > kSnippetLines)
                lines = lines.first(kSnippetLines);
            text = lines.join(u'\n');
            while (text.endsWith(u'\n'))
                text.chop(1);
            if (text != m_textPreview) {
                m_textPreview = text;
                Q_EMIT previewChanged();
            }
        };
        connect(job, &KIO::TransferJob::data, this, [buffer, publish](KIO::Job *j, const QByteArray &data) {
            buffer->append(data);
            if (buffer->size() > kSnippetBytes) {
                publish();
                j->kill(KJob::Quietly);
            }
        });
        connect(job, &KJob::result, this, [publish](KJob *j) {
            if (j->error() == KJob::NoError)
                publish();
        });
    }
}

QString FileProperties::contentsText() const
{
    if (m_totalSize < 0)
        return {};
    const QString files = m_totalFiles == 1 ? tr("1 file") : tr("%1 files").arg(m_totalFiles);
    const QString folders = m_totalFolders == 1 ? tr("1 folder") : tr("%1 folders").arg(m_totalFolders);
    return tr("%1 in %2 and %3").arg(Util::formatBytes(qulonglong(m_totalSize)), files, folders);
}

// ---- single item -------------------------------------------------------------------

QUrl FileProperties::url() const
{
    return m_urls.size() == 1 ? m_urls.first() : QUrl();
}

QString FileProperties::name() const
{
    if (m_urls.size() > 1)
        return tr("%1 items").arg(m_urls.size());
    if (m_urls.isEmpty())
        return {};
    const QString text = item().isNull() ? QString() : item().text();
    return text.isEmpty() || text == "."_L1 ? Util::displayName(m_urls.first()) : text;
}

QString FileProperties::iconName() const
{
    if (m_items.size() == 1)
        return FolderModel::resolvedIconName(item());
    return m_urls.size() > 1 ? u"document-multiple"_s : QString();
}

QString FileProperties::typeName() const
{
    if (m_items.size() == 1)
        return item().mimeComment();
    if (m_items.size() > 1) {
        const int folders = int(std::count_if(m_items.cbegin(), m_items.cend(), [](const KFileItem &i) { return i.isDir(); }));
        const int files = int(m_items.size()) - folders;
        QStringList parts;
        if (folders > 0)
            parts << (folders == 1 ? tr("1 folder") : tr("%1 folders").arg(folders));
        if (files > 0)
            parts << (files == 1 ? tr("1 file") : tr("%1 files").arg(files));
        return parts.join(u", "_s);
    }
    return {};
}

QString FileProperties::mimeType() const
{
    return m_items.size() == 1 ? item().mimetype() : QString();
}

QString FileProperties::location() const
{
    if (m_urls.isEmpty())
        return {};
    const QUrl parent = Util::parentUrl(m_urls.first());
    const bool sameFolder = std::all_of(m_urls.cbegin(), m_urls.cend(), [&parent](const QUrl &u) {
        return Util::sameUrl(Util::parentUrl(u), parent);
    });
    return sameFolder ? Util::displayPath(parent) : tr("Several folders");
}

bool FileProperties::isDir() const
{
    return m_items.size() == 1 && item().isDir();
}

bool FileProperties::isLink() const
{
    return m_items.size() == 1 && item().isLink();
}

QString FileProperties::linkTarget() const
{
    return isLink() ? item().linkDest() : QString();
}

double FileProperties::size() const
{
    return m_items.size() == 1 && !item().isDir() ? double(item().size()) : -1.0;
}

QString FileProperties::created() const
{
    return m_items.size() == 1 ? longDateTime(item().time(KFileItem::CreationTime)) : QString();
}

QString FileProperties::modified() const
{
    return m_items.size() == 1 ? longDateTime(item().time(KFileItem::ModificationTime)) : QString();
}

QString FileProperties::accessed() const
{
    return m_items.size() == 1 ? longDateTime(item().time(KFileItem::AccessTime)) : QString();
}

QString FileProperties::owner() const
{
    return m_items.size() == 1 ? item().user() : QString();
}

QString FileProperties::group() const
{
    return m_items.size() == 1 ? item().group() : QString();
}

int FileProperties::mode() const
{
    if (m_items.size() != 1 || item().permissions() == KFileItem::Unknown)
        return -1;
    return int(item().permissions() & 07777);
}

QString FileProperties::permissionsText() const
{
    const int m = mode();
    if (m < 0)
        return {};
    return rwx(m) + u" ("_s + QString::number(m & 07777, 8).rightJustified(3, u'0') + u')';
}

bool FileProperties::canChangePermissions() const
{
    return m_items.size() == 1 && mode() >= 0 && permissionsNote().isEmpty();
}

QString FileProperties::permissionsNote() const
{
    if (m_items.size() != 1 || mode() < 0)
        return {};
    const KFileItem &it = item();
    const QUrl u = it.url();
    if (u.scheme() == "trash"_L1)
        return tr("Permissions of items in the Trash cannot be changed. Restore the item first.");
    if (it.isLocalFile()) {
        const QFileInfo info(it.localPath());
        const uint uid = info.ownerId();
        const uid_t me = ::geteuid();
        if (me != 0 && uid != uint(-2) && uid != uint(me)) {
            return tr("Only the owner (%1) or an administrator can change the permissions of this item.")
                .arg(info.owner().isEmpty() ? QString::number(uid) : info.owner());
        }
        return {};
    }
    if (!KProtocolManager::supportsPermissions(u))
        return tr("%1:// locations do not support changing permissions.").arg(u.scheme());
    return {};
}

void FileProperties::applyMode(int mode, bool recursive)
{
    if (m_items.size() != 1 || m_chmodBusy)
        return;
    m_chmodBusy = true;
    m_chmodError.clear();
    Q_EMIT chmodChanged();
    // Only the nine rwx bits are touched; set-id and sticky bits stay as they are.
    KIO::ChmodJob *job = KIO::chmod(m_items, mode & 0777, 0777, QString(), QString(), recursive && isDir(),
                                    KIO::HideProgressInfo | KIO::NoPrivilegeExecution);
    connect(job, &KJob::result, this, [this](KJob *j) {
        m_chmodBusy = false;
        const bool failed = j->error() != KJob::NoError;
        if (failed)
            m_chmodError = j->errorString();
        Q_EMIT chmodChanged();
        if (!failed) {
            Q_EMIT permissionsApplied();
            refresh();
        }
    });
}
