// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

// The SHADOW Arsenal profile manifests, as written in arsenal/profiles/*.json
// (format: schema.json in that directory) and installed by shadow-arsenal-data
// to /usr/share/shadow/arsenal/profiles.
namespace Arsenal {

struct Category
{
    QString id;
    QString name;
    QString icon;           // ShadowUI glyph
    QString description;
};

struct Tool
{
    QString package;        // Debian binary package
    QString name;
    QString summary;
    QString category;       // category id (may differ per profile)
    QString interface;      // "cli" | "gui" | "both"
    QString homepage;
    QString command;        // executable, for cli/both
    QStringList help;       // arguments that print the usage (default: --help)
    QString desktopId;      // desktop entry without .desktop, for gui/both
    QString notes;
    QStringList architectures; // empty: every architecture

    bool availableOn(const QString &arch) const { return architectures.isEmpty() || architectures.contains(arch); }
    QStringList helpArguments() const { return hasHelp ? help : QStringList{QStringLiteral("--help")}; }
    bool hasHelp = false;
};

struct External
{
    QString name;
    QString summary;
    QString category;
    QString homepage;
    QString license;
    QString reason;         // not-in-debian | not-in-debian-main | proprietary | vendor-terms
    QString why;
    QString how;
    QString howUrl;
};

struct Profile
{
    QString id;
    QString name;
    QString summary;
    QString description;
    QString icon;
    QString metapackage;
    QStringList includes;
    QList<Tool> tools;          // own tools, as listed
    QList<External> external;   // own entries
};

class Manifests
{
public:
    // Reads categories.json and every profile manifest in 'dir'. Returns false
    // and sets error() when the directory is unusable; a single broken
    // manifest is skipped and reported in warnings().
    bool load(const QString &dir);

    QString directory() const { return m_dir; }
    QString error() const { return m_error; }
    QStringList warnings() const { return m_warnings; }

    const QList<Category> &categories() const { return m_categories; }
    const QList<Profile> &profiles() const { return m_profiles; }
    const Profile *profile(const QString &id) const;
    Category category(const QString &id) const;

    // Own tools followed by those of included profiles; one entry per package.
    QList<Tool> effectiveTools(const QString &profileId) const;
    QList<External> effectiveExternal(const QString &profileId) const;
    // Ids of the profiles that list 'package' themselves.
    QStringList profilesOf(const QString &package) const;
    // Every distinct package over all profiles.
    QStringList allPackages() const;
    QStringList allMetapackages() const;
    // First entry for 'package' (identity fields are equal in every profile).
    const Tool *tool(const QString &package) const;

private:
    QStringList includedProfiles(const QString &id) const;

    QString m_dir;
    QString m_error;
    QStringList m_warnings;
    QList<Category> m_categories;
    QList<Profile> m_profiles;
};

} // namespace Arsenal
