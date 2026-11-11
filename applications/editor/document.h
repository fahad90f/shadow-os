// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDateTime>
#include <QFileSystemWatcher>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QQuickTextDocument>
#include <QRegularExpression>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <utility>
#include <vector>

namespace KSyntaxHighlighting {
class Repository;
class SyntaxHighlighter;
}
class QTextDocument;

// Limits established by measurement (see README, "Large files").
namespace EditorLimits {
// Files larger than this are refused with an explanation.
constexpr qint64 maxFileBytes = 64LL * 1024 * 1024;
// Above this many characters syntax highlighting is switched off.
constexpr qint64 maxHighlightChars = 4LL * 1024 * 1024;
}

KSyntaxHighlighting::Repository &syntaxRepository();

// One open text file: the model behind an EditorView. Owns everything that
// is not pixels — file I/O, encoding, line endings, change detection on
// disk, syntax highlighting, search and the indentation edits.
class EditorDocument : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickTextDocument *textDocument READ textDocument WRITE setTextDocument NOTIFY textDocumentChanged)
    Q_PROPERTY(QString filePath READ filePath NOTIFY filePathChanged)
    Q_PROPERTY(QString fileName READ fileName NOTIFY filePathChanged)
    Q_PROPERTY(bool modified READ modified NOTIFY modifiedChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(bool saving READ saving NOTIFY savingChanged)
    Q_PROPERTY(QString encoding READ encoding WRITE setEncoding NOTIFY formatChanged)
    Q_PROPERTY(bool byteOrderMark READ byteOrderMark WRITE setByteOrderMark NOTIFY formatChanged)
    Q_PROPERTY(bool byteOrderMarkPossible READ byteOrderMarkPossible NOTIFY formatChanged)
    Q_PROPERTY(QString lineEnding READ lineEnding WRITE setLineEnding NOTIFY formatChanged)
    Q_PROPERTY(bool mixedLineEndings READ mixedLineEndings NOTIFY formatChanged)
    Q_PROPERTY(QString syntax READ syntax WRITE setSyntax NOTIFY syntaxChanged)
    Q_PROPERTY(bool highlighting READ highlighting NOTIFY syntaxChanged)
    Q_PROPERTY(bool dark READ dark WRITE setDark NOTIFY darkChanged)
    Q_PROPERTY(bool changedOnDisk READ changedOnDisk NOTIFY diskStateChanged)
    Q_PROPERTY(bool deletedOnDisk READ deletedOnDisk NOTIFY diskStateChanged)
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)
    Q_PROPERTY(int lineCount READ lineCount NOTIFY lineCountChanged)
    Q_PROPERTY(int matchCount READ matchCount NOTIFY matchesChanged)
    Q_PROPERTY(QString searchError READ searchError NOTIFY matchesChanged)
    Q_PROPERTY(int layoutRevision READ layoutRevision NOTIFY layoutRevisionChanged)
    Q_PROPERTY(int insertMilliseconds READ insertMilliseconds NOTIFY loaded)

public:
    explicit EditorDocument(QObject *parent = nullptr);
    ~EditorDocument() override;

    QQuickTextDocument *textDocument() const { return m_quickDocument; }
    void setTextDocument(QQuickTextDocument *document);

    QString filePath() const { return m_filePath; }
    QString fileName() const;
    bool modified() const;
    bool loading() const { return m_loading; }
    bool saving() const { return m_saving; }
    QString encoding() const { return m_encoding; }
    void setEncoding(const QString &encoding);
    bool byteOrderMark() const { return m_bom; }
    void setByteOrderMark(bool bom);
    bool byteOrderMarkPossible() const;
    QString lineEnding() const { return m_lineEnding; }
    void setLineEnding(const QString &lineEnding);
    bool mixedLineEndings() const { return m_mixedLineEndings; }
    QString syntax() const { return m_syntax; }
    void setSyntax(const QString &name);
    bool highlighting() const { return m_highlighting; }
    bool dark() const { return m_dark; }
    void setDark(bool dark);
    bool changedOnDisk() const { return m_changedOnDisk; }
    bool deletedOnDisk() const { return m_deletedOnDisk; }
    QString notice() const { return m_notice; }
    int lineCount() const;
    int matchCount() const { return int(m_matches.size()); }
    QString searchError() const { return m_searchError; }
    int layoutRevision() const { return m_layoutRevision; }
    // How long putting the text of the last load into the document took (diagnostics).
    int insertMilliseconds() const { return m_insertMs; }

    // ---- files -------------------------------------------------------------
    // Asynchronous; ends with loaded() or loadFailed(message). An empty
    // encoding means "detect".
    Q_INVOKABLE void load(const QString &path, const QString &encoding = QString());
    Q_INVOKABLE void reload();
    // Asynchronous; ends with saveFinished(ok, message). The write is atomic
    // (QSaveFile: temporary file + rename) wherever the directory allows it.
    Q_INVOKABLE void save();
    Q_INVOKABLE void saveAs(const QString &path);
    // Compare the file on disk with what was last loaded or saved.
    Q_INVOKABLE void checkDisk();
    // "Keep my version": stop warning until the file changes again.
    Q_INVOKABLE void acknowledgeDiskChange();
    Q_INVOKABLE void dismissNotice();

    // ---- geometry for the line-number gutter -------------------------------
    // Blocks intersecting [top, bottom] in text coordinates:
    // [{ line, y, height, lineHeight }]
    Q_INVOKABLE QVariantList visibleLines(qreal top, qreal bottom) const;
    Q_INVOKABLE QVariantMap lineColumn(int position) const;
    Q_INVOKABLE int positionForLine(int line, int column) const;

    // ---- search ------------------------------------------------------------
    Q_INVOKABLE void setSearch(const QString &pattern, bool regex, bool caseSensitive, bool wholeWord);
    // { found, start, end, index (1-based), wrapped }
    Q_INVOKABLE QVariantMap findNext(int from, bool backward);
    // Replace the match that is exactly [start, end) — nothing happens when
    // the selection is not a match — and return the following match.
    Q_INVOKABLE QVariantMap replaceOne(int start, int end, const QString &replacement);
    Q_INVOKABLE int replaceAll(const QString &replacement);

    // ---- indentation -------------------------------------------------------
    // Indent or unindent every line touched by [start, end]; returns the new
    // selection { start, end }.
    Q_INVOKABLE QVariantMap indentLines(int start, int end, bool unindent, bool useSpaces, int width);
    // Text a Tab key press inserts at 'position'.
    Q_INVOKABLE QString tabText(int position, bool useSpaces, int width) const;
    // Text a Return key press inserts at 'position' (newline + indentation).
    Q_INVOKABLE QString newlineText(int position, bool autoIndent, bool useSpaces, int width) const;
    // How many characters Backspace should remove at 'position' to step back
    // one indentation level (0: ordinary backspace).
    Q_INVOKABLE int backspaceIndent(int position, bool useSpaces, int width) const;

    QString plainText() const;

Q_SIGNALS:
    void textDocumentChanged();
    void filePathChanged();
    void modifiedChanged();
    void loadingChanged();
    void savingChanged();
    void formatChanged();
    void syntaxChanged();
    void darkChanged();
    void diskStateChanged();
    void noticeChanged();
    void lineCountChanged();
    void matchesChanged();
    void layoutRevisionChanged();
    void loaded();
    void loadFailed(const QString &message);
    void saveFinished(bool ok, const QString &message);

private:
    QTextDocument *doc() const;
    void applyDefinition(qint64 characters = -1);
    void applyTheme();
    void watch();
    void rememberDiskState();
    void setNotice(const QString &notice);
    void markFormatEdited();
    const QString &searchText();
    void refreshMatches();
    QString expandReplacement(const QString &replacement, const QRegularExpressionMatch &match) const;
    void writeFile(const QString &path);
    bool definitionTooLarge() const;

    QPointer<QQuickTextDocument> m_quickDocument;
    KSyntaxHighlighting::SyntaxHighlighter *m_highlighter = nullptr;
    QString m_filePath;
    QString m_encoding = QStringLiteral("UTF-8");
    bool m_bom = false;
    QString m_lineEnding = QStringLiteral("LF");
    bool m_mixedLineEndings = false;
    QString m_syntax = QStringLiteral("None");
    bool m_syntaxChosenByUser = false;
    bool m_highlighting = false;
    bool m_dark = true;
    bool m_loading = false;
    bool m_saving = false;
    bool m_formatEdited = false;    // encoding/line ending changed since the last save
    bool m_changedOnDisk = false;
    bool m_deletedOnDisk = false;
    QString m_notice;
    int m_loadSerial = 0;
    int m_layoutRevision = 0;
    int m_insertMs = 0;

    QFileSystemWatcher m_watcher;
    QTimer m_diskTimer;
    QDateTime m_diskModified;
    qint64 m_diskSize = -1;

    // search state
    QString m_pattern;
    bool m_regex = false;
    bool m_caseSensitive = false;
    bool m_wholeWord = false;
    QRegularExpression m_expression;
    QString m_searchError;
    std::vector<std::pair<int, int>> m_matches;
    bool m_matchesDirty = false;
    QTimer m_matchTimer;
    QString m_searchText;
    int m_searchTextRevision = -1;
};
