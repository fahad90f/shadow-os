// SPDX-License-Identifier: GPL-3.0-or-later
#include "manifest.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

namespace Arsenal {

namespace {

// The order profiles are presented in; unknown ids follow alphabetically.
const QStringList kOrder = {QStringLiteral("core"), QStringLiteral("web"), QStringLiteral("network"),
                            QStringLiteral("redteam"), QStringLiteral("blueteam"), QStringLiteral("forensics"),
                            QStringLiteral("reversing"), QStringLiteral("developer"), QStringLiteral("complete")};
const QSet<QString> kNotProfiles = {QStringLiteral("schema.json"), QStringLiteral("categories.json"),
                                    QStringLiteral("sizes.json")};

QString tr(const char *text)
{
    return QCoreApplication::translate("Arsenal::Manifests", text);
}

bool readObject(const QString &path, QJsonObject *object, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = file.errorString();
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        *error = tr("not valid JSON (%1 at offset %2)").arg(parseError.errorString()).arg(parseError.offset);
        return false;
    }
    if (!document.isObject()) {
        *error = tr("not a JSON object");
        return false;
    }
    *object = document.object();
    return true;
}

QStringList stringList(const QJsonValue &value)
{
    QStringList list;
    const QJsonArray array = value.toArray();
    for (const QJsonValue &item : array)
        list << item.toString();
    return list;
}

} // namespace

bool Manifests::load(const QString &dirPath)
{
    m_dir = dirPath;
    m_error.clear();
    m_warnings.clear();
    m_categories.clear();
    m_profiles.clear();

    const QDir dir(dirPath);
    if (!dir.exists()) {
        m_error = tr("The profile directory %1 does not exist. It is installed by the package nexus-arsenal-data.")
                      .arg(dirPath);
        return false;
    }

    QJsonObject object;
    QString error;
    if (readObject(dir.filePath(QStringLiteral("categories.json")), &object, &error)) {
        const QJsonArray array = object.value(QLatin1String("categories")).toArray();
        for (const QJsonValue &value : array) {
            const QJsonObject c = value.toObject();
            m_categories.append({c.value(QLatin1String("id")).toString(), c.value(QLatin1String("name")).toString(),
                                 c.value(QLatin1String("icon")).toString(),
                                 c.value(QLatin1String("description")).toString()});
        }
    } else {
        m_warnings << tr("categories.json: %1").arg(error);
    }

    QStringList names = dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    std::stable_sort(names.begin(), names.end(), [](const QString &a, const QString &b) {
        const auto rank = [](const QString &name) {
            const int index = kOrder.indexOf(name.chopped(5));
            return index < 0 ? int(kOrder.size()) : index;
        };
        return rank(a) < rank(b);
    });

    for (const QString &name : std::as_const(names)) {
        if (kNotProfiles.contains(name))
            continue;
        if (!readObject(dir.filePath(name), &object, &error)) {
            m_warnings << QStringLiteral("%1: %2").arg(name, error);
            continue;
        }
        if (object.value(QLatin1String("schema")).toInt() != 1) {
            m_warnings << tr("%1: unsupported manifest format (schema %2)")
                              .arg(name).arg(object.value(QLatin1String("schema")).toInt());
            continue;
        }
        Profile profile;
        profile.id = object.value(QLatin1String("id")).toString();
        profile.name = object.value(QLatin1String("name")).toString();
        profile.summary = object.value(QLatin1String("summary")).toString();
        profile.description = object.value(QLatin1String("description")).toString();
        profile.icon = object.value(QLatin1String("icon")).toString();
        profile.metapackage = object.value(QLatin1String("metapackage")).toString();
        profile.includes = stringList(object.value(QLatin1String("includes")));
        if (profile.id.isEmpty() || profile.name.isEmpty() || profile.metapackage.isEmpty()) {
            m_warnings << tr("%1: id, name or metapackage is missing").arg(name);
            continue;
        }
        if (this->profile(profile.id)) {
            m_warnings << tr("%1: profile id '%2' is already defined").arg(name, profile.id);
            continue;
        }
        const QJsonArray tools = object.value(QLatin1String("tools")).toArray();
        for (const QJsonValue &value : tools) {
            const QJsonObject t = value.toObject();
            Tool tool;
            tool.package = t.value(QLatin1String("package")).toString();
            tool.name = t.value(QLatin1String("name")).toString();
            tool.summary = t.value(QLatin1String("summary")).toString();
            tool.category = t.value(QLatin1String("category")).toString();
            tool.interface = t.value(QLatin1String("interface")).toString();
            tool.homepage = t.value(QLatin1String("homepage")).toString();
            tool.command = t.value(QLatin1String("command")).toString();
            tool.hasHelp = t.contains(QLatin1String("help"));
            tool.help = stringList(t.value(QLatin1String("help")));
            tool.desktopId = t.value(QLatin1String("desktop-id")).toString();
            tool.notes = t.value(QLatin1String("notes")).toString();
            tool.architectures = stringList(t.value(QLatin1String("architectures")));
            if (tool.package.isEmpty() || tool.name.isEmpty()) {
                m_warnings << tr("%1: a tool without package or name was skipped").arg(name);
                continue;
            }
            profile.tools.append(tool);
        }
        const QJsonArray external = object.value(QLatin1String("external")).toArray();
        for (const QJsonValue &value : external) {
            const QJsonObject e = value.toObject();
            const QJsonObject why = e.value(QLatin1String("why")).toObject();
            const QJsonObject how = e.value(QLatin1String("how")).toObject();
            External entry;
            entry.name = e.value(QLatin1String("name")).toString();
            entry.summary = e.value(QLatin1String("summary")).toString();
            entry.category = e.value(QLatin1String("category")).toString();
            entry.homepage = e.value(QLatin1String("homepage")).toString();
            entry.license = e.value(QLatin1String("license")).toString();
            entry.reason = why.value(QLatin1String("reason")).toString();
            entry.why = why.value(QLatin1String("text")).toString();
            entry.how = how.value(QLatin1String("text")).toString();
            entry.howUrl = how.value(QLatin1String("url")).toString();
            if (!entry.name.isEmpty())
                profile.external.append(entry);
        }
        m_profiles.append(profile);
    }

    if (m_profiles.isEmpty()) {
        m_error = tr("No profile manifests were found in %1.").arg(dirPath);
        return false;
    }
    return true;
}

const Profile *Manifests::profile(const QString &id) const
{
    for (const Profile &profile : m_profiles) {
        if (profile.id == id)
            return &profile;
    }
    return nullptr;
}

Category Manifests::category(const QString &id) const
{
    for (const Category &category : m_categories) {
        if (category.id == id)
            return category;
    }
    // A category the installed categories.json does not know: show its id.
    return {id, id, QStringLiteral("wrench"), QString()};
}

QStringList Manifests::includedProfiles(const QString &id) const
{
    QStringList seen;
    QStringList queue{id};
    while (!queue.isEmpty()) {
        const Profile *current = profile(queue.takeFirst());
        if (!current)
            continue;
        for (const QString &other : current->includes) {
            if (other != id && !seen.contains(other) && profile(other)) {
                seen << other;
                queue << other;
            }
        }
    }
    return seen;
}

QList<Tool> Manifests::effectiveTools(const QString &profileId) const
{
    QList<Tool> tools;
    QSet<QString> names;
    for (const QString &id : QStringList{profileId} + includedProfiles(profileId)) {
        const Profile *source = profile(id);
        if (!source)
            continue;
        for (const Tool &tool : source->tools) {
            if (!names.contains(tool.package)) {
                names.insert(tool.package);
                tools.append(tool);
            }
        }
    }
    return tools;
}

QList<External> Manifests::effectiveExternal(const QString &profileId) const
{
    QList<External> entries;
    QSet<QString> names;
    for (const QString &id : QStringList{profileId} + includedProfiles(profileId)) {
        const Profile *source = profile(id);
        if (!source)
            continue;
        for (const External &entry : source->external) {
            if (!names.contains(entry.name)) {
                names.insert(entry.name);
                entries.append(entry);
            }
        }
    }
    return entries;
}

QStringList Manifests::profilesOf(const QString &package) const
{
    QStringList ids;
    for (const Profile &profile : m_profiles) {
        for (const Tool &tool : profile.tools) {
            if (tool.package == package) {
                ids << profile.id;
                break;
            }
        }
    }
    return ids;
}

QStringList Manifests::allPackages() const
{
    QStringList packages;
    QSet<QString> seen;
    for (const Profile &profile : m_profiles) {
        for (const Tool &tool : profile.tools) {
            if (!seen.contains(tool.package)) {
                seen.insert(tool.package);
                packages << tool.package;
            }
        }
    }
    return packages;
}

QStringList Manifests::allMetapackages() const
{
    QStringList packages;
    for (const Profile &profile : m_profiles)
        packages << profile.metapackage;
    return packages;
}

const Tool *Manifests::tool(const QString &package) const
{
    for (const Profile &profile : m_profiles) {
        for (const Tool &tool : profile.tools) {
            if (tool.package == package)
                return &tool;
        }
    }
    return nullptr;
}

} // namespace Arsenal
