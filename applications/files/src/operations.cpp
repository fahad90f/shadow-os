// SPDX-License-Identifier: GPL-3.0-or-later
#include "operations.h"

#include "interaction.h"
#include "util.h"

#include <KFileUtils>
#include <KIO/CopyJob>
#include <KIO/DeleteJob>
#include <KIO/EmptyTrashJob>
#include <KIO/FileUndoManager>
#include <KIO/JobTracker>
#include <KIO/MkdirJob>
#include <KIO/Paste>
#include <KIO/RestoreJob>
#include <KIO/StoredTransferJob>
#include <KJob>

#include <QClipboard>
#include <QDateTime>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMimeData>
#include <QStorageInfo>

#include <memory>
#include <sys/stat.h>

using namespace Qt::StringLiterals;

namespace {

// “name” for one item, "N items" for several.
QString describe(const QList<QUrl> &urls)
{
    if (urls.size() == 1)
        return FileOperations::tr("“%1”").arg(Util::displayName(urls.first()));
    return FileOperations::tr("%1 items").arg(urls.size());
}

bool isTrashUrl(const QUrl &url)
{
    return url.scheme() == "trash"_L1;
}

// Device id of a local path (of its nearest existing ancestor for `dir`).
bool deviceOf(const QString &path, dev_t *device)
{
    struct stat st {};
    if (::lstat(QFile::encodeName(path).constData(), &st) != 0)
        return false;
    *device = st.st_dev;
    return true;
}

class UndoUi : public KIO::FileUndoManager::UiInterface
{
public:
    UndoUi() { setShowProgressInfo(true); }

    void jobError(KIO::Job *job) override
    {
        Interaction::instance()->reportError(FileOperations::tr("Undo failed"), job->errorString());
    }

    bool copiedFileWasModified(const QUrl &src, const QUrl &dest, const QDateTime &srcTime,
                               const QDateTime &destTime) override
    {
        Q_UNUSED(src)
        Q_UNUSED(srcTime)
        Q_UNUSED(destTime)
        // Undoing the copy would delete a file that was changed afterwards.
        // Keep it and say so instead of silently losing the modifications.
        Interaction::instance()->reportError(
            FileOperations::tr("Undo stopped"),
            FileOperations::tr("“%1” was modified after it was copied, so it was not deleted.")
                .arg(Util::displayName(dest)));
        return false;
    }
};

} // namespace

// ---- OperationsModel -----------------------------------------------------------

OperationsModel::OperationsModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int OperationsModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_ops.size());
}

QHash<int, QByteArray> OperationsModel::roleNames() const
{
    return {
        {IdRole, "opId"}, {TitleRole, "title"}, {DetailRole, "detail"}, {ProgressRole, "progress"},
        {StateRole, "status"}, {ErrorRole, "error"}, {CancellableRole, "cancellable"},
    };
}

QString OperationsModel::amountText(const Op &op)
{
    QString text;
    if (op.total > 0)
        text = tr("%1 of %2").arg(Util::formatBytes(op.processed), Util::formatBytes(op.total));
    else if (op.processed > 0)
        text = Util::formatBytes(op.processed);
    if (op.speed > 0)
        text += (text.isEmpty() ? QString() : u" · "_s) + tr("%1/s").arg(Util::formatBytes(op.speed));
    return text;
}

QVariant OperationsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_ops.size())
        return {};
    const Op &op = m_ops.at(index.row());
    switch (role) {
    case IdRole: return op.id;
    case TitleRole: return op.title;
    case DetailRole: {
        const QString amounts = amountText(op);
        if (op.detail.isEmpty())
            return amounts;
        return amounts.isEmpty() ? op.detail : op.detail + u" · "_s + amounts;
    }
    case ProgressRole: return op.progress;
    case StateRole: return op.state;
    case ErrorRole: return op.error;
    case CancellableRole: return op.state == "running"_L1 && bool(op.cancel);
    }
    return {};
}

int OperationsModel::activeCount() const
{
    return int(std::count_if(m_ops.cbegin(), m_ops.cend(), [](const Op &op) { return op.state == "running"_L1; }));
}

double OperationsModel::overallProgress() const
{
    double sum = 0;
    int known = 0;
    for (const Op &op : m_ops) {
        if (op.state == "running"_L1 && op.progress >= 0) {
            sum += op.progress;
            ++known;
        }
    }
    return known == 0 ? -1.0 : sum / known;
}

int OperationsModel::rowOfJob(const KJob *job) const
{
    for (int i = 0; i < m_ops.size(); ++i) {
        if (m_ops.at(i).job == job)
            return i;
    }
    return -1;
}

int OperationsModel::rowOfId(int id) const
{
    for (int i = 0; i < m_ops.size(); ++i) {
        if (m_ops.at(i).id == id)
            return i;
    }
    return -1;
}

void OperationsModel::changed(int row, const QList<int> &roles)
{
    const QModelIndex idx = index(row);
    Q_EMIT dataChanged(idx, idx, roles);
    if (roles.contains(ProgressRole))
        Q_EMIT progressChanged();
}

void OperationsModel::removeRow(int row)
{
    beginRemoveRows({}, row, row);
    m_ops.removeAt(row);
    endRemoveRows();
    Q_EMIT countChanged();
    Q_EMIT activeCountChanged();
    Q_EMIT progressChanged();
}

void OperationsModel::fail(int row, const QString &error)
{
    Op &op = m_ops[row];
    op.state = u"error"_s;
    op.error = error;
    op.cancel = nullptr;
    changed(row, {StateRole, ErrorRole, CancellableRole});
    Q_EMIT activeCountChanged();
    Q_EMIT progressChanged();
}

void OperationsModel::cancel(int id)
{
    const int row = rowOfId(id);
    if (row < 0 || m_ops.at(row).state != "running"_L1)
        return;
    const auto cancelFn = m_ops.at(row).cancel;   // copy: the callback may remove the row
    if (cancelFn)
        cancelFn();
}

void OperationsModel::dismiss(int id)
{
    const int row = rowOfId(id);
    if (row >= 0 && m_ops.at(row).state != "running"_L1)
        removeRow(row);
}

void OperationsModel::clearFinished()
{
    for (int row = int(m_ops.size()) - 1; row >= 0; --row) {
        if (m_ops.at(row).state != "running"_L1)
            removeRow(row);
    }
}

void OperationsModel::addJob(KJob *job)
{
    if (!job || rowOfJob(job) >= 0)
        return;
    Op op;
    op.id = m_nextId++;
    op.job = job;
    op.title = tr("Working…");
    if (job->capabilities() & KJob::Killable) {
        const QPointer<KJob> guard(job);
        op.cancel = [guard] {
            if (guard) {
                guard->setProperty(Interaction::cancelledProperty, true);
                guard->kill(KJob::EmitResult);
            }
        };
    }
    beginInsertRows({}, int(m_ops.size()), int(m_ops.size()));
    m_ops.append(op);
    endInsertRows();
    Q_EMIT countChanged();
    Q_EMIT activeCountChanged();
    Q_EMIT progressChanged();
}

void OperationsModel::removeJob(KJob *job)
{
    const int row = rowOfJob(job);
    if (row >= 0 && m_ops.at(row).state == "running"_L1)
        removeRow(row);
}

void OperationsModel::setJobTitle(KJob *job, const QString &title)
{
    const int row = rowOfJob(job);
    if (row < 0)
        return;
    m_ops[row].title = title;
    changed(row, {TitleRole});
}

void OperationsModel::setJobDetail(KJob *job, const QString &detail)
{
    const int row = rowOfJob(job);
    if (row < 0 || m_ops.at(row).detail == detail)
        return;
    m_ops[row].detail = detail;
    changed(row, {DetailRole});
}

void OperationsModel::setJobAmounts(KJob *job, qulonglong processed, qulonglong total)
{
    const int row = rowOfJob(job);
    if (row < 0)
        return;
    Op &op = m_ops[row];
    op.processed = processed;
    op.total = total;
    changed(row, {DetailRole});
}

void OperationsModel::setJobPercent(KJob *job, unsigned long percent)
{
    const int row = rowOfJob(job);
    if (row < 0)
        return;
    m_ops[row].progress = qBound(0.0, double(percent) / 100.0, 1.0);
    changed(row, {ProgressRole});
}

void OperationsModel::setJobSpeed(KJob *job, unsigned long bytesPerSecond)
{
    const int row = rowOfJob(job);
    if (row < 0)
        return;
    m_ops[row].speed = bytesPerSecond;
    changed(row, {DetailRole});
}

void OperationsModel::finishJob(KJob *job)
{
    const int row = rowOfJob(job);
    if (row < 0)
        return;
    if (job->error() != KJob::NoError && job->error() != KJob::KilledJobError)
        fail(row, job->errorString());
    else
        removeRow(row);
}

int OperationsModel::addExternal(const QString &title, std::function<void()> cancel)
{
    Op op;
    op.id = m_nextId++;
    op.title = title;
    op.cancel = std::move(cancel);
    beginInsertRows({}, int(m_ops.size()), int(m_ops.size()));
    m_ops.append(op);
    endInsertRows();
    Q_EMIT countChanged();
    Q_EMIT activeCountChanged();
    Q_EMIT progressChanged();
    return op.id;
}

void OperationsModel::setExternalDetail(int id, const QString &detail)
{
    const int row = rowOfId(id);
    if (row < 0)
        return;
    m_ops[row].detail = detail;
    changed(row, {DetailRole});
}

void OperationsModel::finishExternal(int id, bool ok, const QString &error)
{
    const int row = rowOfId(id);
    if (row < 0)
        return;
    if (ok)
        removeRow(row);
    else
        fail(row, error);
}

// ---- OperationsTracker -----------------------------------------------------------

OperationsTracker::OperationsTracker(OperationsModel *model, QObject *parent)
    : KJobTrackerInterface(parent)
    , m_model(model)
{
}

void OperationsTracker::registerJob(KJob *job)
{
    // The base class would connect the job to its virtual slots; everything
    // needed is connected here directly.
    m_model->addJob(job);
    connect(job, &KJob::finished, this, [this](KJob *j) { m_model->finishJob(j); });
    connect(job, &KJob::description, this,
            [this](KJob *j, const QString &title, const QPair<QString, QString> &field1, const QPair<QString, QString> &) {
        Q_UNUSED(title)
        // field1 is the item being processed (path or URL).
        QString name = field1.second;
        const qsizetype slash = name.lastIndexOf(u'/');
        if (slash >= 0 && slash < name.size() - 1)
            name = name.mid(slash + 1);
        m_model->setJobDetail(j, name);
    });
    connect(job, &KJob::percentChanged, this, [this](KJob *j, unsigned long percent) {
        m_model->setJobPercent(j, percent);
    });
    connect(job, &KJob::processedAmountChanged, this, [this](KJob *j, KJob::Unit unit, qulonglong) {
        if (unit == KJob::Bytes)
            m_model->setJobAmounts(j, j->processedAmount(KJob::Bytes), j->totalAmount(KJob::Bytes));
    });
    connect(job, &KJob::totalAmountChanged, this, [this](KJob *j, KJob::Unit unit, qulonglong) {
        if (unit == KJob::Bytes)
            m_model->setJobAmounts(j, j->processedAmount(KJob::Bytes), j->totalAmount(KJob::Bytes));
    });
    connect(job, &KJob::speed, this, [this](KJob *j, unsigned long value) { m_model->setJobSpeed(j, value); });
}

void OperationsTracker::unregisterJob(KJob *job)
{
    job->disconnect(this);
    m_model->removeJob(job);
}

// ---- FileOperations ----------------------------------------------------------------

FileOperations::FileOperations(QObject *parent)
    : QObject(parent)
    , m_jobs(new OperationsModel(this))
{
    KIO::setJobTracker(new OperationsTracker(m_jobs, this));

    auto *undo = KIO::FileUndoManager::self();
    undo->setUiInterface(new UndoUi);   // owned by the undo manager
    connect(undo, &KIO::FileUndoManager::undoAvailable, this, &FileOperations::undoChanged);
    connect(undo, &KIO::FileUndoManager::undoTextChanged, this, &FileOperations::undoChanged);
    connect(undo, &KIO::FileUndoManager::undoJobFinished, this, &FileOperations::operationFinished);

    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &FileOperations::readClipboard);
    readClipboard();
}

FileOperations *FileOperations::instance()
{
    static FileOperations *self = new FileOperations(qApp);
    return self;
}

FileOperations *FileOperations::create(QQmlEngine *, QJSEngine *)
{
    FileOperations *self = instance();
    QQmlEngine::setObjectOwnership(self, QQmlEngine::CppOwnership);
    return self;
}

bool FileOperations::undoAvailable() const
{
    return KIO::FileUndoManager::self()->isUndoAvailable();
}

QString FileOperations::undoText() const
{
    // "Und&o: Copy" → "Undo: Copy"
    return KIO::FileUndoManager::self()->undoText().remove(u'&');
}

void FileOperations::undo()
{
    if (undoAvailable())
        KIO::FileUndoManager::self()->undo();
}

void FileOperations::watchResult(KJob *job, const QString &failureTitle)
{
    connect(job, &KJob::result, this, [this, failureTitle](KJob *j) {
        if (j->error() == KJob::KilledJobError) {
            // KIO also ends a job this way when the system refuses it without
            // a specific error (an immutable file, a privileged step that is
            // not available). Only a cancellation the user asked for is silent.
            if (!j->property(Interaction::cancelledProperty).toBool()) {
                Interaction::instance()->reportError(
                    failureTitle,
                    tr("The operation was stopped before it finished: the system refused it, "
                       "or a confirmation it needed was dismissed."));
            }
        } else if (j->error() != KJob::NoError) {
            Interaction::instance()->reportError(failureTitle, j->errorString());
        }
        Q_EMIT operationFinished();
    });
}

void FileOperations::startCopy(const QList<QUrl> &urls, const QUrl &destDir, bool move, bool autoRename,
                               bool clearClipboardWhenDone)
{
    if (urls.isEmpty() || !destDir.isValid())
        return;
    KIO::CopyJob *job = move ? KIO::move(urls, destDir) : KIO::copy(urls, destDir);
    if (autoRename)
        job->setAutoRename(true);
    const QString where = Util::displayName(destDir);
    m_jobs->setJobTitle(job, move ? tr("Moving %1 to “%2”").arg(describe(urls), where)
                                  : tr("Copying %1 to “%2”").arg(describe(urls), where));
    KIO::FileUndoManager::self()->recordCopyJob(job);

    auto created = std::make_shared<QList<QUrl>>();
    connect(job, &KIO::CopyJob::copyingDone, this,
            [created, destDir](KIO::Job *, const QUrl &, const QUrl &to, const QDateTime &, bool, bool) {
        if (Util::sameUrl(Util::parentUrl(to), destDir))
            created->append(to);
    });
    connect(job, &KJob::result, this, [this, created, destDir, clearClipboardWhenDone](KJob *j) {
        if (!created->isEmpty())
            Q_EMIT itemsCreated(destDir, *created);
        if (clearClipboardWhenDone && j->error() == KJob::NoError)
            QGuiApplication::clipboard()->clear();
    });
    watchResult(job, move ? tr("Could not move %1").arg(describe(urls)) : tr("Could not copy %1").arg(describe(urls)));
}

void FileOperations::copyTo(const QList<QUrl> &urls, const QUrl &destDir)
{
    startCopy(urls, destDir, false, false, false);
}

void FileOperations::moveTo(const QList<QUrl> &urls, const QUrl &destDir)
{
    startCopy(urls, destDir, true, false, false);
}

void FileOperations::linkTo(const QList<QUrl> &urls, const QUrl &destDir)
{
    if (urls.isEmpty() || !destDir.isValid())
        return;
    KIO::CopyJob *job = KIO::link(urls, destDir);
    m_jobs->setJobTitle(job, tr("Linking %1 in “%2”").arg(describe(urls), Util::displayName(destDir)));
    KIO::FileUndoManager::self()->recordCopyJob(job);
    watchResult(job, tr("Could not create the link"));
}

void FileOperations::rename(const QUrl &url, const QString &newName)
{
    const QString name = newName.trimmed();
    if (!validateName(name).isEmpty() || name == url.adjusted(QUrl::StripTrailingSlash).fileName())
        return;
    const QUrl dir = Util::parentUrl(url);
    const QUrl dest = Util::childUrl(dir, name);
    KIO::CopyJob *job = KIO::moveAs(url, dest, KIO::HideProgressInfo);
    KIO::FileUndoManager::self()->recordJob(KIO::FileUndoManager::Rename, {url}, dest, job);
    connect(job, &KJob::result, this, [this, dir, dest](KJob *j) {
        if (j->error() == KJob::NoError)
            Q_EMIT itemsCreated(dir, {dest});
    });
    watchResult(job, tr("Could not rename “%1”").arg(Util::displayName(url)));
}

void FileOperations::trash(const QList<QUrl> &urls)
{
    if (urls.isEmpty())
        return;
    KIO::CopyJob *job = KIO::trash(urls);
    m_jobs->setJobTitle(job, tr("Moving %1 to the Trash").arg(describe(urls)));
    KIO::FileUndoManager::self()->recordJob(KIO::FileUndoManager::Trash, urls, QUrl(u"trash:/"_s), job);
    watchResult(job, tr("Could not move %1 to the Trash").arg(describe(urls)));
}

void FileOperations::deletePermanently(const QList<QUrl> &urls)
{
    if (urls.isEmpty())
        return;
    KIO::DeleteJob *job = KIO::del(urls);
    m_jobs->setJobTitle(job, tr("Deleting %1").arg(describe(urls)));
    watchResult(job, tr("Could not delete %1").arg(describe(urls)));
}

void FileOperations::restore(const QList<QUrl> &trashUrls)
{
    if (trashUrls.isEmpty())
        return;
    KIO::RestoreJob *job = KIO::restoreFromTrash(trashUrls);
    m_jobs->setJobTitle(job, tr("Restoring %1").arg(describe(trashUrls)));
    watchResult(job, tr("Could not restore %1").arg(describe(trashUrls)));
}

void FileOperations::emptyTrash()
{
    KIO::EmptyTrashJob *job = KIO::emptyTrash();
    KIO::getJobTracker()->registerJob(job);
    m_jobs->setJobTitle(job, tr("Emptying the Trash"));
    watchResult(job, tr("Could not empty the Trash"));
}

void FileOperations::createFolder(const QUrl &parentDir, const QString &name)
{
    const QString trimmed = name.trimmed();
    if (!validateName(trimmed).isEmpty())
        return;
    const QUrl url = Util::childUrl(parentDir, trimmed);
    KIO::MkdirJob *job = KIO::mkdir(url);
    KIO::FileUndoManager::self()->recordJob(KIO::FileUndoManager::Mkdir, {}, url, job);
    connect(job, &KJob::result, this, [this, parentDir, url](KJob *j) {
        if (j->error() == KJob::NoError)
            Q_EMIT itemsCreated(parentDir, {url});
    });
    watchResult(job, tr("Could not create the folder “%1”").arg(trimmed));
}

void FileOperations::createFile(const QUrl &parentDir, const QString &name)
{
    const QString trimmed = name.trimmed();
    if (!validateName(trimmed).isEmpty())
        return;
    const QUrl url = Util::childUrl(parentDir, trimmed);
    // Without KIO::Overwrite an existing file is reported as an error, never replaced.
    KIO::StoredTransferJob *job = KIO::storedPut(QByteArray(), url, -1, KIO::HideProgressInfo);
    KIO::FileUndoManager::self()->recordJob(KIO::FileUndoManager::Put, {}, url, job);
    connect(job, &KJob::result, this, [this, parentDir, url](KJob *j) {
        if (j->error() == KJob::NoError)
            Q_EMIT itemsCreated(parentDir, {url});
    });
    watchResult(job, tr("Could not create the file “%1”").arg(trimmed));
}

void FileOperations::duplicate(const QList<QUrl> &urls)
{
    for (const QUrl &url : urls) {
        const QUrl dir = Util::parentUrl(url);
        QUrl base(dir);
        if (!base.path().endsWith(u'/'))
            base.setPath(base.path() + u'/');
        const QString name = KFileUtils::suggestName(base, url.adjusted(QUrl::StripTrailingSlash).fileName());
        const QUrl dest = Util::childUrl(dir, name);
        KIO::CopyJob *job = KIO::copyAs(url, dest);
        m_jobs->setJobTitle(job, tr("Duplicating “%1”").arg(Util::displayName(url)));
        KIO::FileUndoManager::self()->recordCopyJob(job);
        connect(job, &KJob::result, this, [this, dir, dest](KJob *j) {
            if (j->error() == KJob::NoError)
                Q_EMIT itemsCreated(dir, {dest});
        });
        watchResult(job, tr("Could not duplicate “%1”").arg(Util::displayName(url)));
    }
}

// ---- clipboard ---------------------------------------------------------------------

void FileOperations::readClipboard()
{
    const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
    QList<QUrl> urls;
    bool cut = false;
    if (mime) {
        urls = mime->urls();
        cut = !urls.isEmpty() && KIO::isClipboardDataCut(mime);
        // Files cut or copied in GTK file managers.
        if (mime->hasFormat(u"x-special/gnome-copied-files"_s)) {
            const QList<QByteArray> lines = mime->data(u"x-special/gnome-copied-files"_s).split('\n');
            if (!lines.isEmpty()) {
                if (lines.first().trimmed() == "cut")
                    cut = true;
                if (urls.isEmpty()) {
                    for (qsizetype i = 1; i < lines.size(); ++i) {
                        const QUrl u = QUrl::fromEncoded(lines.at(i).trimmed());
                        if (u.isValid() && !u.isEmpty())
                            urls << u;
                    }
                }
            }
        }
    }
    const QSet<QUrl> cutSet = cut ? QSet<QUrl>(urls.cbegin(), urls.cend()) : QSet<QUrl>();
    if (cutSet != m_cutSet)
        ++m_cutRevision;
    m_cutSet = cutSet;
    m_clipUrls = urls;
    m_clipCut = cut;
    Q_EMIT clipboardChanged();
}

void FileOperations::copyToClipboard(const QList<QUrl> &urls, bool cut)
{
    if (urls.isEmpty())
        return;
    auto *mime = new QMimeData;
    mime->setUrls(urls);
    QStringList text;
    QByteArray gnome = cut ? "cut" : "copy";
    for (const QUrl &u : urls) {
        text << Util::displayPath(u);
        gnome += '\n' + u.toEncoded();
    }
    mime->setText(text.join(u'\n'));
    mime->setData(u"x-special/gnome-copied-files"_s, gnome);
    KIO::setClipboardDataCut(mime, cut);
    QGuiApplication::clipboard()->setMimeData(mime);
}

void FileOperations::paste(const QUrl &destDir)
{
    if (m_clipUrls.isEmpty() || !destDir.isValid())
        return;
    const QList<QUrl> urls = m_clipUrls;
    const bool allInDest = std::all_of(urls.cbegin(), urls.cend(), [&destDir](const QUrl &u) {
        return Util::sameUrl(Util::parentUrl(u), destDir);
    });
    if (m_clipCut) {
        if (allInDest)
            return;     // moving items into the folder they are in changes nothing
        startCopy(urls, destDir, true, false, true);
    } else {
        // Pasting copies next to their originals keeps both ("name (1)").
        startCopy(urls, destDir, false, allInDest, false);
    }
}

bool FileOperations::isCut(const QUrl &url) const
{
    return m_cutSet.contains(url);
}

// ---- drag and drop -----------------------------------------------------------------

QString FileOperations::dropAction(const QList<QUrl> &urls, const QUrl &destDir) const
{
    if (urls.isEmpty() || !destDir.isValid())
        return {};

    if (isTrashUrl(destDir)) {
        const bool anyOutside = std::any_of(urls.cbegin(), urls.cend(), [](const QUrl &u) { return !isTrashUrl(u); });
        return anyOutside ? u"trash"_s : QString();
    }

    bool allInDest = true;
    bool local = destDir.isLocalFile();
    bool fromTrash = false;
    for (const QUrl &u : urls) {
        // A folder cannot be dropped on itself or into one of its own sub-folders.
        if (Util::sameUrl(u, destDir) || u.isParentOf(destDir))
            return {};
        allInDest = allInDest && Util::sameUrl(Util::parentUrl(u), destDir);
        local = local && u.isLocalFile();
        fromTrash = fromTrash || isTrashUrl(u);
    }

    const Qt::KeyboardModifiers mods = QGuiApplication::queryKeyboardModifiers();
    const bool ctrl = mods.testFlag(Qt::ControlModifier);
    const bool shift = mods.testFlag(Qt::ShiftModifier);
    QString action;
    if (ctrl && shift) {
        action = u"link"_s;
    } else if (ctrl) {
        action = u"copy"_s;
    } else if (shift || fromTrash) {
        action = u"move"_s;
    } else {
        // Default: move within one file system, copy across file systems and protocols.
        bool sameDevice = local;
        dev_t destDevice = 0;
        if (sameDevice)
            sameDevice = deviceOf(destDir.toLocalFile(), &destDevice);
        for (qsizetype i = 0; sameDevice && i < urls.size(); ++i) {
            dev_t d = 0;
            sameDevice = deviceOf(urls.at(i).toLocalFile(), &d) && d == destDevice;
        }
        action = sameDevice ? u"move"_s : u"copy"_s;
    }
    if (allInDest && action != "copy"_L1)
        return {};
    return action;
}

bool FileOperations::drop(const QList<QUrl> &urls, const QUrl &destDir)
{
    const QString action = dropAction(urls, destDir);
    if (action.isEmpty())
        return false;
    if (action == "trash"_L1) {
        QList<QUrl> outside;
        for (const QUrl &u : urls) {
            if (!isTrashUrl(u))
                outside << u;
        }
        trash(outside);
    } else if (action == "link"_L1) {
        linkTo(urls, destDir);
    } else if (action == "move"_L1) {
        startCopy(urls, destDir, true, false, false);
    } else {
        const bool allInDest = std::all_of(urls.cbegin(), urls.cend(), [&destDir](const QUrl &u) {
            return Util::sameUrl(Util::parentUrl(u), destDir);
        });
        startCopy(urls, destDir, false, allInDest, false);
    }
    return true;
}

QString FileOperations::uriList(const QList<QUrl> &urls) const
{
    QString list;
    for (const QUrl &url : urls)
        list += QString::fromLatin1(url.toEncoded()) + u"\r\n"_s;
    return list;
}

QString FileOperations::displayPath(const QUrl &url) const
{
    return Util::displayPath(url);
}

bool FileOperations::sameLocation(const QUrl &a, const QUrl &b) const
{
    return Util::sameUrl(a, b);
}

// ---- names -------------------------------------------------------------------------

QString FileOperations::validateName(const QString &name) const
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return tr("Enter a name.");
    if (trimmed.contains(u'/'))
        return tr("A name cannot contain “/”.");
    if (trimmed == "."_L1 || trimmed == ".."_L1)
        return tr("“%1” is reserved.").arg(trimmed);
    if (trimmed.toUtf8().size() > 255)
        return tr("This name is too long.");
    return {};
}

QString FileOperations::suggestName(const QUrl &dir, const QString &name) const
{
    QUrl base(dir);
    if (!base.path().endsWith(u'/'))
        base.setPath(base.path() + u'/');
    if (base.isLocalFile() && !QFileInfo::exists(base.toLocalFile() + name))
        return name;
    return KFileUtils::suggestName(base, name);
}
