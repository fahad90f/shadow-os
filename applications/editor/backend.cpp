// SPDX-License-Identifier: GPL-3.0-or-later
#include "backend.h"
#include "document.h"
#include "textfile.h"

#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Repository>

#include <QCollator>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QSettings>

namespace {
const int kMaxRecent = 15;
}

EditorBackend::EditorBackend(QObject *parent)
    : QObject(parent)
{
    m_recent = QSettings().value(QStringLiteral("Recent/files")).toStringList();
}

QStringList EditorBackend::encodings() const
{
    return TextFile::encodings();
}

qreal EditorBackend::maxFileBytes() const
{
    return qreal(EditorLimits::maxFileBytes);
}

QStringList EditorBackend::monospaceFamilies() const
{
    QStringList out;
    const QStringList families = QFontDatabase::families();
    for (const QString &family : families) {
        if (QFontDatabase::isFixedPitch(family) && !QFontDatabase::isPrivateFamily(family))
            out << family;
    }
    return out;
}

QVariantList EditorBackend::syntaxes() const
{
    QVariantList out;
    out.append(QVariantMap{{QStringLiteral("name"), QStringLiteral("None")}, {QStringLiteral("section"), QString()}});
    QList<KSyntaxHighlighting::Definition> definitions = syntaxRepository().definitions();
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(definitions.begin(), definitions.end(), [&collator](const auto &a, const auto &b) {
        return collator.compare(a.name(), b.name()) < 0;
    });
    for (const auto &definition : std::as_const(definitions)) {
        if (definition.isHidden() || definition.name() == QLatin1String("None"))
            continue;
        out.append(QVariantMap{{QStringLiteral("name"), definition.name()},
                               {QStringLiteral("section"), definition.section()}});
    }
    return out;
}

void EditorBackend::addRecentFile(const QString &path)
{
    if (path.isEmpty())
        return;
    m_recent.removeAll(path);
    m_recent.prepend(path);
    while (m_recent.size() > kMaxRecent)
        m_recent.removeLast();
    QSettings().setValue(QStringLiteral("Recent/files"), m_recent);
    Q_EMIT recentFilesChanged();
}

void EditorBackend::clearRecentFiles()
{
    m_recent.clear();
    QSettings().setValue(QStringLiteral("Recent/files"), m_recent);
    Q_EMIT recentFilesChanged();
}

void EditorBackend::saveSession(const QVariantList &files, int current)
{
    QSettings settings;
    settings.beginWriteArray(QStringLiteral("Session/files"), int(files.size()));
    for (int i = 0; i < files.size(); ++i) {
        const QVariantMap file = files.at(i).toMap();
        settings.setArrayIndex(i);
        settings.setValue(QStringLiteral("path"), file.value(QStringLiteral("path")));
        settings.setValue(QStringLiteral("position"), file.value(QStringLiteral("position")));
    }
    settings.endArray();
    settings.setValue(QStringLiteral("Session/current"), current);
    settings.sync();
}

QVariantMap EditorBackend::session() const
{
    QSettings settings;
    QVariantList files;
    const int count = settings.beginReadArray(QStringLiteral("Session/files"));
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        const QString path = settings.value(QStringLiteral("path")).toString();
        if (path.isEmpty() || !QFileInfo(path).isFile())
            continue;       // deleted since the last run
        files.append(QVariantMap{{QStringLiteral("path"), path},
                                 {QStringLiteral("position"), settings.value(QStringLiteral("position")).toInt()}});
    }
    settings.endArray();
    return {{QStringLiteral("files"), files},
            {QStringLiteral("current"), settings.value(QStringLiteral("Session/current")).toInt()}};
}

QString EditorBackend::homePath() const { return QDir::homePath(); }

QString EditorBackend::parentPath(const QString &path) const
{
    QDir dir(path);
    return dir.cdUp() ? dir.absolutePath() : QStringLiteral("/");
}

QString EditorBackend::joinPath(const QString &folder, const QString &name) const
{
    return QDir::cleanPath(QDir(folder).filePath(name));
}

QString EditorBackend::fileName(const QString &path) const { return QFileInfo(path).fileName(); }
bool EditorBackend::isDirectory(const QString &path) const { return !path.isEmpty() && QFileInfo(path).isDir(); }
bool EditorBackend::exists(const QString &path) const { return !path.isEmpty() && QFileInfo::exists(path); }
QString EditorBackend::localPath(const QUrl &url) const { return url.isLocalFile() ? url.toLocalFile() : QString(); }
QString EditorBackend::absolutePath(const QString &path) const { return QFileInfo(path).absoluteFilePath(); }

QVariantList EditorBackend::listDirectory(const QString &path, bool showHidden) const
{
    QVariantList out;
    QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot;
    if (showHidden)
        filters |= QDir::Hidden;
    const QFileInfoList entries = QDir(path).entryInfoList(filters, QDir::DirsFirst | QDir::Name | QDir::IgnoreCase | QDir::LocaleAware);
    for (const QFileInfo &info : entries) {
        out.append(QVariantMap{
            {QStringLiteral("name"), info.fileName()},
            {QStringLiteral("path"), info.absoluteFilePath()},
            {QStringLiteral("directory"), info.isDir()},
            {QStringLiteral("size"), info.isDir() ? qint64(0) : info.size()},
        });
    }
    return out;
}
