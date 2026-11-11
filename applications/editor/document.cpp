// SPDX-License-Identifier: GPL-3.0-or-later
#include "document.h"
#include "codeeditor.h"
#include "textfile.h"

#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/SyntaxHighlighter>
#include <KSyntaxHighlighting/Theme>

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QMimeDatabase>
#include <QSaveFile>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QtConcurrentRun>

namespace {

struct LoadResult
{
    TextFile::Decoded decoded;
    QDateTime modified;
    qint64 size = -1;
};

struct SaveResult
{
    QString error;
    QDateTime modified;
    qint64 size = -1;
};

QString formatSize(qint64 bytes)
{
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeIecFormat);
}

LoadResult readFile(const QString &path, const QString &encoding)
{
    LoadResult r;
    const QFileInfo info(path);
    if (info.isDir()) {
        r.decoded.error = EditorDocument::tr("“%1” is a folder.").arg(path);
        return r;
    }
    if (!info.exists()) {
        r.decoded.error = EditorDocument::tr("“%1” does not exist.").arg(path);
        return r;
    }
    if (!info.isFile()) {
        r.decoded.error = EditorDocument::tr("“%1” is not a regular file (device, pipe or socket).").arg(path);
        return r;
    }
    if (info.size() > EditorLimits::maxFileBytes) {
        r.decoded.error = EditorDocument::tr("“%1” is %2. This editor loads a file completely into memory (taking several "
                                            "times its size) and opening would freeze the window for a long time, so files "
                                            "above %3 are not opened.")
                              .arg(info.fileName(), formatSize(info.size()), formatSize(EditorLimits::maxFileBytes));
        return r;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        r.decoded.error = EditorDocument::tr("Cannot read “%1”: %2").arg(path, file.errorString());
        return r;
    }
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        r.decoded.error = EditorDocument::tr("Cannot read “%1”: %2").arg(path, file.errorString());
        return r;
    }
    r.modified = info.lastModified();
    r.size = bytes.size();
    r.decoded = TextFile::decode(bytes, encoding);
    return r;
}

SaveResult writeBytes(const QString &path, const QByteArray &bytes)
{
    SaveResult r;
    QSaveFile file(path);
    // Directories the user may not create files in (but may write the file
    // itself) cannot take a temporary file; write in place there.
    file.setDirectWriteFallback(true);
    if (!file.open(QIODevice::WriteOnly)) {
        r.error = EditorDocument::tr("Cannot write “%1”: %2").arg(path, file.errorString());
        return r;
    }
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        r.error = EditorDocument::tr("Cannot write “%1”: %2").arg(path, file.errorString());
        return r;
    }
    const QFileInfo info(path);
    r.modified = info.lastModified();
    r.size = info.size();
    return r;
}

QString resourceRoot()
{
    for (const QString &root : {QStringLiteral(":/qt/qml/org/shadow/editor"), QStringLiteral(":/org/shadow/editor")}) {
        if (QFileInfo::exists(root + QStringLiteral("/themes")))
            return root;
    }
    return {};
}

int leadingWhitespace(const QString &text)
{
    int i = 0;
    while (i < text.size() && (text.at(i) == u' ' || text.at(i) == u'\t'))
        ++i;
    return i;
}

int visualColumn(const QString &text, int upTo, int width)
{
    int column = 0;
    for (int i = 0; i < upTo && i < text.size(); ++i)
        column = text.at(i) == u'\t' ? (column / width + 1) * width : column + 1;
    return column;
}

} // namespace

KSyntaxHighlighting::Repository &syntaxRepository()
{
    static KSyntaxHighlighting::Repository *repository = [] {
        auto *r = new KSyntaxHighlighting::Repository;
        const QString root = resourceRoot();
        if (!root.isEmpty())
            r->addCustomSearchPath(root);
        return r;
    }();
    return *repository;
}

EditorDocument::EditorDocument(QObject *parent)
    : QObject(parent)
{
    m_diskTimer.setSingleShot(true);
    m_diskTimer.setInterval(250);
    connect(&m_diskTimer, &QTimer::timeout, this, &EditorDocument::checkDisk);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_diskTimer.start(); });

    m_matchTimer.setSingleShot(true);
    m_matchTimer.setInterval(200);
    connect(&m_matchTimer, &QTimer::timeout, this, [this] {
        if (m_matchesDirty)
            refreshMatches();
    });
}

EditorDocument::~EditorDocument()
{
    if (m_editor)
        m_editor->setMatches(nullptr);
}

QTextDocument *EditorDocument::doc() const
{
    return m_editor ? m_editor->textDocument() : nullptr;
}

void EditorDocument::setEditor(CodeEditor *editor)
{
    if (m_editor == editor)
        return;
    if (QTextDocument *old = doc())
        old->disconnect(this);
    if (m_editor)
        m_editor->setMatches(nullptr);
    delete m_highlighter;
    m_highlighter = nullptr;
    m_editor = editor;
    if (QTextDocument *d = doc()) {
        connect(d, &QTextDocument::modificationChanged, this, &EditorDocument::modifiedChanged);
        connect(d, &QTextDocument::blockCountChanged, this, &EditorDocument::lineCountChanged);
        connect(d, &QTextDocument::contentsChanged, this, [this] {
            if (!m_pattern.isEmpty()) {
                m_matchesDirty = true;
                m_matchTimer.start();
            }
        });
        m_highlighter = new KSyntaxHighlighting::SyntaxHighlighter(d);
        applyTheme();
        applyDefinition();
        m_editor->setMatches(&m_matches);
    }
    Q_EMIT editorChanged();
    Q_EMIT modifiedChanged();
    Q_EMIT lineCountChanged();
}

QString EditorDocument::fileName() const
{
    return m_filePath.isEmpty() ? tr("Untitled") : QFileInfo(m_filePath).fileName();
}

bool EditorDocument::modified() const
{
    const QTextDocument *d = doc();
    return m_formatEdited || (d && d->isModified());
}

int EditorDocument::lineCount() const
{
    const QTextDocument *d = doc();
    return d ? d->blockCount() : 1;
}

QString EditorDocument::plainText() const
{
    const QTextDocument *d = doc();
    if (!d)
        return {};
    // toRawText(), not toPlainText(): the latter turns no-break spaces into
    // ordinary spaces, which would silently change the file.
    QString text = d->toRawText();
    text.replace(QChar(QChar::ParagraphSeparator), QLatin1Char('\n'));
    return text;
}

// ---- format -----------------------------------------------------------------

void EditorDocument::markFormatEdited()
{
    const bool was = modified();
    m_formatEdited = true;
    if (!was)
        Q_EMIT modifiedChanged();
}

void EditorDocument::setEncoding(const QString &encoding)
{
    if (encoding == m_encoding || !TextFile::encodingAvailable(encoding))
        return;
    m_encoding = encoding;
    if (!byteOrderMarkPossible())
        m_bom = false;
    Q_EMIT formatChanged();
    markFormatEdited();
}

bool EditorDocument::byteOrderMarkPossible() const
{
    return m_encoding.startsWith(QLatin1String("UTF-"), Qt::CaseInsensitive);
}

void EditorDocument::setByteOrderMark(bool bom)
{
    if (bom == m_bom || (bom && !byteOrderMarkPossible()))
        return;
    m_bom = bom;
    Q_EMIT formatChanged();
    markFormatEdited();
}

void EditorDocument::setLineEnding(const QString &lineEnding)
{
    if (lineEnding != QLatin1String("LF") && lineEnding != QLatin1String("CRLF") && lineEnding != QLatin1String("CR"))
        return;
    if (lineEnding == m_lineEnding && !m_mixedLineEndings)
        return;
    m_lineEnding = lineEnding;
    m_mixedLineEndings = false;
    Q_EMIT formatChanged();
    markFormatEdited();
}

// ---- syntax -----------------------------------------------------------------

void EditorDocument::setSyntax(const QString &name)
{
    m_syntaxChosenByUser = true;
    if (name == m_syntax)
        return;
    m_syntax = name;
    applyDefinition();
}

void EditorDocument::applyDefinition(qint64 characters)
{
    QTextDocument *d = doc();
    if (!d || !m_highlighter)
        return;
    if (characters < 0)
        characters = d->characterCount();
    KSyntaxHighlighting::Definition definition;
    if (m_syntax != QLatin1String("None"))
        definition = syntaxRepository().definitionForName(m_syntax);
    const bool active = definition.isValid() && characters <= EditorLimits::maxHighlightChars;
    // The highlighter re-queues every block whose stored state differs from
    // the new one — one event (and one repaint) per line of the file. Blocks
    // without stored state are simply highlighted once, so drop it first.
    for (QTextBlock block = d->begin(); block.isValid(); block = block.next())
        block.setUserData(nullptr);
    m_highlighter->setDefinition(active ? definition : KSyntaxHighlighting::Definition());
    m_highlighting = active;
    if (!definition.isValid())
        m_syntax = QStringLiteral("None");
    Q_EMIT syntaxChanged();
}

void EditorDocument::setDark(bool dark)
{
    if (dark == m_dark)
        return;
    m_dark = dark;
    applyTheme();
    Q_EMIT darkChanged();
}

void EditorDocument::applyTheme()
{
    if (!m_highlighter)
        return;
    auto &repository = syntaxRepository();
    KSyntaxHighlighting::Theme theme = repository.theme(m_dark ? QStringLiteral("SHADOW Dark") : QStringLiteral("SHADOW Light"));
    if (!theme.isValid())
        theme = repository.defaultTheme(m_dark ? KSyntaxHighlighting::Repository::DarkTheme
                                               : KSyntaxHighlighting::Repository::LightTheme);
    m_highlighter->setTheme(theme);
    if (m_highlighting)
        m_highlighter->rehighlight();
}

// ---- files ------------------------------------------------------------------

void EditorDocument::setNotice(const QString &notice)
{
    if (notice == m_notice)
        return;
    m_notice = notice;
    Q_EMIT noticeChanged();
}

void EditorDocument::dismissNotice()
{
    setNotice(QString());
}

void EditorDocument::load(const QString &path, const QString &encoding)
{
    if (!doc())
        return;
    const QString absolute = QFileInfo(path).absoluteFilePath();
    const int serial = ++m_loadSerial;
    m_loading = true;
    Q_EMIT loadingChanged();

    auto *watcher = new QFutureWatcher<LoadResult>(this);
    connect(watcher, &QFutureWatcher<LoadResult>::finished, this, [this, watcher, absolute, serial] {
        watcher->deleteLater();
        if (serial != m_loadSerial)
            return;     // superseded by a newer load()
        const LoadResult result = watcher->result();
        m_loading = false;
        if (!result.decoded.ok()) {
            Q_EMIT loadingChanged();
            Q_EMIT loadFailed(result.decoded.error);
            return;
        }
        QTextDocument *d = doc();
        if (!d) {
            Q_EMIT loadingChanged();
            return;
        }
        const TextFile::Decoded &decoded = result.decoded;

        m_filePath = absolute;
        if (!m_syntaxChosenByUser) {
            auto &repository = syntaxRepository();
            KSyntaxHighlighting::Definition definition = repository.definitionForFileName(QFileInfo(absolute).fileName());
            if (!definition.isValid()) {
                const QMimeType mime = QMimeDatabase().mimeTypeForFileNameAndData(absolute, decoded.text.left(4096).toUtf8());
                definition = repository.definitionForMimeType(mime.name());
            }
            m_syntax = definition.isValid() ? definition.name() : QStringLiteral("None");
        }
        // The definition is in place before the text goes in, so every line
        // is highlighted exactly once — and a huge file never is.
        QElapsedTimer timer;
        timer.start();
        d->setUndoRedoEnabled(false);
        d->clear();
        applyDefinition(decoded.text.size());
        d->setPlainText(decoded.text);
        d->setUndoRedoEnabled(true);
        d->setModified(false);
        m_insertMs = int(timer.elapsed());

        m_encoding = decoded.encoding;
        m_bom = decoded.bom;
        m_lineEnding = decoded.lineEnding;
        m_mixedLineEndings = decoded.mixedLineEndings;
        m_formatEdited = false;
        m_diskModified = result.modified;
        m_diskSize = result.size;
        m_changedOnDisk = false;
        m_deletedOnDisk = false;
        watch();

        QStringList notices;
        if (decoded.guessed)
            notices << tr("This file is not UTF-8 and has no byte order mark; it was opened as %1. "
                          "If the text looks wrong, choose another encoding in the status bar.").arg(decoded.encoding);
        if (decoded.mixedLineEndings)
            notices << tr("This file mixes line endings (%1 LF, %2 CRLF, %3 CR). Saving will write %4 everywhere.")
                           .arg(decoded.lf).arg(decoded.crlf).arg(decoded.cr).arg(decoded.lineEnding);
        if (decoded.fragileCharacters)
            notices << tr("This file contains NUL or Unicode paragraph-separator characters that cannot be kept "
                          "exactly; saving will change them.");
        // A paragraph is laid out as a whole, so one enormous line makes
        // every keystroke slow, whatever the size of the file.
        qsizetype longest = 0, lineStart = 0;
        for (qsizetype i = 0; i <= decoded.text.size(); ++i) {
            if (i == decoded.text.size() || decoded.text.at(i) == u'\n') {
                longest = qMax(longest, i - lineStart);
                lineStart = i + 1;
            }
        }
        if (longest > EditorLimits::slowLineChars)
            notices << tr("This file has a line of %1 characters. Lines this long make editing slow, "
                          "because a line is always laid out as a whole.").arg(QLocale().toString(longest));
        if (definitionTooLarge())
            notices << tr("Syntax highlighting is off: the file has more than %1 characters.")
                           .arg(QLocale().toString(EditorLimits::maxHighlightChars));
        setNotice(notices.join(QLatin1Char('\n')));

        m_matchesDirty = !m_pattern.isEmpty();
        if (m_matchesDirty)
            refreshMatches();
        Q_EMIT loadingChanged();
        Q_EMIT filePathChanged();
        Q_EMIT formatChanged();
        Q_EMIT modifiedChanged();
        Q_EMIT diskStateChanged();
        Q_EMIT loaded();
    });
    watcher->setFuture(QtConcurrent::run(readFile, absolute, encoding));
}

bool EditorDocument::definitionTooLarge() const
{
    const QTextDocument *d = doc();
    return d && m_syntax != QLatin1String("None") && d->characterCount() > EditorLimits::maxHighlightChars;
}

void EditorDocument::reload()
{
    if (!m_filePath.isEmpty())
        load(m_filePath);
}

void EditorDocument::save()
{
    if (m_filePath.isEmpty()) {
        Q_EMIT saveFinished(false, tr("The document has no file name yet."));
        return;
    }
    writeFile(m_filePath);
}

void EditorDocument::saveAs(const QString &path)
{
    writeFile(QFileInfo(path).absoluteFilePath());
}

void EditorDocument::writeFile(const QString &path)
{
    QTextDocument *d = doc();
    if (!d || m_saving)
        return;
    const TextFile::Encoded encoded = TextFile::encode(plainText(), m_encoding, m_bom, m_lineEnding);
    if (!encoded.ok()) {
        Q_EMIT saveFinished(false, encoded.error);
        return;
    }
    const int revision = d->revision();
    m_saving = true;
    Q_EMIT savingChanged();
    // Our own write must not be reported as an external change.
    if (!m_watcher.files().isEmpty())
        m_watcher.removePaths(m_watcher.files());

    auto *watcher = new QFutureWatcher<SaveResult>(this);
    connect(watcher, &QFutureWatcher<SaveResult>::finished, this, [this, watcher, path, revision] {
        watcher->deleteLater();
        const SaveResult result = watcher->result();
        m_saving = false;
        Q_EMIT savingChanged();
        if (!result.error.isEmpty()) {
            watch();
            Q_EMIT saveFinished(false, result.error);
            return;
        }
        const bool renamed = path != m_filePath;
        m_filePath = path;
        m_diskModified = result.modified;
        m_diskSize = result.size;
        m_changedOnDisk = false;
        m_deletedOnDisk = false;
        m_formatEdited = false;
        m_mixedLineEndings = false;
        watch();
        QTextDocument *current = doc();
        if (current && current->revision() == revision)
            current->setModified(false);
        if (renamed) {
            Q_EMIT filePathChanged();
            if (!m_syntaxChosenByUser) {
                const auto definition = syntaxRepository().definitionForFileName(QFileInfo(path).fileName());
                if (definition.isValid() && definition.name() != m_syntax) {
                    m_syntax = definition.name();
                    applyDefinition();
                }
            }
        }
        Q_EMIT formatChanged();
        Q_EMIT modifiedChanged();
        Q_EMIT diskStateChanged();
        Q_EMIT saveFinished(true, QString());
    });
    watcher->setFuture(QtConcurrent::run(writeBytes, path, encoded.bytes));
}

void EditorDocument::watch()
{
    if (!m_watcher.files().isEmpty())
        m_watcher.removePaths(m_watcher.files());
    if (!m_filePath.isEmpty() && QFileInfo::exists(m_filePath))
        m_watcher.addPath(m_filePath);
}

void EditorDocument::checkDisk()
{
    if (m_filePath.isEmpty() || m_loading || m_saving)
        return;
    const QFileInfo info(m_filePath);
    bool changed = false, deleted = false;
    if (!info.exists())
        deleted = true;
    else if (info.lastModified() != m_diskModified || info.size() != m_diskSize)
        changed = true;
    // Editors and tools that replace the file (rename over it) make the
    // watcher lose it; pick the new one up.
    if (info.exists() && !m_watcher.files().contains(m_filePath))
        m_watcher.addPath(m_filePath);
    if (changed != m_changedOnDisk || deleted != m_deletedOnDisk) {
        m_changedOnDisk = changed;
        m_deletedOnDisk = deleted;
        Q_EMIT diskStateChanged();
    }
}

void EditorDocument::acknowledgeDiskChange()
{
    const QFileInfo info(m_filePath);
    if (info.exists()) {
        m_diskModified = info.lastModified();
        m_diskSize = info.size();
    } else {
        // The file is gone; what is in the editor is now the only copy.
        m_diskModified = QDateTime();
        m_diskSize = -1;
        markFormatEdited();
    }
    if (m_changedOnDisk || m_deletedOnDisk) {
        m_changedOnDisk = false;
        m_deletedOnDisk = false;
        Q_EMIT diskStateChanged();
    }
}

// ---- positions --------------------------------------------------------------

QVariantMap EditorDocument::lineColumn(int position) const
{
    const QTextDocument *d = doc();
    if (!d)
        return {{QStringLiteral("line"), 1}, {QStringLiteral("column"), 1}};
    const QTextBlock block = d->findBlock(position);
    return {
        {QStringLiteral("line"), block.blockNumber() + 1},
        {QStringLiteral("column"), position - block.position() + 1},
    };
}

int EditorDocument::positionForLine(int line, int column) const
{
    const QTextDocument *d = doc();
    if (!d)
        return 0;
    const QTextBlock block = d->findBlockByNumber(qBound(1, line, d->blockCount()) - 1);
    return block.position() + qBound(0, column - 1, block.length() - 1);
}

// ---- search -----------------------------------------------------------------

void EditorDocument::notifyMatches()
{
    if (m_editor)
        m_editor->refreshMatches();
    Q_EMIT matchesChanged();
}

const QString &EditorDocument::searchText()
{
    const QTextDocument *d = doc();
    const int revision = d ? d->revision() : -1;
    if (revision != m_searchTextRevision || revision < 0) {
        m_searchText = plainText();
        m_searchTextRevision = revision;
    }
    return m_searchText;
}

void EditorDocument::setSearch(const QString &pattern, bool regex, bool caseSensitive, bool wholeWord)
{
    m_pattern = pattern;
    m_regex = regex;
    m_caseSensitive = caseSensitive;
    m_wholeWord = wholeWord;
    refreshMatches();
}

void EditorDocument::refreshMatches()
{
    m_matchesDirty = false;
    m_matches.clear();
    m_searchError.clear();
    if (m_pattern.isEmpty() || !doc()) {
        m_searchText.clear();
        m_searchTextRevision = -1;
        notifyMatches();
        return;
    }
    QString pattern = m_regex ? m_pattern : QRegularExpression::escape(m_pattern);
    if (m_wholeWord)
        pattern = QStringLiteral("\\b(?:") + pattern + QStringLiteral(")\\b");
    QRegularExpression::PatternOptions options = QRegularExpression::MultilineOption
                                                 | QRegularExpression::UseUnicodePropertiesOption;
    if (!m_caseSensitive)
        options |= QRegularExpression::CaseInsensitiveOption;
    m_expression = QRegularExpression(pattern, options);
    if (!m_expression.isValid()) {
        m_searchError = tr("Invalid expression: %1").arg(m_expression.errorString());
        notifyMatches();
        return;
    }
    const QString &text = searchText();
    auto it = m_expression.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        if (match.capturedLength() == 0)
            continue;       // "^", "x*" and the like: nothing to select or replace
        m_matches.emplace_back(int(match.capturedStart()), int(match.capturedEnd()));
    }
    notifyMatches();
}

QVariantMap EditorDocument::findNext(int from, bool backward)
{
    if (m_matchesDirty)
        refreshMatches();
    QVariantMap result{{QStringLiteral("found"), false}};
    if (m_matches.empty())
        return result;
    qsizetype index;
    bool wrapped = false;
    if (!backward) {
        auto it = std::lower_bound(m_matches.begin(), m_matches.end(), from,
                                   [](const std::pair<int, int> &m, int pos) { return m.first < pos; });
        if (it == m_matches.end()) {
            it = m_matches.begin();
            wrapped = true;
        }
        index = it - m_matches.begin();
    } else {
        auto it = std::lower_bound(m_matches.begin(), m_matches.end(), from,
                                   [](const std::pair<int, int> &m, int pos) { return m.first < pos; });
        if (it == m_matches.begin()) {
            it = m_matches.end();
            wrapped = true;
        }
        index = (it - m_matches.begin()) - 1;
    }
    const auto &match = m_matches[size_t(index)];
    result[QStringLiteral("found")] = true;
    result[QStringLiteral("start")] = match.first;
    result[QStringLiteral("end")] = match.second;
    result[QStringLiteral("index")] = int(index) + 1;
    result[QStringLiteral("wrapped")] = wrapped;
    return result;
}

QString EditorDocument::expandReplacement(const QString &replacement, const QRegularExpressionMatch &match) const
{
    if (!m_regex)
        return replacement;
    QString out;
    for (int i = 0; i < replacement.size(); ++i) {
        const QChar c = replacement.at(i);
        if ((c == u'\\' || c == u'$') && i + 1 < replacement.size()) {
            const QChar next = replacement.at(i + 1);
            if (next.isDigit()) {
                out += match.captured(next.digitValue());
                ++i;
                continue;
            }
            if (c == u'\\') {
                if (next == u'n') { out += u'\n'; ++i; continue; }
                if (next == u't') { out += u'\t'; ++i; continue; }
                if (next == u'\\' || next == u'$') { out += next; ++i; continue; }
            }
        }
        out += c;
    }
    return out;
}

QVariantMap EditorDocument::replaceOne(int start, int end, const QString &replacement)
{
    if (m_matchesDirty)
        refreshMatches();
    QTextDocument *d = doc();
    if (!d)
        return {{QStringLiteral("found"), false}};
    const auto it = std::lower_bound(m_matches.begin(), m_matches.end(), start,
                                     [](const std::pair<int, int> &m, int pos) { return m.first < pos; });
    if (it == m_matches.end() || it->first != start || it->second != end)
        return findNext(start, false);
    const QRegularExpressionMatch match = m_expression.match(searchText(), start, QRegularExpression::NormalMatch,
                                                             QRegularExpression::AnchorAtOffsetMatchOption);
    const QString text = expandReplacement(replacement, match);
    QTextCursor cursor(d);
    cursor.setPosition(start);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    cursor.insertText(text);
    refreshMatches();
    QVariantMap next = findNext(start + int(text.size()), false);
    next[QStringLiteral("replaced")] = true;
    return next;
}

int EditorDocument::replaceAll(const QString &replacement)
{
    if (m_matchesDirty)
        refreshMatches();
    QTextDocument *d = doc();
    if (!d || m_matches.empty())
        return 0;
    const int count = int(m_matches.size());
    const QString text = searchText();
    QTextCursor cursor(d);
    cursor.beginEditBlock();
    if (count <= 400) {
        // Back to front so earlier positions stay valid.
        for (auto it = m_matches.rbegin(); it != m_matches.rend(); ++it) {
            const QRegularExpressionMatch match = m_expression.match(text, it->first, QRegularExpression::NormalMatch,
                                                                     QRegularExpression::AnchorAtOffsetMatchOption);
            cursor.setPosition(it->first);
            cursor.setPosition(it->second, QTextCursor::KeepAnchor);
            cursor.insertText(expandReplacement(replacement, match));
        }
    } else {
        // Many matches: build the result once and exchange the whole text —
        // still a single undo step.
        QString out;
        out.reserve(text.size());
        int last = 0;
        for (const auto &m : m_matches) {
            const QRegularExpressionMatch match = m_expression.match(text, m.first, QRegularExpression::NormalMatch,
                                                                     QRegularExpression::AnchorAtOffsetMatchOption);
            out += QStringView(text).mid(last, m.first - last);
            out += expandReplacement(replacement, match);
            last = m.second;
        }
        out += QStringView(text).mid(last);
        cursor.select(QTextCursor::Document);
        cursor.insertText(out);
    }
    cursor.endEditBlock();
    refreshMatches();
    return count;
}

// ---- indentation ------------------------------------------------------------

QVariantMap EditorDocument::indentLines(int start, int end, bool unindent, bool useSpaces, int width)
{
    QTextDocument *d = doc();
    if (!d)
        return {};
    width = qBound(1, width, 16);
    if (start > end)
        std::swap(start, end);
    QTextBlock first = d->findBlock(start);
    QTextBlock last = d->findBlock(end);
    // A selection ending at the very start of a line does not include it.
    if (last.blockNumber() > first.blockNumber() && end == last.position())
        last = last.previous();
    const int firstNumber = first.blockNumber();
    const int lastNumber = last.blockNumber();

    QTextCursor cursor(d);
    cursor.beginEditBlock();
    for (int number = firstNumber; number <= lastNumber; ++number) {
        const QTextBlock block = d->findBlockByNumber(number);
        const QString text = block.text();
        cursor.setPosition(block.position());
        if (!unindent) {
            if (!text.trimmed().isEmpty())
                cursor.insertText(useSpaces ? QString(width, QLatin1Char(' ')) : QStringLiteral("\t"));
        } else {
            int remove = 0;
            if (text.startsWith(QLatin1Char('\t'))) {
                remove = 1;
            } else {
                while (remove < width && remove < text.size() && text.at(remove) == u' ')
                    ++remove;
            }
            if (remove > 0) {
                cursor.setPosition(block.position() + remove, QTextCursor::KeepAnchor);
                cursor.removeSelectedText();
            }
        }
    }
    cursor.endEditBlock();
    const QTextBlock a = d->findBlockByNumber(firstNumber);
    const QTextBlock b = d->findBlockByNumber(lastNumber);
    return {
        {QStringLiteral("start"), a.position()},
        {QStringLiteral("end"), b.position() + b.length() - 1},
    };
}

QString EditorDocument::tabText(int position, bool useSpaces, int width) const
{
    const QTextDocument *d = doc();
    if (!useSpaces || !d)
        return QStringLiteral("\t");
    width = qBound(1, width, 16);
    const QTextBlock block = d->findBlock(position);
    const int column = visualColumn(block.text(), position - block.position(), width);
    return QString(width - column % width, QLatin1Char(' '));
}

QString EditorDocument::newlineText(int position, bool autoIndent, bool useSpaces, int width) const
{
    const QTextDocument *d = doc();
    if (!autoIndent || !d)
        return QStringLiteral("\n");
    width = qBound(1, width, 16);
    const QTextBlock block = d->findBlock(position);
    const QString before = block.text().left(position - block.position());
    QString indent = before.left(leadingWhitespace(before));
    const QString trimmed = before.trimmed();
    if (!trimmed.isEmpty()) {
        const QChar lastChar = trimmed.at(trimmed.size() - 1);
        const bool colonOpens = m_syntax == QLatin1String("Python") || m_syntax == QLatin1String("YAML");
        if (lastChar == u'{' || lastChar == u'[' || lastChar == u'(' || (colonOpens && lastChar == u':'))
            indent += useSpaces ? QString(width, QLatin1Char(' ')) : QStringLiteral("\t");
    }
    return QStringLiteral("\n") + indent;
}

int EditorDocument::backspaceIndent(int position, bool useSpaces, int width) const
{
    const QTextDocument *d = doc();
    if (!useSpaces || !d)
        return 0;
    width = qBound(1, width, 16);
    const QTextBlock block = d->findBlock(position);
    const QString before = block.text().left(position - block.position());
    if (before.isEmpty())
        return 0;
    for (const QChar c : before) {
        if (c != u' ')
            return 0;
    }
    const int remove = int((before.size() - 1) % width) + 1;
    return remove > 1 ? remove : 0;
}
