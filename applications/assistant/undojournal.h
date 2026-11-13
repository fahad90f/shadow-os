// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonArray>
#include <QObject>
#include <QString>

// A record of every file-changing action the assistant takes, with enough
// saved state to reverse it. The assistant runs tools without asking first, so
// this is the safety net: a visible history, and an undo for file operations.
//
// Nothing here ever blocks or prompts. Destructive operations (overwrite,
// delete, move over an existing file) are backed up to an undo store BEFORE
// they run, so undo can restore them; creations are undone by removal. The
// store and the log live under ~/.local/share/shadow/assistant/.
class UndoJournal : public QObject
{
    Q_OBJECT
public:
    explicit UndoJournal(QObject *parent = nullptr);

    // Prepared, per-operation. Each prepare*() captures whatever is needed to
    // reverse the op and returns a token; call commit() after the op succeeds,
    // or discard() if it failed. A token is empty when journalling is off.
    struct Token { QString id; QString backup; bool existed = false; };

    Token prepareWrite(const QString &path);              // overwrite or create a file
    Token prepareDelete(const QString &path);             // delete a file or tree
    Token prepareMove(const QString &src, const QString &dst);
    Token prepareCopy(const QString &dst);                // the new copy at dst
    Token prepareMkdir(const QString &path);

    // Record the finished action (op is "write_file", "delete_path", …).
    void commit(const Token &token, const QString &op,
                const QString &path, const QString &summary);
    void discard(const Token &token);                     // op failed; drop the backup

    // The log, newest first: [{id, time, op, path, summary, undone}].
    QJsonArray list(int limit = 100) const;

    // Reverse the action with this id. Returns an empty string on success,
    // otherwise a human-readable reason it could not be undone.
    QString undo(const QString &id);

Q_SIGNALS:
    void changed();

private:
    QString m_dir;      // ~/.local/share/shadow/assistant
    QString m_store;    // <dir>/undo
    QString m_log;      // <dir>/actions.jsonl

    QString newId() const;
    QJsonArray readAll() const;
    void append(const QJsonObject &record);
    void rewrite(const QJsonArray &records);
    static bool copyTree(const QString &from, const QString &to);
    static bool removeTree(const QString &path);
};
