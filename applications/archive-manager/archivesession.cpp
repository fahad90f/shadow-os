// SPDX-License-Identifier: GPL-3.0-or-later
#include "archivesession.h"

#include <command.h>

#include <QCollator>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonObject>
#include <QLocale>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QUrl>
#include <QtConcurrentRun>

#include <cstdio>

namespace {

const qint64 kMiB = 1024 * 1024;
// An archive is called suspicious when it expands to at least this much…
const qint64 kBombMinimumSize = 100 * kMiB;
// …and to at least this many times its own size.
const int kBombRatio = 100;
// Extraction stops when the destination has less free space than this.
const qint64 kFreeSpaceReserve = 32 * kMiB;

QString formatSize(qint64 bytes)
{
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeIecFormat);
}

QString lastLines(const QByteArray &text, int count)
{
    QStringList lines = QString::fromUtf8(text).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (QString &line : lines)
        line = line.trimmed();
    lines.removeAll(QString());
    while (lines.size() > count)
        lines.removeFirst();
    return lines.join(QLatin1Char('\n'));
}

// bsdtar treats its file arguments as shell-style patterns.
QString escapePattern(const QString &path)
{
    QString out;
    for (const QChar c : path) {
        if (c == u'*' || c == u'?' || c == u'[' || c == u']' || c == u'\\')
            out += u'\\';
        out += c;
    }
    return out;
}

QString stripTrailingSlash(QString path)
{
    while (path.size() > 1 && path.endsWith(QLatin1Char('/')))
        path.chop(1);
    return path;
}

// Messages bsdtar prints when it refuses a dangerous entry and carries on.
bool isRefusalLine(const QString &line)
{
    static const char *benign[] = {
        "Path contains '..'", "Cannot extract through symlink", "Removing leading",
        "Error exit delayed from previous errors", "Hard-link target", "Can't create",
        "Could not create", "Cannot extract through", "Path is absolute",
    };
    for (const char *text : benign) {
        if (line.contains(QLatin1String(text)))
            return true;
    }
    return false;
}

// What a tool wrote to stderr, without bsdtar's per-entry progress lines
// ("x name" / "a name"). With -v an error is printed on such a line too
// ("x name: Path contains '..'"), so those are kept.
QStringList errorLines(const QByteArray &err)
{
    QStringList out;
    const QList<QByteArray> lines = err.split('\n');
    for (const QByteArray &raw : lines) {
        QString line = QString::fromUtf8(raw.trimmed());
        if (line.isEmpty())
            continue;
        if (line.startsWith(QLatin1String("x ")) || line.startsWith(QLatin1String("a "))) {
            if (!isRefusalLine(line) && !line.contains(QLatin1String(": ")))
                continue;
            line = line.mid(2);
        }
        out << line;
    }
    return out;
}

} // namespace

ArchiveSession::ArchiveSession(QObject *parent)
    : QObject(parent)
{
    m_guard.setInterval(1000);
    connect(&m_guard, &QTimer::timeout, this, [this] {
        if (!m_process || m_guardPath.isEmpty())
            return;
        const QStorageInfo storage(m_guardPath);
        const qint64 free = storage.bytesAvailable();
        if (free >= 0 && free < kFreeSpaceReserve) {
            m_guardAbort = tr("Extraction was stopped: the destination has less than %1 of free space left.")
                               .arg(formatSize(kFreeSpaceReserve));
        } else if (m_stage) {
            // Sizes in an archive's index can lie. Measure what really arrived.
            const qint64 written = SafeMerge::treeSize(m_stage->path());
            if (written > m_guardExpected + m_guardExpected / 4 + 64 * kMiB) {
                m_guardAbort = tr("Extraction was stopped: the archive unpacks to more data (%1 so far) than it declares (%2). "
                                  "This is typical of a decompression bomb.")
                                   .arg(formatSize(written), formatSize(m_guardExpected));
            }
        }
        if (!m_guardAbort.isEmpty())
            m_process->kill();
    });
}

ArchiveSession::~ArchiveSession()
{
    if (m_cancelFlag)
        m_cancelFlag->store(true);
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(2000);
    }
    if (!m_temporaryOutput.isEmpty())
        QFile::remove(m_temporaryOutput);
}

QString ArchiveSession::archiveName() const
{
    return m_path.isEmpty() ? QString() : QFileInfo(m_path).fileName();
}

bool ArchiveSession::canModify() const
{
    return m_open && m_format.canModify && QFileInfo(m_path).isWritable()
        && QFileInfo(QFileInfo(m_path).absolutePath()).isWritable();
}

QString ArchiveSession::readOnlyReason() const
{
    if (!m_open)
        return {};
    if (!m_format.canModify)
        return tr("%1 archives can be read but not changed by this application.").arg(m_format.label);
    if (!QFileInfo(m_path).isWritable() || !QFileInfo(QFileInfo(m_path).absolutePath()).isWritable())
        return tr("You do not have permission to change this file.");
    return {};
}

bool ArchiveSession::bombSuspected() const
{
    return m_archiveSize > 0 && m_totalSize >= kBombMinimumSize && m_totalSize / m_archiveSize >= kBombRatio;
}

int ArchiveSession::rootItemCount() const
{
    return m_nodes.isEmpty() ? 0 : int(m_nodes.first().children.size());
}

QString ArchiveSession::suggestedFolderName() const
{
    return m_path.isEmpty() ? QString() : Formats::baseName(QFileInfo(m_path).fileName());
}

// ---- state ------------------------------------------------------------------

void ArchiveSession::setBusy(const QString &kind)
{
    m_busy = true;
    m_jobKind = kind;
    m_progress = -1;
    m_progressText.clear();
    m_cancelRequested = false;
    m_guardAbort.clear();
    Q_EMIT busyChanged();
    Q_EMIT progressChanged();
}

void ArchiveSession::setIdle()
{
    m_guard.stop();
    if (!m_busy)
        return;
    m_busy = false;
    m_jobKind.clear();
    Q_EMIT busyChanged();
}

void ArchiveSession::setProgress(qreal value, const QString &text)
{
    m_progress = value;
    if (!text.isNull())
        m_progressText = text;
    Q_EMIT progressChanged();
}

void ArchiveSession::setError(const QString &error)
{
    if (error == m_error)
        return;
    m_error = error;
    Q_EMIT errorChanged();
}

void ArchiveSession::finish(const QString &kind, bool ok, const QString &message, const QStringList &details)
{
    m_stage.reset();
    setIdle();
    Q_EMIT finished(kind, ok, message, details);
}

// ---- running tools ----------------------------------------------------------

void ArchiveSession::startTool(const ToolRun &run)
{
    m_out.clear();
    m_err.clear();
    auto *process = new QProcess(this);
    m_process = process;
    process->setProcessEnvironment(Shadow::Command::parseEnvironment());
    process->setProgram(run.program);
    process->setArguments(run.arguments);
    if (run.discardStdout)
        process->setStandardOutputFile(QProcess::nullDevice());
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process, run] {
        const QByteArray chunk = process->readAllStandardOutput();
        if (run.onStdout)
            run.onStdout(chunk);
        else
            m_out += chunk;
    });
    connect(process, &QProcess::readyReadStandardError, this, [this, process, run] {
        const QByteArray chunk = process->readAllStandardError();
        m_err += chunk;
        if (run.onStderr)
            run.onStderr(chunk);
    });
    auto done = [this, process, run](int code) {
        if (m_process != process)
            return;
        m_process = nullptr;
        m_guard.stop();
        process->deleteLater();
        run.done(code);
    };
    connect(process, &QProcess::finished, this, [process, done](int exitCode, QProcess::ExitStatus status) {
        Q_UNUSED(process)
        done(status == QProcess::NormalExit ? exitCode : -1);
    });
    connect(process, &QProcess::errorOccurred, this, [this, done, run](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_err = tr("The program “%1” could not be started. Is it installed?").arg(run.program).toUtf8();
            done(-1);
        }
    });
    connect(process, &QProcess::started, this, [process, run] {
        // The password (if any) goes to the tool's standard input; closing
        // it makes a tool that asks for one fail instead of waiting.
        if (!run.stdinData.isEmpty())
            process->write(run.stdinData);
        process->closeWriteChannel();
    });
    process->start();
}

QByteArray ArchiveSession::passwordInput(int repeat) const
{
    if (m_password.isEmpty())
        return {};
    QByteArray out;
    for (int i = 0; i < repeat; ++i)
        out += m_password.toUtf8() + '\n';
    return out;
}

bool ArchiveSession::passwordProblem(const QByteArray &out, const QByteArray &err, bool *wrong) const
{
    const QByteArray all = out + '\n' + err;
    const bool wrongPassword = all.contains("Wrong password") || all.contains("Cannot open encrypted archive");
    const bool asked = all.contains("Enter password");
    if (wrongPassword) {
        // 7z reports damaged data of an encrypted entry the same way; with a
        // password in hand it is the password that is in doubt.
        *wrong = !m_password.isEmpty();
        return true;
    }
    // With its normal output switched off (-bso0) the prompt is not visible;
    // what remains is 7z giving up when it cannot read an answer.
    const bool gaveUp = m_encrypted && all.contains("Break signaled");
    if ((asked || gaveUp) && m_password.isEmpty()) {
        *wrong = false;
        return true;
    }
    return false;
}

void ArchiveSession::askPassword(bool wrong)
{
    m_password.clear();
    m_stage.reset();
    setIdle();
    Q_EMIT passwordRequired(wrong);
}

void ArchiveSession::submitPassword(const QString &password)
{
    m_password = password;
    if (m_retry) {
        const auto retry = m_retry;
        retry();
    }
}

void ArchiveSession::cancelPassword()
{
    const bool wasListing = !m_open;
    m_retry = nullptr;
    if (wasListing)
        close();
}

void ArchiveSession::parseSevenZipProgress(const QByteArray &chunk)
{
    static const QRegularExpression pattern(QStringLiteral("(\\d{1,3})%(?: +\\d+)?(?: [-+UT] ([^\\r\\n\\x08]*))?"));
    const QString text = QString::fromUtf8(chunk);
    QRegularExpressionMatch last;
    auto it = pattern.globalMatch(text);
    while (it.hasNext())
        last = it.next();
    if (last.hasMatch())
        setProgress(qBound(0.0, last.captured(1).toInt() / 100.0, 1.0), last.captured(2).trimmed());
}

void ArchiveSession::cancel()
{
    if (!m_busy)
        return;
    m_cancelRequested = true;
    if (m_cancelFlag)
        m_cancelFlag->store(true);
    if (m_process)
        m_process->kill();
}

// ---- opening and listing ----------------------------------------------------

void ArchiveSession::open(const QString &path)
{
    if (m_busy)
        return;
    close();
    const QString absolute = QFileInfo(path).absoluteFilePath();
    QString problem;
    const Formats::Format format = Formats::detect(absolute, &problem);
    if (!format.valid()) {
        setError(problem);
        Q_EMIT finished(QStringLiteral("list"), false, problem, {});
        return;
    }
    if (!Formats::toolAvailable(format.engine)) {
        problem = tr("%1 archives are read with the program “%2”, which is not installed.")
                      .arg(format.label, Formats::toolName(format.engine));
        setError(problem);
        Q_EMIT finished(QStringLiteral("list"), false, problem, {});
        return;
    }
    m_path = absolute;
    m_format = format;
    setError(QString());
    Q_EMIT archiveChanged();
    runList();
}

void ArchiveSession::close()
{
    if (m_busy)
        cancel();
    m_retry = nullptr;
    m_password.clear();
    m_path.clear();
    m_format = {};
    m_open = false;
    m_encrypted = m_headerEncrypted = false;
    m_entries.clear();
    m_nodes.clear();
    m_totalSize = m_archiveSize = 0;
    m_hazardCount = 0;
    m_listWarning.clear();
    m_gzipVerified.clear();
    m_currentDir.clear();
    m_view.clear();
    m_previewDir.reset();       // removes every previewed file
    Q_EMIT archiveChanged();
    Q_EMIT contentsChanged();
    Q_EMIT currentDirChanged();
    Q_EMIT entriesChanged();
}

void ArchiveSession::reload()
{
    if (m_path.isEmpty() || m_busy)
        return;
    runList();
}

void ArchiveSession::runList()
{
    m_retry = [this] { runList(); };
    setBusy(QStringLiteral("list"));
    ToolRun run;
    const bool sevenZip = m_format.engine == Formats::Engine::SevenZip;
    if (sevenZip) {
        run.program = QStringLiteral("7z");
        run.arguments = {QStringLiteral("l"), QStringLiteral("-slt"), QStringLiteral("-sccUTF-8"), QStringLiteral("-bsp0"),
                         QStringLiteral("--"), m_path};
        run.stdinData = passwordInput();
    } else {
        run.program = QStringLiteral("bsdtar");
        run.arguments = {QStringLiteral("-tv"), QStringLiteral("--numeric-owner"), QStringLiteral("-f"), m_path};
    }
    run.done = [this, sevenZip](int code) {
        if (m_cancelRequested) {
            setIdle();
            if (!m_open)
                close();
            return;
        }
        bool wrong = false;
        if (sevenZip && code != 0 && passwordProblem(m_out, m_err, &wrong)) {
            m_headerEncrypted = true;
            askPassword(wrong);
            return;
        }
        Listing::Result result = sevenZip ? Listing::parseSevenZip(m_out) : Listing::parseBsdTar(m_out);
        const bool failed = sevenZip ? (code != 0 && code != 1) : code != 0;
        QString warning;
        if (failed) {
            const QString detail = lastLines(m_err.isEmpty() ? m_out : m_err, 4);
            if (result.entries.isEmpty()) {
                const QString message = tr("The archive could not be read.\n%1").arg(detail);
                setIdle();
                if (!m_open)
                    close();
                setError(message);
                Q_EMIT finished(QStringLiteral("list"), false, message, {});
                return;
            }
            warning = tr("The archive is damaged or incomplete; only the readable part is shown. %1").arg(detail);
        }
        m_entries = result.entries;
        m_headerEncrypted = m_headerEncrypted || result.headerEncrypted;
        m_listWarning = warning;
        m_archiveSize = QFileInfo(m_path).size();
        buildTree();
        m_open = true;
        if (nodeFor(m_currentDir) < 0)
            m_currentDir.clear();
        rebuildView();
        setIdle();
        Q_EMIT archiveChanged();
        Q_EMIT contentsChanged();
        Q_EMIT currentDirChanged();
        Q_EMIT listed();
        Q_EMIT finished(QStringLiteral("list"), true, QString(), {});
    };
    startTool(run);
}

void ArchiveSession::buildTree()
{
    m_nodes.clear();
    m_nodes.append(Node());
    m_nodes[0].directory = true;
    m_totalSize = 0;
    m_hazardCount = 0;
    m_encrypted = m_headerEncrypted;

    for (int index = 0; index < m_entries.size(); ++index) {
        const Listing::Entry &entry = m_entries.at(index);
        m_totalSize += entry.size;
        if (!entry.hazards.isEmpty())
            ++m_hazardCount;
        if (entry.encrypted)
            m_encrypted = true;
        if (entry.parts.isEmpty())
            continue;       // "./" and the like
        // Raw prefixes for folders that have no entry of their own.
        QStringList rawPrefixes;
        {
            const QStringList raw = entry.rawPath.split(QLatin1Char('/'));
            QString prefix;
            for (int i = 0; i < raw.size(); ++i) {
                prefix += (i > 0 ? QStringLiteral("/") : QString()) + raw.at(i);
                if (!raw.at(i).isEmpty() && raw.at(i) != QLatin1String("."))
                    rawPrefixes << prefix;
            }
        }
        int node = 0;
        for (int depth = 0; depth < entry.parts.size(); ++depth) {
            const QString &part = entry.parts.at(depth);
            int child = m_nodes.at(node).children.value(part, -1);
            if (child < 0) {
                child = int(m_nodes.size());
                Node created;
                created.name = part;
                created.parent = node;
                created.directory = true;
                created.rawPath = rawPrefixes.value(depth);
                m_nodes.append(created);
                m_nodes[node].children.insert(part, child);
            }
            node = child;
        }
        Node &leaf = m_nodes[node];
        leaf.entry = index;
        leaf.directory = entry.kind == Listing::Kind::Directory || !leaf.children.isEmpty();
        leaf.rawPath = stripTrailingSlash(entry.rawPath);
        leaf.size += entry.size;
        leaf.count += 1;
        leaf.hazard = leaf.hazard || !entry.hazards.isEmpty();
    }
    // Children were always appended after their parent: one backward pass
    // accumulates sizes.
    for (int i = int(m_nodes.size()) - 1; i > 0; --i) {
        Node &parent = m_nodes[m_nodes.at(i).parent];
        parent.size += m_nodes.at(i).size;
        parent.count += m_nodes.at(i).count;
        parent.hazard = parent.hazard || m_nodes.at(i).hazard;
    }
}

int ArchiveSession::nodeFor(const QString &treePath) const
{
    if (m_nodes.isEmpty())
        return -1;
    int node = 0;
    const QStringList parts = treePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        node = m_nodes.at(node).children.value(part, -1);
        if (node < 0)
            return -1;
    }
    return node;
}

QString ArchiveSession::treePath(int node) const
{
    QStringList parts;
    while (node > 0) {
        parts.prepend(m_nodes.at(node).name);
        node = m_nodes.at(node).parent;
    }
    return parts.join(QLatin1Char('/'));
}

void ArchiveSession::rebuildView()
{
    m_view.clear();
    const int directory = nodeFor(m_currentDir);
    if (directory < 0) {
        Q_EMIT entriesChanged();
        return;
    }
    QList<int> children = m_nodes.at(directory).children.values();
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    const bool ascending = m_sortAscending;
    const QString role = m_sortRole;
    std::sort(children.begin(), children.end(), [&](int a, int b) {
        const Node &x = m_nodes.at(a);
        const Node &y = m_nodes.at(b);
        if (x.directory != y.directory)
            return x.directory;         // folders first, whatever the order
        int order = 0;
        if (role == QLatin1String("size")) {
            order = x.size < y.size ? -1 : x.size > y.size ? 1 : 0;
        } else if (role == QLatin1String("modified")) {
            const QDateTime dx = x.entry >= 0 ? m_entries.at(x.entry).modified : QDateTime();
            const QDateTime dy = y.entry >= 0 ? m_entries.at(y.entry).modified : QDateTime();
            order = dx < dy ? -1 : dx > dy ? 1 : 0;
        }
        if (order == 0)
            order = collator.compare(x.name, y.name);
        return ascending ? order < 0 : order > 0;
    });
    const QString prefix = m_currentDir.isEmpty() ? QString() : m_currentDir + QLatin1Char('/');
    const QLocale locale;
    for (const int child : std::as_const(children)) {
        const Node &node = m_nodes.at(child);
        QVariantMap row{
            {QStringLiteral("name"), node.name},
            {QStringLiteral("path"), prefix + node.name},
            {QStringLiteral("directory"), node.directory},
            {QStringLiteral("size"), qreal(node.size)},
            {QStringLiteral("items"), node.count},
            {QStringLiteral("hazard"), node.hazard},
            {QStringLiteral("kind"), node.directory ? QStringLiteral("directory") : QStringLiteral("file")},
            {QStringLiteral("link"), QString()},
            {QStringLiteral("modified"), QString()},
            {QStringLiteral("packed"), -1.0},
            {QStringLiteral("encrypted"), false},
            {QStringLiteral("hazards"), QString()},
        };
        if (node.entry >= 0) {
            const Listing::Entry &entry = m_entries.at(node.entry);
            if (!node.directory)
                row[QStringLiteral("kind")] = Listing::kindName(entry.kind);
            row[QStringLiteral("link")] = entry.linkTarget;
            row[QStringLiteral("modified")] = entry.modified.isValid() ? locale.toString(entry.modified, QLocale::ShortFormat) : QString();
            row[QStringLiteral("packed")] = qreal(entry.packedSize);
            row[QStringLiteral("encrypted")] = entry.encrypted;
            row[QStringLiteral("hazards")] = entry.hazards.join(QStringLiteral("; "));
        }
        m_view.append(row);
    }
    Q_EMIT entriesChanged();
}

void ArchiveSession::enter(const QString &path)
{
    const int node = nodeFor(path);
    if (node < 0 || !m_nodes.at(node).directory)
        return;
    m_currentDir = treePath(node);
    rebuildView();
    Q_EMIT currentDirChanged();
}

void ArchiveSession::goUp()
{
    if (m_currentDir.isEmpty())
        return;
    const qsizetype slash = m_currentDir.lastIndexOf(QLatin1Char('/'));
    enter(slash < 0 ? QString() : m_currentDir.left(slash));
}

void ArchiveSession::sortBy(const QString &role, bool ascending)
{
    m_sortRole = role;
    m_sortAscending = ascending;
    rebuildView();
}

QJsonArray ArchiveSession::toJson() const
{
    QJsonArray out;
    for (const Listing::Entry &entry : m_entries) {
        out.append(QJsonObject{
            {QStringLiteral("path"), entry.rawPath},
            {QStringLiteral("kind"), Listing::kindName(entry.kind)},
            {QStringLiteral("size"), double(entry.size)},
            {QStringLiteral("packedSize"), double(entry.packedSize)},
            {QStringLiteral("modified"), entry.modified.toString(Qt::ISODate)},
            {QStringLiteral("link"), entry.linkTarget},
            {QStringLiteral("mode"), entry.mode},
            {QStringLiteral("encrypted"), entry.encrypted},
            {QStringLiteral("hazards"), QJsonArray::fromStringList(entry.hazards)},
        });
    }
    return out;
}

QStringList ArchiveSession::rawPathsFor(const QStringList &selection, qint64 *size, int *count) const
{
    QStringList raw;
    qint64 totalSize = 0;
    int totalCount = 0;
    for (const QString &path : selection) {
        const int node = nodeFor(path);
        if (node <= 0)
            continue;
        raw << m_nodes.at(node).rawPath;
        totalSize += m_nodes.at(node).size;
        totalCount += m_nodes.at(node).count;
    }
    if (size) *size = totalSize;
    if (count) *count = totalCount;
    return raw;
}

// ---- extraction -------------------------------------------------------------

QVariantMap ArchiveSession::extractionEstimate(const QStringList &selection, const QString &destination) const
{
    qint64 expanded = m_totalSize;
    int count = int(m_entries.size());
    if (!selection.isEmpty())
        rawPathsFor(selection, &expanded, &count);
    // The nearest existing folder decides which file system is used.
    QString existing = destination;
    while (!existing.isEmpty() && !QFileInfo::exists(existing)) {
        const QString parent = QFileInfo(existing).absolutePath();
        if (parent == existing)
            break;
        existing = parent;
    }
    const QStorageInfo storage(existing);
    const qint64 free = storage.isValid() ? storage.bytesAvailable() : -1;
    return {
        {QStringLiteral("entries"), count},
        {QStringLiteral("expanded"), qreal(expanded)},
        {QStringLiteral("archiveSize"), qreal(m_archiveSize)},
        {QStringLiteral("ratio"), m_archiveSize > 0 ? qreal(m_totalSize) / qreal(m_archiveSize) : 0.0},
        {QStringLiteral("suspicious"), bombSuspected()},
        {QStringLiteral("free"), qreal(free)},
        {QStringLiteral("enough"), free < 0 || expanded + kFreeSpaceReserve <= free},
        {QStringLiteral("hazards"), m_hazardCount},
        {QStringLiteral("writable"), QFileInfo(existing).isDir() && QFileInfo(existing).isWritable()},
    };
}

void ArchiveSession::extract(const QString &destination, const QStringList &selection, const QVariantMap &options)
{
    if (!m_open || m_busy)
        return;
    ExtractRequest request;
    request.destination = QDir::cleanPath(QFileInfo(destination).absoluteFilePath());
    request.selection = selection;
    request.subPath = selection.isEmpty() ? QString() : m_currentDir;
    const QString conflict = options.value(QStringLiteral("conflict")).toString();
    request.conflict = conflict == QLatin1String("overwrite") ? SafeMerge::Conflict::Overwrite
                     : conflict == QLatin1String("skip") ? SafeMerge::Conflict::Skip : SafeMerge::Conflict::Rename;
    runExtract(request);
}

void ArchiveSession::preview(const QString &path)
{
    if (!m_open || m_busy)
        return;
    const int node = nodeFor(path);
    if (node <= 0 || m_nodes.at(node).directory)
        return;
    if (!m_previewDir) {
        // Private (0700) and removed when the archive is closed.
        QString base = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
        if (base.isEmpty())
            base = QDir::tempPath();
        m_previewDir = std::make_unique<QTemporaryDir>(base + QStringLiteral("/shadow-archive-preview-XXXXXX"));
        if (!m_previewDir->isValid()) {
            m_previewDir.reset();
            Q_EMIT finished(QStringLiteral("preview"), false, tr("A temporary folder for the preview could not be created."), {});
            return;
        }
    }
    ExtractRequest request;
    request.preview = true;
    request.previewName = m_nodes.at(node).name;
    request.destination = m_previewDir->filePath(QString::number(++m_previewSerial));
    request.selection = {treePath(node)};
    request.subPath = treePath(m_nodes.at(node).parent);
    request.conflict = SafeMerge::Conflict::Overwrite;
    runExtract(request);
}

void ArchiveSession::runExtract(const ExtractRequest &request)
{
    const QString kind = request.preview ? QStringLiteral("preview") : QStringLiteral("extract");
    m_retry = [this, request] { runExtract(request); };
    setBusy(kind);

    if (!QDir().mkpath(request.destination)) {
        finish(kind, false, tr("The folder “%1” cannot be created.").arg(request.destination));
        return;
    }
    m_stage = std::make_unique<QTemporaryDir>(request.destination + QStringLiteral("/.shadow-extract-XXXXXX"));
    if (!m_stage->isValid()) {
        finish(kind, false, tr("Cannot write to “%1”: %2").arg(request.destination, m_stage->errorString()));
        return;
    }

    qint64 expected = m_totalSize;
    int count = int(m_entries.size());
    const QStringList rawPaths = rawPathsFor(request.selection, request.selection.isEmpty() ? nullptr : &expected,
                                             request.selection.isEmpty() ? nullptr : &count);
    if (!request.selection.isEmpty() && rawPaths.isEmpty()) {
        finish(kind, false, tr("Nothing is selected."));
        return;
    }
    m_progressDone = 0;
    m_progressTotal = qMax(1, count);
    m_guardExpected = expected;
    m_guardPath = request.destination;
    m_guardStartFree = QStorageInfo(request.destination).bytesAvailable();

    ToolRun run;
    const bool sevenZip = m_format.engine == Formats::Engine::SevenZip;
    if (sevenZip) {
        run.program = QStringLiteral("7z");
        run.arguments = {QStringLiteral("x"), QStringLiteral("-y"), QStringLiteral("-bso0"), QStringLiteral("-bsp1"),
                         QStringLiteral("-sccUTF-8"), QStringLiteral("-o") + m_stage->path(), QStringLiteral("--"), m_path};
        run.arguments += rawPaths;
        run.stdinData = passwordInput();
        run.onStdout = [this](const QByteArray &chunk) {
            m_out += chunk;
            if (m_out.size() > 65536)
                m_out = m_out.right(8192);
            parseSevenZipProgress(chunk);
        };
    } else {
        run.program = QStringLiteral("bsdtar");
        run.arguments = {QStringLiteral("-x"), QStringLiteral("-v"), QStringLiteral("-f"), m_path,
                         QStringLiteral("-C"), m_stage->path(),
                         QStringLiteral("--no-same-owner"), QStringLiteral("--no-same-permissions"),
                         QStringLiteral("--no-acls"), QStringLiteral("--no-xattrs"), QStringLiteral("--no-fflags")};
        for (const QString &raw : rawPaths)
            run.arguments << escapePattern(raw);
        run.onStderr = [this](const QByteArray &chunk) {
            const QList<QByteArray> lines = chunk.split('\n');
            QByteArray lastName;
            for (const QByteArray &line : lines) {
                if (line.startsWith("x ")) {
                    ++m_progressDone;
                    lastName = line.mid(2);
                }
            }
            if (!lastName.isEmpty())
                setProgress(qMin(1.0, qreal(m_progressDone) / m_progressTotal), QString::fromUtf8(lastName));
        };
    }
    run.done = [this, request, kind, sevenZip](int code) {
        if (m_cancelRequested) {
            finish(kind, false, tr("Cancelled. Nothing was added to “%1”.").arg(request.destination));
            return;
        }
        if (!m_guardAbort.isEmpty()) {
            finish(kind, false, m_guardAbort);
            return;
        }
        bool wrong = false;
        if (sevenZip && code != 0 && passwordProblem(m_out, m_err, &wrong)) {
            askPassword(wrong);
            return;
        }
        QStringList warnings;
        if (sevenZip) {
            if (code != 0 && code != 1) {
                finish(kind, false, tr("The archive could not be extracted; nothing was added to “%1”.\n%2")
                                        .arg(request.destination, lastLines(m_err.isEmpty() ? m_out : m_err, 6)));
                return;
            }
            if (code == 1)
                warnings = errorLines(m_err);
        } else if (code != 0) {
            const QStringList lines = errorLines(m_err);
            bool onlyRefusals = !lines.isEmpty();
            for (const QString &line : lines)
                onlyRefusals = onlyRefusals && isRefusalLine(line);
            if (!onlyRefusals) {
                finish(kind, false, tr("The archive could not be extracted; nothing was added to “%1”.\n%2")
                                        .arg(request.destination, lastLines(m_err, 6)));
                return;
            }
            warnings = lines;
            // Not refusals of an entry: a summary line, and the note that a
            // leading '/' was dropped (that entry is extracted, inside the destination).
            warnings.removeIf([](const QString &line) {
                return line.contains(QLatin1String("Error exit delayed")) || line.contains(QLatin1String("Removing leading"));
            });
        }
        finishExtract(request, warnings);
    };
    verifyGzip(kind, [this, run] {
        startTool(run);
        m_guard.start();
    });
}

void ArchiveSession::finishExtract(const ExtractRequest &request, const QStringList &toolWarnings)
{
    const QString kind = request.preview ? QStringLiteral("preview") : QStringLiteral("extract");
    setProgress(-1, tr("Moving files into place…"));
    m_cancelFlag = std::make_shared<std::atomic_bool>(false);
    const auto cancelFlag = m_cancelFlag;
    const QString stagePath = m_stage->path();
    auto *watcher = new QFutureWatcher<SafeMerge::Report>(this);
    connect(watcher, &QFutureWatcher<SafeMerge::Report>::finished, this, [this, watcher, request, toolWarnings, kind] {
        watcher->deleteLater();
        const SafeMerge::Report report = watcher->result();
        m_cancelFlag.reset();
        QStringList details = report.refused;
        for (const QString &warning : toolWarnings)
            details << tr("Not extracted by the tool: %1").arg(warning);
        if (!report.error.isEmpty()) {
            finish(kind, false, report.error, details);
            return;
        }
        if (report.cancelled) {
            finish(kind, false, tr("Cancelled. Some files were already placed in “%1”.").arg(request.destination), details);
            return;
        }
        if (request.preview) {
            const QString file = request.destination + QLatin1Char('/') + request.previewName;
            const QFileInfo info(file);
            if (!info.isFile() || info.isSymLink()) {
                finish(kind, false, tr("“%1” could not be extracted for preview.").arg(request.previewName), details);
                return;
            }
            const QMimeType mime = QMimeDatabase().mimeTypeForFile(file);
            if (mime.inherits(QStringLiteral("application/x-executable")) || mime.inherits(QStringLiteral("application/x-sharedlib"))
                || mime.name() == QLatin1String("application/x-desktop")) {
                finish(kind, false, tr("“%1” is a program or launcher (%2). It is not opened from inside an archive; "
                                       "extract it first if you trust it.").arg(request.previewName, mime.name()), details);
                return;
            }
            QFile::setPermissions(file, QFileDevice::ReadOwner);
            // QDesktopServices reports success even when nothing can open the
            // file, so the launcher is run here and its exit status checked.
            QString launcher;
            QStringList arguments;
            if (Shadow::Command::available(QStringLiteral("xdg-open"))) {
                launcher = QStringLiteral("xdg-open");
                arguments = {file};
            } else if (Shadow::Command::available(QStringLiteral("gio"))) {
                launcher = QStringLiteral("gio");
                arguments = {QStringLiteral("open"), file};
            } else {
                finish(kind, false, tr("“%1” was extracted to a temporary folder, but no program that opens files with their "
                                       "default application (xdg-open) is installed.").arg(request.previewName), details);
                return;
            }
            const QString name = request.previewName;
            const QString mimeName = mime.name();
            setProgress(-1, tr("Opening…"));
            Shadow::Command::run(this, launcher, arguments, [this, kind, name, mimeName, details](const Shadow::CommandResult &result) {
                if (result.ok())
                    finish(kind, true, tr("Opened a temporary, read-only copy of “%1”.").arg(name), details);
                else
                    finish(kind, false, tr("No application is set up to open “%1” (%2).").arg(name, mimeName),
                           details + QStringList{result.errText().trimmed()});
            }, 20000);
            return;
        }
        QString message = tr("Extracted to “%1”: %2 files and folders").arg(request.destination).arg(report.files + report.links + report.directories);
        message += tr(" (%1).").arg(formatSize(report.bytes));
        if (report.skippedExisting > 0)
            message += QLatin1Char(' ') + tr("Existing files kept: %1.").arg(report.skippedExisting);
        if (report.renamed > 0)
            message += QLatin1Char(' ') + tr("Saved under a new name next to existing files: %1.").arg(report.renamed);
        if (!details.isEmpty())
            message += QLatin1Char(' ') + tr("Not extracted for safety: %1 — see the details.").arg(details.size());
        finish(kind, true, message, details);
    });
    const QString subPath = request.subPath;
    const QString destination = request.destination;
    const SafeMerge::Conflict conflict = request.conflict;
    watcher->setFuture(QtConcurrent::run([stagePath, subPath, destination, conflict, cancelFlag] {
        return SafeMerge::merge(stagePath, subPath, destination, conflict, *cancelFlag);
    }));
}

// ---- integrity test ---------------------------------------------------------

// libarchive does not check the CRC-32 at the end of a gzip stream, so a
// damaged .tar.gz can unpack "successfully" into wrong data. 7z does check
// it; run that first where it matters. Other compressors (bzip2, xz, zstd)
// are verified by their own libraries while bsdtar reads them.
void ArchiveSession::verifyGzip(const QString &kind, const std::function<void()> &next)
{
    const QFileInfo info(m_path);
    const QString stamp = QString::number(info.size()) + QLatin1Char('/') + QString::number(info.lastModified().toMSecsSinceEpoch());
    if (m_format.id != QLatin1String("tar.gz") || m_gzipVerified == stamp || !Shadow::Command::available(QStringLiteral("7z"))) {
        next();
        return;
    }
    setProgress(-1, tr("Checking the gzip checksum…"));
    ToolRun run;
    run.program = QStringLiteral("7z");
    run.arguments = {QStringLiteral("t"), QStringLiteral("-bso0"), QStringLiteral("-bsp1"), QStringLiteral("-tgzip"), QStringLiteral("--"), m_path};
    run.onStdout = [this](const QByteArray &chunk) { parseSevenZipProgress(chunk); };
    run.done = [this, kind, next, stamp](int code) {
        if (m_cancelRequested) {
            finish(kind, false, tr("Cancelled."));
            return;
        }
        if (code != 0) {
            finish(kind, false, tr("The archive is damaged: its gzip checksum does not match. Nothing was extracted."),
                   errorLines(m_err));
            return;
        }
        m_gzipVerified = stamp;
        setProgress(-1, QString());
        next();
    };
    startTool(run);
}

void ArchiveSession::test()
{
    if (!m_open || m_busy)
        return;
    runTest();
}

void ArchiveSession::runTest()
{
    m_retry = [this] { runTest(); };
    setBusy(QStringLiteral("test"));
    ToolRun run;
    const bool sevenZip = m_format.engine == Formats::Engine::SevenZip;
    if (sevenZip) {
        run.program = QStringLiteral("7z");
        run.arguments = {QStringLiteral("t"), QStringLiteral("-bso0"), QStringLiteral("-bsp1"), QStringLiteral("--"), m_path};
        run.stdinData = passwordInput();
        run.onStdout = [this](const QByteArray &chunk) {
            m_out += chunk;
            if (m_out.size() > 65536)
                m_out = m_out.right(8192);
            parseSevenZipProgress(chunk);
        };
    } else {
        // Reading every entry to the end makes libarchive verify the
        // archive structure and (except for gzip, see verifyGzip) the
        // checksums of the compression stream; the data is discarded.
        run.program = QStringLiteral("bsdtar");
        run.arguments = {QStringLiteral("-x"), QStringLiteral("-O"), QStringLiteral("-f"), m_path};
        run.discardStdout = true;
    }
    run.done = [this, sevenZip](int code) {
        const QString kind = QStringLiteral("test");
        if (m_cancelRequested) {
            finish(kind, false, tr("The test was cancelled."));
            return;
        }
        bool wrong = false;
        if (sevenZip && code != 0 && passwordProblem(m_out, m_err, &wrong)) {
            askPassword(wrong);
            return;
        }
        if (code == 0) {
            finish(kind, true, tr("No errors found. Entries read and verified: %1.").arg(m_entries.size()));
        } else {
            finish(kind, false, tr("The archive is damaged."), errorLines(m_err.isEmpty() ? m_out : m_err));
        }
    };
    verifyGzip(QStringLiteral("test"), [this, run] { startTool(run); });
}

// ---- changing an archive ----------------------------------------------------

QStringList ArchiveSession::tarWriteOptions(const Formats::Format &format, const QString &level) const
{
    QStringList options;
    auto pick = [&level](int fast, int normal, int best) {
        return level == QLatin1String("fast") ? fast : level == QLatin1String("best") ? best : normal;
    };
    if (format.id == QLatin1String("tar.gz"))
        options << QStringLiteral("--gzip") << QStringLiteral("--options") << QStringLiteral("gzip:compression-level=%1").arg(pick(1, 6, 9));
    else if (format.id == QLatin1String("tar.bz2"))
        options << QStringLiteral("--bzip2") << QStringLiteral("--options") << QStringLiteral("bzip2:compression-level=%1").arg(pick(1, 9, 9));
    else if (format.id == QLatin1String("tar.xz"))
        options << QStringLiteral("--xz") << QStringLiteral("--options") << QStringLiteral("xz:compression-level=%1").arg(pick(1, 6, 9));
    else if (format.id == QLatin1String("tar.zst"))
        options << QStringLiteral("--zstd") << QStringLiteral("--options") << QStringLiteral("zstd:compression-level=%1").arg(pick(1, 3, 19));
    else if (format.id == QLatin1String("cpio"))
        options << QStringLiteral("--format=newc");
    return options;
}

// "-C <folder> <name>" pairs, so entries are stored by name only.
QStringList ArchiveSession::tarInputArguments(const QStringList &paths)
{
    QStringList arguments;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        QString name = info.fileName();
        // A leading '-' would read as an option, '@' as "include this archive".
        if (name.startsWith(QLatin1Char('-')) || name.startsWith(QLatin1Char('@')))
            name.prepend(QStringLiteral("./"));
        arguments << QStringLiteral("-C") << info.absolutePath() << name;
    }
    return arguments;
}

void ArchiveSession::replaceArchive(const QString &temporary, const QString &kind, const QString &message)
{
    // rename() replaces the archive atomically; the old one stays intact
    // until the new one is complete.
    const QFileInfo original(m_path);
    QFile::setPermissions(temporary, original.permissions());
    if (::rename(QFile::encodeName(temporary).constData(), QFile::encodeName(m_path).constData()) != 0) {
        QFile::remove(temporary);
        m_temporaryOutput.clear();
        finish(kind, false, tr("The changed archive could not replace “%1”.").arg(m_path));
        return;
    }
    m_temporaryOutput.clear();
    finish(kind, true, message);
    runList();
}

void ArchiveSession::deleteEntries(const QStringList &selection)
{
    if (!canModify() || m_busy || selection.isEmpty())
        return;
    runDelete(selection);
}

void ArchiveSession::runDelete(const QStringList &selection)
{
    const QString kind = QStringLiteral("delete");
    m_retry = [this, selection] { runDelete(selection); };
    int count = 0;
    const QStringList rawPaths = rawPathsFor(selection, nullptr, &count);
    if (rawPaths.isEmpty())
        return;
    setBusy(kind);
    const QString message = tr("Deleted from the archive: %1 entries (folders counted with their contents).").arg(count);
    ToolRun run;
    if (m_format.engine == Formats::Engine::SevenZip) {
        run.program = QStringLiteral("7z");
        run.arguments = {QStringLiteral("d"), QStringLiteral("-bso0"), QStringLiteral("-bsp1"), QStringLiteral("-sccUTF-8"),
                         QStringLiteral("--"), m_path};
        run.arguments += rawPaths;
        run.stdinData = passwordInput();
        run.onStdout = [this](const QByteArray &chunk) { m_out += chunk; parseSevenZipProgress(chunk); };
        run.done = [this, kind, message](int code) {
            bool wrong = false;
            if (m_cancelRequested) {
                finish(kind, false, tr("Cancelled; the archive was not changed."));
            } else if (code != 0 && passwordProblem(m_out, m_err, &wrong)) {
                askPassword(wrong);
            } else if (code != 0) {
                finish(kind, false, tr("The entries could not be deleted.\n%1").arg(lastLines(m_err.isEmpty() ? m_out : m_err, 5)));
            } else {
                finish(kind, true, message);
                runList();
            }
        };
    } else {
        // Tar has no index to edit: write a new archive without the entries
        // and put it in place of the old one.
        const QFileInfo info(m_path);
        m_temporaryOutput = info.absolutePath() + QStringLiteral("/.") + info.fileName() + QStringLiteral(".shadow-part");
        QFile::remove(m_temporaryOutput);
        run.program = QStringLiteral("bsdtar");
        run.arguments = {QStringLiteral("-c"), QStringLiteral("-f"), m_temporaryOutput};
        run.arguments += tarWriteOptions(m_format, QStringLiteral("normal"));
        for (const QString &raw : rawPaths)
            run.arguments << QStringLiteral("--exclude") << QStringLiteral("^") + escapePattern(raw);
        run.arguments << QStringLiteral("@") + m_path;
        const QString temporary = m_temporaryOutput;
        run.done = [this, kind, message, temporary](int code) {
            if (m_cancelRequested || code != 0) {
                QFile::remove(temporary);
                m_temporaryOutput.clear();
                finish(kind, false, m_cancelRequested ? tr("Cancelled; the archive was not changed.")
                                                      : tr("The entries could not be deleted; the archive was not changed.\n%1").arg(lastLines(m_err, 5)));
                return;
            }
            replaceArchive(temporary, kind, message);
        };
    }
    startTool(run);
}

void ArchiveSession::addFiles(const QStringList &paths)
{
    if (!canModify() || m_busy || paths.isEmpty())
        return;
    runAdd(paths);
}

void ArchiveSession::runAdd(const QStringList &paths)
{
    const QString kind = QStringLiteral("add");
    m_retry = [this, paths] { runAdd(paths); };
    QStringList absolute;
    QStringList names;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (!info.exists() && !info.isSymLink()) {
            Q_EMIT finished(kind, false, tr("“%1” does not exist.").arg(path), {});
            return;
        }
        if (info.absoluteFilePath() == m_path) {
            Q_EMIT finished(kind, false, tr("An archive cannot be added to itself."), {});
            return;
        }
        if (names.contains(info.fileName())) {
            Q_EMIT finished(kind, false, tr("Two of the chosen items are called “%1”; add them one at a time.").arg(info.fileName()), {});
            return;
        }
        names << info.fileName();
        absolute << info.absoluteFilePath();
    }
    const bool sevenZip = m_format.engine == Formats::Engine::SevenZip;
    if (!sevenZip) {
        // A rewritten tar would end up with two entries of the same name.
        for (const QString &name : std::as_const(names)) {
            if (m_nodes.first().children.contains(name)) {
                Q_EMIT finished(kind, false, tr("The archive already contains “%1”. Delete it from the archive first.").arg(name), {});
                return;
            }
        }
    }
    if (sevenZip && m_encrypted && m_password.isEmpty()) {
        // Without the password the new files would be stored unencrypted
        // next to encrypted ones.
        Q_EMIT passwordRequired(false);
        return;
    }
    setBusy(kind);
    const QString message = tr("Added to the archive: %1.").arg(absolute.size());
    ToolRun run;
    if (sevenZip) {
        run.program = QStringLiteral("7z");
        run.arguments = {QStringLiteral("a"), QStringLiteral("-bso0"), QStringLiteral("-bsp1"), QStringLiteral("-sccUTF-8"),
                         QStringLiteral("-snl"), QStringLiteral("-t") + m_format.id};
        if (!m_password.isEmpty()) {
            run.arguments << QStringLiteral("-p");
            if (m_format.id == QLatin1String("zip"))
                run.arguments << QStringLiteral("-mem=AES256");
            else if (m_headerEncrypted)
                run.arguments << QStringLiteral("-mhe=on");
        }
        run.arguments << QStringLiteral("--") << m_path;
        run.arguments += absolute;
        // The first line answers an "open the archive" prompt or the "new
        // password" prompt; the later ones any further prompt.
        run.stdinData = passwordInput(3);
        run.onStdout = [this](const QByteArray &chunk) { m_out += chunk; parseSevenZipProgress(chunk); };
        run.done = [this, kind, message](int code) {
            bool wrong = false;
            if (m_cancelRequested) {
                finish(kind, false, tr("Cancelled; the archive was not changed."));
            } else if (code != 0 && passwordProblem(m_out, m_err, &wrong)) {
                askPassword(wrong);
            } else if (code != 0 && code != 1) {
                finish(kind, false, tr("The files could not be added.\n%1").arg(lastLines(m_err.isEmpty() ? m_out : m_err, 5)));
            } else {
                finish(kind, true, message, code == 1 ? errorLines(m_err) : QStringList());
                runList();
            }
        };
    } else {
        const QFileInfo info(m_path);
        m_temporaryOutput = info.absolutePath() + QStringLiteral("/.") + info.fileName() + QStringLiteral(".shadow-part");
        QFile::remove(m_temporaryOutput);
        run.program = QStringLiteral("bsdtar");
        run.arguments = {QStringLiteral("-c"), QStringLiteral("-v"), QStringLiteral("-f"), m_temporaryOutput};
        run.arguments += tarWriteOptions(m_format, QStringLiteral("normal"));
        run.arguments << QStringLiteral("@") + m_path;
        run.arguments += tarInputArguments(absolute);
        const QString temporary = m_temporaryOutput;
        run.onStderr = [this](const QByteArray &chunk) {
            const qsizetype at = chunk.lastIndexOf("a ");
            if (at >= 0)
                setProgress(-1, QString::fromUtf8(chunk.mid(at + 2).split('\n').first()));
        };
        run.done = [this, kind, message, temporary](int code) {
            if (m_cancelRequested || code != 0) {
                QFile::remove(temporary);
                m_temporaryOutput.clear();
                finish(kind, false, m_cancelRequested ? tr("Cancelled; the archive was not changed.")
                                                      : tr("The files could not be added; the archive was not changed.\n%1").arg(lastLines(m_err, 5)));
                return;
            }
            replaceArchive(temporary, kind, message);
        };
    }
    startTool(run);
}

// ---- creating ---------------------------------------------------------------

void ArchiveSession::create(const QString &output, const QStringList &inputs, const QVariantMap &options)
{
    if (m_busy)
        return;
    runCreate(output, inputs, options);
}

void ArchiveSession::runCreate(const QString &output, const QStringList &inputs, const QVariantMap &options)
{
    const QString kind = QStringLiteral("create");
    const QString target = QFileInfo(output).absoluteFilePath();
    Formats::Format format = Formats::byId(options.value(QStringLiteral("format")).toString());
    if (!format.valid())
        format = Formats::fromFileName(QFileInfo(target).fileName());
    auto fail = [this, kind](const QString &message) { Q_EMIT finished(kind, false, message, {}); };
    if (!format.valid() || !format.canCreate) {
        fail(tr("Choose a format this application can create: %1.").arg(QStringLiteral("zip, 7z, tar, tar.gz, tar.bz2, tar.xz, tar.zst, cpio")));
        return;
    }
    if (!Formats::toolAvailable(format.engine)) {
        fail(tr("%1 archives are written with the program “%2”, which is not installed.").arg(format.label, Formats::toolName(format.engine)));
        return;
    }
    if (inputs.isEmpty()) {
        fail(tr("Choose at least one file or folder to put into the archive."));
        return;
    }
    QStringList absolute, names;
    for (const QString &path : inputs) {
        const QFileInfo info(path);
        if (!info.exists() && !info.isSymLink()) {
            fail(tr("“%1” does not exist.").arg(path));
            return;
        }
        if (info.absoluteFilePath() == target) {
            fail(tr("The archive cannot contain itself."));
            return;
        }
        if (names.contains(info.fileName())) {
            fail(tr("Two of the chosen items are called “%1”. Put them in a folder and archive that instead.").arg(info.fileName()));
            return;
        }
        names << info.fileName();
        absolute << info.absoluteFilePath();
    }
    const QString password = options.value(QStringLiteral("password")).toString();
    if (!password.isEmpty() && !format.canEncrypt) {
        fail(tr("%1 archives cannot be protected with a password.").arg(format.label));
        return;
    }
    if (password.contains(QLatin1Char('\n'))) {
        fail(tr("The password cannot contain a line break."));
        return;
    }
    const QString level = options.value(QStringLiteral("level"), QStringLiteral("normal")).toString();
    const QFileInfo targetInfo(target);
    if (!QFileInfo(targetInfo.absolutePath()).isDir()) {
        fail(tr("The folder “%1” does not exist.").arg(targetInfo.absolutePath()));
        return;
    }
    // Written under a temporary name and renamed when complete, so a failed
    // or cancelled run never leaves a half-written (or replaces a good) file.
    m_temporaryOutput = targetInfo.absolutePath() + QStringLiteral("/.") + targetInfo.fileName() + QStringLiteral(".shadow-part.")
                        + format.extensions.first();
    QFile::remove(m_temporaryOutput);
    const QString temporary = m_temporaryOutput;

    setBusy(kind);
    ToolRun run;
    if (format.engine == Formats::Engine::SevenZip) {
        const int mx = level == QLatin1String("store") ? 0 : level == QLatin1String("fast") ? 1 : level == QLatin1String("best") ? 9 : 5;
        run.program = QStringLiteral("7z");
        run.arguments = {QStringLiteral("a"), QStringLiteral("-bso0"), QStringLiteral("-bsp1"), QStringLiteral("-sccUTF-8"),
                         QStringLiteral("-snl"), QStringLiteral("-t") + format.id, QStringLiteral("-mx=%1").arg(mx)};
        if (!password.isEmpty()) {
            // "-p" without a value: 7z asks, and reads the answer from its
            // standard input — the password never appears in a process list.
            run.arguments << QStringLiteral("-p");
            if (format.id == QLatin1String("zip"))
                run.arguments << QStringLiteral("-mem=AES256");
            else if (options.value(QStringLiteral("encryptNames")).toBool())
                run.arguments << QStringLiteral("-mhe=on");
            run.stdinData = password.toUtf8() + '\n' + password.toUtf8() + '\n';
        }
        run.arguments << QStringLiteral("--") << temporary;
        run.arguments += absolute;
        run.onStdout = [this](const QByteArray &chunk) { m_out += chunk; parseSevenZipProgress(chunk); };
    } else {
        run.program = QStringLiteral("bsdtar");
        run.arguments = {QStringLiteral("-c"), QStringLiteral("-v"), QStringLiteral("-f"), temporary};
        run.arguments += tarWriteOptions(format, level);
        run.arguments += tarInputArguments(absolute);
        run.onStderr = [this](const QByteArray &chunk) {
            const QList<QByteArray> lines = chunk.split('\n');
            for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
                if (it->startsWith("a ")) {
                    setProgress(-1, QString::fromUtf8(it->mid(2)));
                    break;
                }
            }
        };
    }
    run.done = [this, kind, temporary, target, format](int code) {
        const bool sevenZip = format.engine == Formats::Engine::SevenZip;
        const bool ok = !m_cancelRequested && (code == 0 || (sevenZip && code == 1)) && QFileInfo(temporary).isFile();
        if (!ok) {
            QFile::remove(temporary);
            m_temporaryOutput.clear();
            finish(kind, false, m_cancelRequested ? tr("Cancelled; no archive was created.")
                                                  : tr("The archive could not be created.\n%1").arg(lastLines(m_err.isEmpty() ? m_out : m_err, 6)));
            return;
        }
        if (::rename(QFile::encodeName(temporary).constData(), QFile::encodeName(target).constData()) != 0) {
            QFile::remove(temporary);
            m_temporaryOutput.clear();
            finish(kind, false, tr("The finished archive could not be saved as “%1”.").arg(target));
            return;
        }
        m_temporaryOutput.clear();
        finish(kind, true, tr("Created “%1” (%2).").arg(QFileInfo(target).fileName(), formatSize(QFileInfo(target).size())),
               sevenZip && code == 1 ? errorLines(m_err) : QStringList());
    };
    startTool(run);
}
