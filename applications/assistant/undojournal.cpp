// SPDX-License-Identifier: GPL-3.0-or-later
#include "undojournal.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QStandardPaths>

UndoJournal::UndoJournal(QObject *parent) : QObject(parent)
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    // AppDataLocation already includes the app name; keep a stable folder.
    m_dir = QDir(base).absoluteFilePath(QStringLiteral("shadow-assistant"));
    if (!QFileInfo::exists(m_dir))
        m_dir = QDir(QDir::homePath()).absoluteFilePath(
            QStringLiteral(".local/share/shadow/assistant"));
    m_store = QDir(m_dir).absoluteFilePath(QStringLiteral("undo"));
    m_log   = QDir(m_dir).absoluteFilePath(QStringLiteral("actions.jsonl"));
    QDir().mkpath(m_store);
}

QString UndoJournal::newId() const
{
    return QString::number(QDateTime::currentMSecsSinceEpoch(), 36)
         + QString::number(QRandomGenerator::global()->bounded(100000), 36);
}

bool UndoJournal::copyTree(const QString &from, const QString &to)
{
    QFileInfo fi(from);
    if (fi.isDir()) {
        QDir().mkpath(to);
        const QDir src(from);
        const auto entries = src.entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden | QDir::System);
        for (const QFileInfo &e : entries)
            if (!copyTree(e.absoluteFilePath(), QDir(to).absoluteFilePath(e.fileName())))
                return false;
        return true;
    }
    QDir().mkpath(QFileInfo(to).absolutePath());
    QFile::remove(to);
    return QFile::copy(from, to);
}

bool UndoJournal::removeTree(const QString &path)
{
    QFileInfo fi(path);
    if (fi.isDir() && !fi.isSymLink())
        return QDir(path).removeRecursively();
    return QFile::remove(path);
}

UndoJournal::Token UndoJournal::prepareWrite(const QString &path)
{
    Token t;
    t.id = newId();
    if (QFileInfo::exists(path)) {
        t.existed = true;
        t.backup = QDir(m_store).absoluteFilePath(t.id);
        if (!copyTree(path, t.backup))
            t.backup.clear();  // could not back up; undo will not restore content
    }
    return t;
}

UndoJournal::Token UndoJournal::prepareDelete(const QString &path)
{
    Token t;
    t.id = newId();
    t.existed = QFileInfo::exists(path);
    if (t.existed) {
        t.backup = QDir(m_store).absoluteFilePath(t.id);
        if (!copyTree(path, t.backup))
            t.backup.clear();  // could not back up; the delete is not undoable
    }
    return t;
}

UndoJournal::Token UndoJournal::prepareMove(const QString & /*src*/, const QString &dst)
{
    // Only the destination needs saving: if the move overwrites something, that
    // is the data that would otherwise be lost. The source reappears on undo.
    return prepareWrite(dst);
}

UndoJournal::Token UndoJournal::prepareCopy(const QString &dst)
{
    // existed == true means dst was overwritten (restore it); false means the
    // copy is new (remove it on undo).
    return prepareWrite(dst);
}

UndoJournal::Token UndoJournal::prepareMkdir(const QString &path)
{
    Token t;
    t.id = newId();
    t.existed = QFileInfo::exists(path);  // if it already existed, undo is a no-op
    return t;
}

void UndoJournal::commit(const Token &token, const QString &op,
                         const QString &path, const QString &summary)
{
    if (token.id.isEmpty())
        return;
    QJsonObject rec{
        {QStringLiteral("id"), token.id},
        {QStringLiteral("time"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {QStringLiteral("op"), op},
        {QStringLiteral("path"), path},
        {QStringLiteral("summary"), summary},
        {QStringLiteral("existed"), token.existed},
        {QStringLiteral("backup"), token.backup},
        {QStringLiteral("undone"), false},
    };
    append(rec);
    Q_EMIT changed();
}

void UndoJournal::discard(const Token &token)
{
    if (!token.backup.isEmpty())
        removeTree(token.backup);
}

QJsonArray UndoJournal::readAll() const
{
    QJsonArray out;
    QFile f(m_log);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return out;
    while (!f.atEnd()) {
        const QByteArray line = f.readLine().trimmed();
        if (line.isEmpty())
            continue;
        const QJsonDocument d = QJsonDocument::fromJson(line);
        if (d.isObject())
            out.append(d.object());
    }
    return out;
}

void UndoJournal::append(const QJsonObject &record)
{
    QDir().mkpath(m_dir);
    QFile f(m_log);
    if (!f.open(QIODevice::Append | QIODevice::Text))
        return;
    f.write(QJsonDocument(record).toJson(QJsonDocument::Compact));
    f.write("\n");
}

void UndoJournal::rewrite(const QJsonArray &records)
{
    QFile f(m_log);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return;
    for (const QJsonValue &v : records) {
        f.write(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
        f.write("\n");
    }
}

QJsonArray UndoJournal::list(int limit) const
{
    const QJsonArray all = readAll();
    QJsonArray out;
    for (int i = all.size() - 1; i >= 0 && out.size() < limit; --i)
        out.append(all.at(i));
    return out;
}

QString UndoJournal::undo(const QString &id)
{
    QJsonArray all = readAll();
    int idx = -1;
    for (int i = 0; i < all.size(); ++i)
        if (all.at(i).toObject().value(QStringLiteral("id")).toString() == id) { idx = i; break; }
    if (idx < 0)
        return tr("That action is no longer in the history.");

    QJsonObject rec = all.at(idx).toObject();
    if (rec.value(QStringLiteral("undone")).toBool())
        return tr("That action was already undone.");

    const QString op      = rec.value(QStringLiteral("op")).toString();
    const QString path    = rec.value(QStringLiteral("path")).toString();
    const QString backup  = rec.value(QStringLiteral("backup")).toString();
    const bool    existed = rec.value(QStringLiteral("existed")).toBool();

    auto restoreFromBackup = [&](const QString &target) -> QString {
        if (backup.isEmpty() || !QFileInfo::exists(backup))
            return tr("The saved copy needed to undo this is no longer available.");
        removeTree(target);
        return copyTree(backup, target) ? QString() : tr("Could not restore %1.").arg(target);
    };

    QString err;
    if (op == QLatin1String("write_file") || op == QLatin1String("write_code")) {
        err = existed ? restoreFromBackup(path)
                      : (removeTree(path) ? QString() : tr("Could not remove %1.").arg(path));
    } else if (op == QLatin1String("delete_path")) {
        err = restoreFromBackup(path);
    } else if (op == QLatin1String("make_directory")) {
        if (!existed) {
            QFileInfo fi(path);
            if (fi.isDir() && QDir(path).isEmpty())
                err = QDir().rmpath(path) ? QString() : tr("Could not remove the directory %1.").arg(path);
            else if (fi.isDir())
                err = tr("The directory %1 is no longer empty — not removing it.").arg(path);
        }
    } else if (op == QLatin1String("copy_path")) {
        err = existed ? restoreFromBackup(path)
                      : (removeTree(path) ? QString() : tr("Could not remove the copy at %1.").arg(path));
    } else if (op == QLatin1String("move_path")) {
        // The move's destination is recorded as 'path'; restore whatever the
        // move overwrote there (if anything). The moved-in file is left as is,
        // because we do not have its original location recorded reliably.
        err = existed ? restoreFromBackup(path) : QString();
    } else {
        return tr("This kind of action cannot be undone.");
    }

    if (!err.isEmpty())
        return err;

    rec.insert(QStringLiteral("undone"), true);
    all.replace(idx, rec);
    rewrite(all);
    if (!backup.isEmpty())
        removeTree(backup);
    Q_EMIT changed();
    return QString();
}
