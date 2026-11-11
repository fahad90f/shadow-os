// SPDX-License-Identifier: GPL-3.0-or-later
#include "catalog.h"

#include <asyncrun.h>

#include <AppStreamQt/component-box.h>
#include <AppStreamQt/component.h>
#include <AppStreamQt/contentrating.h>
#include <AppStreamQt/developer.h>
#include <AppStreamQt/icon.h>
#include <AppStreamQt/image.h>
#include <AppStreamQt/launchable.h>
#include <AppStreamQt/pool.h>
#include <AppStreamQt/release-list.h>
#include <AppStreamQt/release.h>
#include <AppStreamQt/screenshot.h>
#include <AppStreamQt/spdx.h>
#include <AppStreamQt/utils.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QIcon>
#include <QLocale>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>
#include <QXmlStreamReader>

#include <algorithm>

namespace {

bool isWebUrl(const QUrl &url)
{
    return url.isValid() && (url.scheme() == QLatin1String("https") || url.scheme() == QLatin1String("http"))
           && !url.host().isEmpty();
}

// Pictures (icons, screenshots) are fetched by the store itself and only over
// HTTPS; metadata that points to plain-http media is not used.
bool isHttpsUrl(const QUrl &url)
{
    return url.isValid() && url.scheme() == QLatin1String("https") && !url.host().isEmpty();
}

bool isApplicationKind(AppStream::Component::Kind kind)
{
    return kind == AppStream::Component::KindDesktopApp || kind == AppStream::Component::KindConsoleApp
           || kind == AppStream::Component::KindWebApp;
}

// AppStream description markup (<p>, <ul>/<ol> with <li>, <em>, <code>) as a
// list of blocks the page lays out itself:
//   { type: "p" | "li", text, ordered, number }
// 'text' is safe for Text.StyledText: every character of the original text is
// HTML-escaped and the only markup produced here is <i>…</i>. Unknown
// elements are dropped (their text is kept), so descriptions cannot inject
// images, links or styling.
QVariantList markupToBlocks(const QString &markup)
{
    QVariantList blocks;
    if (markup.trimmed().isEmpty())
        return blocks;

    QXmlStreamReader xml(QStringLiteral("<root>") + markup + QStringLiteral("</root>"));
    QString text;
    bool ordered = false;
    int number = 0;
    auto flush = [&](const QString &type) {
        const QString t = text.simplified();
        if (!t.isEmpty()) {
            blocks << QVariantMap{
                {QStringLiteral("type"), type},
                {QStringLiteral("text"), t},
                {QStringLiteral("ordered"), type == QLatin1String("li") && ordered},
                {QStringLiteral("number"), type == QLatin1String("li") ? number : 0},
            };
        }
        text.clear();
    };

    while (!xml.atEnd()) {
        const QXmlStreamReader::TokenType token = xml.readNext();
        const QStringView name = xml.name();
        if (token == QXmlStreamReader::StartElement) {
            if (name == QLatin1String("p")) {
                flush(QStringLiteral("p"));
            } else if (name == QLatin1String("ul") || name == QLatin1String("ol")) {
                flush(QStringLiteral("p"));
                ordered = name == QLatin1String("ol");
                number = 0;
            } else if (name == QLatin1String("li")) {
                text.clear();
                ++number;
            } else if (name == QLatin1String("em")) {
                text += QStringLiteral("<i>");
            }
        } else if (token == QXmlStreamReader::EndElement) {
            if (name == QLatin1String("p"))
                flush(QStringLiteral("p"));
            else if (name == QLatin1String("li"))
                flush(QStringLiteral("li"));
            else if (name == QLatin1String("em"))
                text += QStringLiteral("</i>");
        } else if (token == QXmlStreamReader::Characters) {
            text += xml.text().toString().toHtmlEscaped();
        }
    }
    flush(QStringLiteral("p"));

    if (xml.hasError()) {
        // Not well-formed: show it as one plain paragraph without any tags.
        QString plain = markup;
        static const QRegularExpression tags(QStringLiteral("<[^>]*>"));
        plain.replace(tags, QStringLiteral(" "));
        blocks = {QVariantMap{
            {QStringLiteral("type"), QStringLiteral("p")},
            {QStringLiteral("text"), plain.simplified().toHtmlEscaped()},
            {QStringLiteral("ordered"), false},
            {QStringLiteral("number"), 0},
        }};
    }
    return blocks;
}

QString blocksToPlainText(const QVariantList &blocks)
{
    QStringList lines;
    for (const QVariant &v : blocks) {
        const QVariantMap b = v.toMap();
        QString t = b.value(QStringLiteral("text")).toString();
        t.remove(QStringLiteral("<i>")).remove(QStringLiteral("</i>"));
        t.replace(QStringLiteral("&lt;"), QStringLiteral("<")).replace(QStringLiteral("&gt;"), QStringLiteral(">"))
            .replace(QStringLiteral("&quot;"), QStringLiteral("\"")).replace(QStringLiteral("&amp;"), QStringLiteral("&"));
        if (b.value(QStringLiteral("type")).toString() == QLatin1String("li"))
            t.prepend(b.value(QStringLiteral("ordered")).toBool()
                          ? QStringLiteral("%1. ").arg(b.value(QStringLiteral("number")).toInt()) : QStringLiteral("• "));
        lines << t;
    }
    return lines.join(QLatin1Char('\n'));
}

void collectIcons(const AppStream::Component &component, AppEntry &entry)
{
    uint localBest = 0, remoteBest = 0;
    const QList<AppStream::Icon> icons = component.icons();
    for (const AppStream::Icon &icon : icons) {
        const uint width = icon.width();
        switch (icon.kind()) {
        case AppStream::Icon::KindCached:
        case AppStream::Icon::KindLocal: {
            const QUrl url = icon.url();
            const QString path = url.isLocalFile() ? url.toLocalFile() : QString();
            // Prefer 64 px and above, but nothing larger than needed.
            if (!path.isEmpty() && QFileInfo::exists(path)
                && (entry.localIcon.isEmpty() || (localBest < 64 && width > localBest) || (width >= 64 && width < localBest))) {
                entry.localIcon = path;
                localBest = width;
            }
            break;
        }
        case AppStream::Icon::KindStock:
            if (entry.stockIcon.isEmpty())
                entry.stockIcon = icon.name();
            break;
        case AppStream::Icon::KindRemote: {
            const QUrl url = icon.url();
            if (isHttpsUrl(url)
                && (entry.remoteIcon.isEmpty() || (remoteBest < 64 && width > remoteBest) || (width >= 64 && width < remoteBest))) {
                entry.remoteIcon = url.toString();
                remoteBest = width;
            }
            break;
        }
        case AppStream::Icon::KindUnknown:
            break;
        }
    }
}

quint32 categoryBits(const QStringList &componentCategories)
{
    quint32 bits = 0;
    const QList<Catalog::CategoryDefinition> &defs = Catalog::categoryDefinitions();
    for (int i = 0; i < defs.size(); ++i) {
        for (const QString &c : defs.at(i).appstream) {
            if (componentCategories.contains(c)) {
                bits |= (1u << i);
                break;
            }
        }
    }
    return bits;
}

} // namespace

struct Catalog::Loaded
{
    std::shared_ptr<AppStream::Pool> pool;
    bool ok = false;
    QString error;
    QList<AppEntry> entries;
    int componentCount = 0;
};

const QList<Catalog::CategoryDefinition> &Catalog::categoryDefinitions()
{
    // The curated NEXUS categories and the freedesktop.org main categories
    // (plus "Security") that AppStream components carry.
    static const QList<CategoryDefinition> defs = {
        {QStringLiteral("security"), tr("Security"), QStringLiteral("shield"), {QStringLiteral("Security")}},
        {QStringLiteral("development"), tr("Development"), QStringLiteral("code"), {QStringLiteral("Development")}},
        {QStringLiteral("internet"), tr("Internet"), QStringLiteral("globe"), {QStringLiteral("Network")}},
        {QStringLiteral("office"), tr("Office"), QStringLiteral("file-text"), {QStringLiteral("Office")}},
        {QStringLiteral("graphics"), tr("Graphics"), QStringLiteral("palette"), {QStringLiteral("Graphics")}},
        {QStringLiteral("multimedia"), tr("Multimedia"), QStringLiteral("music"),
         {QStringLiteral("AudioVideo"), QStringLiteral("Audio"), QStringLiteral("Video")}},
        {QStringLiteral("system"), tr("System"), QStringLiteral("cpu"), {QStringLiteral("System"), QStringLiteral("Settings")}},
        {QStringLiteral("utilities"), tr("Utilities"), QStringLiteral("wrench"), {QStringLiteral("Utility")}},
        {QStringLiteral("games"), tr("Games"), QStringLiteral("target"), {QStringLiteral("Game")}},
        {QStringLiteral("science"), tr("Science & Education"), QStringLiteral("flask"),
         {QStringLiteral("Science"), QStringLiteral("Education")}},
    };
    return defs;
}

Catalog::Catalog(QObject *parent)
    : QObject(parent)
{
    m_categoryCounts.fill(0, categoryDefinitions().size());
}

Catalog::~Catalog() = default;

QString Catalog::appstreamVersion() const
{
    return AppStream::Utils::currentAppStreamVersion();
}

QVariantList Catalog::categories() const
{
    QVariantList out;
    const QList<CategoryDefinition> &defs = categoryDefinitions();
    for (int i = 0; i < defs.size(); ++i) {
        out << QVariantMap{
            {QStringLiteral("key"), defs.at(i).key},
            {QStringLiteral("title"), defs.at(i).title},
            {QStringLiteral("icon"), defs.at(i).icon},
            {QStringLiteral("count"), m_categoryCounts.value(i)},
            {QStringLiteral("appstreamCategories"), defs.at(i).appstream},
        };
    }
    return out;
}

void Catalog::load()
{
    const quint64 serial = ++m_loadSerial;
    m_state = QStringLiteral("loading");
    m_error.clear();
    Q_EMIT stateChanged();

    // The pool belongs to the GUI thread; the worker only calls load() on it
    // and reads it. It is deleted on the GUI thread whenever its last user lets go.
    std::shared_ptr<AppStream::Pool> pool(new AppStream::Pool, [](AppStream::Pool *p) { p->deleteLater(); });
    // The distribution's catalogue (DEP-11 data downloaded by APT) and the
    // metainfo of installed software. No Flatpak or other bundle sources:
    // NEXUS Store installs DEB packages only.
    pool->setFlags(AppStream::Pool::Flags(AppStream::Pool::FlagLoadOsCatalog) | AppStream::Pool::FlagLoadOsMetainfo);

    NexusPk::runAsync(this, [pool] {
        Loaded result;
        result.pool = pool;
        result.ok = pool->load();
        if (!result.ok)
            result.error = pool->lastError();

        const AppStream::ComponentBox box = pool->components();
        result.componentCount = int(box.size());
        const qint64 newestPlausible = QDateTime::currentSecsSinceEpoch() + 24 * 3600;
        QHash<QString, int> seen;
        for (const AppStream::Component &c : box) {
            AppEntry e;
            e.id = c.id();
            if (e.id.isEmpty())
                continue;
            e.name = c.name();
            e.summary = c.summary();
            e.packageName = c.packageNames().value(0);
            e.kind = AppStream::Component::kindToString(c.kind());
            e.application = isApplicationKind(c.kind());
            e.categories = e.application ? categoryBits(c.categories()) : 0;
            collectIcons(c, e);
            const QList<AppStream::Release> releases = c.releasesPlain().entries();
            for (const AppStream::Release &r : releases) {
                const QDateTime when = r.timestamp();
                // Some metadata carries release dates in the future (typos,
                // announced releases); those are not "the newest release".
                if (when.isValid() && when.toSecsSinceEpoch() <= newestPlausible && when.toSecsSinceEpoch() > e.releaseTime) {
                    e.releaseTime = when.toSecsSinceEpoch();
                    e.releaseVersion = r.version();
                }
            }

            // The same component can come from several sources (catalogue and
            // installed metainfo, two suites). Keep one, preferring the one
            // that names its package.
            const auto it = seen.constFind(e.id);
            if (it != seen.constEnd()) {
                if (result.entries.at(it.value()).packageName.isEmpty() && !e.packageName.isEmpty())
                    result.entries[it.value()] = e;
                continue;
            }
            seen.insert(e.id, int(result.entries.size()));
            result.entries.append(e);
        }
        return result;
    }, [this, serial](const Loaded &result) {
        if (serial != m_loadSerial)
            return;
        apply(result);
    });
}

void Catalog::apply(const Loaded &result)
{
    m_pool = result.pool;
    m_entries = result.entries;
    m_componentCount = result.componentCount;
    m_byId.clear();
    m_byPackage.clear();
    m_themeIconKnown.clear();
    m_categoryCounts.fill(0, categoryDefinitions().size());
    m_applicationCount = 0;
    m_uncategorizedCount = 0;

    for (int i = 0; i < m_entries.size(); ++i) {
        const AppEntry &e = m_entries.at(i);
        m_byId.insert(e.id, i);
        if (!e.packageName.isEmpty()) {
            const auto it = m_byPackage.constFind(e.packageName);
            // Several components can ship in one package; applications win.
            if (it == m_byPackage.constEnd() || (!m_entries.at(it.value()).application && e.application))
                m_byPackage.insert(e.packageName, i);
        }
        if (!e.application)
            continue;
        ++m_applicationCount;
        if (e.categories == 0)
            ++m_uncategorizedCount;
        for (int c = 0; c < m_categoryCounts.size(); ++c) {
            if (e.categories & (1u << c))
                ++m_categoryCounts[c];
        }
    }

    if (!result.ok && m_entries.isEmpty()) {
        m_state = QStringLiteral("error");
        m_error = result.error;
    } else {
        // A pool that loads but is empty is a valid state: no catalogue has
        // been downloaded yet. The UI explains it; it is not an error.
        m_state = QStringLiteral("ready");
        m_error = result.ok ? QString() : result.error;
    }
    Q_EMIT stateChanged();
    Q_EMIT loaded();
}

int Catalog::indexOfId(const QString &id) const
{
    return m_byId.value(id, -1);
}

int Catalog::indexOfPackage(const QString &packageName) const
{
    return m_byPackage.value(packageName, -1);
}

QList<int> Catalog::applications() const
{
    QList<int> out;
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).application)
            out << i;
    }
    return out;
}

QList<int> Catalog::applicationsInCategory(const QString &key) const
{
    QList<int> out;
    const QList<CategoryDefinition> &defs = categoryDefinitions();
    int bit = -1;
    for (int i = 0; i < defs.size(); ++i) {
        if (defs.at(i).key == key)
            bit = i;
    }
    if (bit < 0)
        return out;
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).application && (m_entries.at(i).categories & (1u << bit)))
            out << i;
    }
    std::sort(out.begin(), out.end(), [this](int a, int b) {
        return QString::localeAwareCompare(m_entries.at(a).name, m_entries.at(b).name) < 0;
    });
    return out;
}

QList<int> Catalog::recentlyReleased(int limit) const
{
    QList<int> out;
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).application && m_entries.at(i).releaseTime > 0)
            out << i;
    }
    std::sort(out.begin(), out.end(), [this](int a, int b) {
        return m_entries.at(a).releaseTime > m_entries.at(b).releaseTime;
    });
    if (out.size() > limit)
        out.resize(limit);
    return out;
}

void Catalog::search(const QString &text, QObject *context, std::function<void(const QList<int> &)> done)
{
    const QString term = text.trimmed();
    const std::shared_ptr<AppStream::Pool> pool = m_pool;
    if (!pool || term.isEmpty()) {
        QTimer::singleShot(0, context, [done] { done({}); });
        return;
    }
    NexusPk::runAsync(context, [pool, term] {
        QStringList ids;
        const AppStream::ComponentBox box = pool->search(term);
        for (const AppStream::Component &c : box) {
            ids << c.id();
            if (ids.size() >= 500)
                break;
        }
        return ids;
    }, [this, done](const QStringList &ids) {
        // Map to the current index (it may have been reloaded meanwhile).
        QList<int> out;
        QSet<int> seen;
        for (const QString &id : ids) {
            const int i = m_byId.value(id, -1);
            if (i >= 0 && m_entries.at(i).application && !seen.contains(i)) {
                seen.insert(i);
                out << i;
            }
        }
        done(out);
    });
}

QString Catalog::iconSource(const AppEntry &entry, bool *remote) const
{
    if (remote)
        *remote = false;
    if (!entry.localIcon.isEmpty())
        return QStringLiteral("image://nxtheme/") + QString::fromLatin1(QUrl::toPercentEncoding(entry.localIcon));
    if (!entry.stockIcon.isEmpty()) {
        auto it = m_themeIconKnown.constFind(entry.stockIcon);
        if (it == m_themeIconKnown.constEnd())
            it = m_themeIconKnown.insert(entry.stockIcon, QIcon::hasThemeIcon(entry.stockIcon));
        if (it.value())
            return QStringLiteral("image://nxtheme/") + QString::fromLatin1(QUrl::toPercentEncoding(entry.stockIcon));
    }
    if (!entry.remoteIcon.isEmpty()) {
        if (remote)
            *remote = true;
        return entry.remoteIcon;
    }
    return {};
}

QJsonObject Catalog::entryJson(int index) const
{
    if (index < 0 || index >= m_entries.size())
        return {};
    const AppEntry &e = m_entries.at(index);
    QJsonArray categories;
    const QList<CategoryDefinition> &defs = categoryDefinitions();
    for (int c = 0; c < defs.size(); ++c) {
        if (e.categories & (1u << c))
            categories.append(defs.at(c).key);
    }
    bool remote = false;
    const QString icon = iconSource(e, &remote);
    QJsonObject o{
        {QStringLiteral("id"), e.id},
        {QStringLiteral("name"), e.name},
        {QStringLiteral("summary"), e.summary},
        {QStringLiteral("packageName"), e.packageName},
        {QStringLiteral("kind"), e.kind},
        {QStringLiteral("categories"), categories},
        {QStringLiteral("icon"), icon},
        {QStringLiteral("iconRemote"), remote},
    };
    if (e.releaseTime > 0) {
        o.insert(QStringLiteral("latestRelease"), e.releaseVersion);
        o.insert(QStringLiteral("latestReleaseDate"),
                 QDateTime::fromSecsSinceEpoch(e.releaseTime).date().toString(Qt::ISODate));
    }
    return o;
}

QJsonObject Catalog::statusJson() const
{
    QJsonObject o{
        {QStringLiteral("loaded"), m_state == QLatin1String("ready")},
        {QStringLiteral("state"), m_state},
        {QStringLiteral("componentCount"), m_componentCount},
        {QStringLiteral("indexedComponents"), m_entries.size()},
        {QStringLiteral("applicationCount"), m_applicationCount},
        {QStringLiteral("uncategorizedApplications"), m_uncategorizedCount},
        {QStringLiteral("libraryVersion"), appstreamVersion()},
        {QStringLiteral("sources"), QJsonArray{QStringLiteral("os-catalog"), QStringLiteral("os-metainfo")}},
    };
    if (!m_error.isEmpty())
        o.insert(QStringLiteral("error"), m_error);
    return o;
}

QVariantMap Catalog::details(const QString &id) const
{
    return detailsJson(id).toVariantMap();
}

QJsonObject Catalog::detailsJson(const QString &id) const
{
    if (!m_pool || id.isEmpty())
        return {};
    const AppStream::ComponentBox box = m_pool->componentsById(id);
    if (box.isEmpty())
        return {};

    // Same preference as the index: the instance that names its package.
    std::optional<AppStream::Component> chosen;
    for (const AppStream::Component &c : box) {
        if (!chosen || (chosen->packageNames().isEmpty() && !c.packageNames().isEmpty()))
            chosen = c;
    }
    const AppStream::Component &c = *chosen;

    QJsonObject o = entryJson(indexOfId(id));
    o.insert(QStringLiteral("id"), c.id());
    o.insert(QStringLiteral("name"), c.name());
    o.insert(QStringLiteral("summary"), c.summary());
    o.insert(QStringLiteral("kind"), AppStream::Component::kindToString(c.kind()));
    o.insert(QStringLiteral("application"), isApplicationKind(c.kind()));
    o.insert(QStringLiteral("origin"), c.origin());
    o.insert(QStringLiteral("packageNames"), QJsonArray::fromStringList(c.packageNames()));
    o.insert(QStringLiteral("packageName"), c.packageNames().value(0));
    o.insert(QStringLiteral("appstreamCategories"), QJsonArray::fromStringList(c.categories()));

    const QVariantList blocks = markupToBlocks(c.description());
    o.insert(QStringLiteral("descriptionBlocks"), QJsonArray::fromVariantList(blocks));
    o.insert(QStringLiteral("description"), blocksToPlainText(blocks));

    // Who makes it, under which licence.
    QString developer = c.developer().name();
    if (developer.isEmpty())
        developer = c.projectGroup();
    o.insert(QStringLiteral("developer"), developer);
    o.insert(QStringLiteral("projectGroup"), c.projectGroup());
    const QString license = c.projectLicense();
    o.insert(QStringLiteral("projectLicense"), license);
    o.insert(QStringLiteral("licenseIsFree"), !license.isEmpty() && c.isFloss());
    const QString licenseUrl = license.isEmpty() ? QString() : AppStream::SPDX::licenseUrl(license);
    o.insert(QStringLiteral("licenseUrl"), isWebUrl(QUrl(licenseUrl)) ? licenseUrl : QString());

    // Links — only web addresses are passed on.
    QJsonObject urls;
    const QList<QPair<AppStream::Component::UrlKind, QString>> kinds = {
        {AppStream::Component::UrlKindHomepage, QStringLiteral("homepage")},
        {AppStream::Component::UrlKindBugtracker, QStringLiteral("bugtracker")},
        {AppStream::Component::UrlKindHelp, QStringLiteral("help")},
        {AppStream::Component::UrlKindFaq, QStringLiteral("faq")},
        {AppStream::Component::UrlKindDonation, QStringLiteral("donation")},
        {AppStream::Component::UrlKindTranslate, QStringLiteral("translate")},
        {AppStream::Component::UrlKindVcsBrowser, QStringLiteral("vcs-browser")},
        {AppStream::Component::UrlKindContribute, QStringLiteral("contribute")},
        {AppStream::Component::UrlKindContact, QStringLiteral("contact")},
    };
    for (const auto &k : kinds) {
        const QUrl url = c.url(k.first);
        if (isWebUrl(url))
            urls.insert(k.second, url.toString());
    }
    o.insert(QStringLiteral("urls"), urls);

    // Screenshots: remote images; the page fetches them lazily.
    QJsonArray screenshots;
    int videos = 0;
    const QList<AppStream::Screenshot> shots = c.screenshotsAll();
    for (const AppStream::Screenshot &shot : shots) {
        if (shot.mediaKind() != AppStream::Screenshot::MediaKindImage) {
            ++videos;
            continue;
        }
        QList<AppStream::Image> images = shot.images();
        if (images.isEmpty())
            images = shot.imagesAll();
        QUrl thumbUrl, sourceUrl;
        uint thumbWidth = 0, thumbHeight = 0, sourceWidth = 0, sourceHeight = 0;
        for (const AppStream::Image &image : std::as_const(images)) {
            const QUrl url = image.url();
            if (!isHttpsUrl(url))
                continue;
            if (image.kind() == AppStream::Image::KindThumbnail) {
                // The thumbnail closest to the strip's size (about 620 px wide), not smaller if possible.
                const uint w = image.width();
                const bool better = thumbUrl.isEmpty() || (thumbWidth < 600 && w > thumbWidth) || (w >= 600 && w < thumbWidth);
                if (better) {
                    thumbUrl = url;
                    thumbWidth = w;
                    thumbHeight = image.height();
                }
            } else if (sourceUrl.isEmpty() || image.width() > sourceWidth) {
                sourceUrl = url;
                sourceWidth = image.width();
                sourceHeight = image.height();
            }
        }
        if (thumbUrl.isEmpty() && sourceUrl.isEmpty())
            continue;
        if (thumbUrl.isEmpty()) {
            thumbUrl = sourceUrl;
            thumbWidth = sourceWidth;
            thumbHeight = sourceHeight;
        }
        if (sourceUrl.isEmpty()) {
            sourceUrl = thumbUrl;
            sourceWidth = thumbWidth;
            sourceHeight = thumbHeight;
        }
        screenshots.append(QJsonObject{
            {QStringLiteral("caption"), shot.caption()},
            {QStringLiteral("default"), shot.isDefault()},
            {QStringLiteral("thumbnail"), thumbUrl.toString()},
            {QStringLiteral("thumbnailWidth"), int(thumbWidth)},
            {QStringLiteral("thumbnailHeight"), int(thumbHeight)},
            {QStringLiteral("source"), sourceUrl.toString()},
            {QStringLiteral("sourceWidth"), int(sourceWidth)},
            {QStringLiteral("sourceHeight"), int(sourceHeight)},
        });
    }
    o.insert(QStringLiteral("screenshots"), screenshots);
    o.insert(QStringLiteral("videoCount"), videos);

    // Upstream releases known to AppStream (newest first, a few).
    QJsonArray releases;
    QList<AppStream::Release> entries = c.releasesPlain().entries();
    std::sort(entries.begin(), entries.end(), [](const AppStream::Release &a, const AppStream::Release &b) {
        return a.timestamp() > b.timestamp();
    });
    for (const AppStream::Release &r : std::as_const(entries)) {
        if (releases.size() >= 5)
            break;
        const QVariantList releaseBlocks = markupToBlocks(r.description());
        const QDateTime when = r.timestamp();
        releases.append(QJsonObject{
            {QStringLiteral("version"), r.version()},
            {QStringLiteral("date"), when.isValid() ? when.date().toString(Qt::ISODate) : QString()},
            {QStringLiteral("dateText"), when.isValid() ? QLocale().toString(when.date(), QLocale::LongFormat) : QString()},
            {QStringLiteral("descriptionBlocks"), QJsonArray::fromVariantList(releaseBlocks)},
            {QStringLiteral("description"), blocksToPlainText(releaseBlocks)},
        });
    }
    o.insert(QStringLiteral("releases"), releases);

    // Content rating (OARS), where the publisher declared one.
    QJsonArray ratings;
    const QList<AppStream::ContentRating> contentRatings = c.contentRatings();
    for (const AppStream::ContentRating &rating : contentRatings) {
        QJsonArray attributes;
        const QStringList ids = rating.ratingIds();
        for (const QString &ratingId : ids) {
            const AppStream::ContentRating::RatingValue value = rating.value(ratingId);
            if (value == AppStream::ContentRating::RatingValueUnknown || value == AppStream::ContentRating::RatingValueNone)
                continue;
            attributes.append(QJsonObject{
                {QStringLiteral("id"), ratingId},
                {QStringLiteral("value"), AppStream::ContentRating::ratingValueToString(value)},
                {QStringLiteral("description"), rating.description(ratingId)},
            });
        }
        ratings.append(QJsonObject{
            {QStringLiteral("kind"), rating.kind()},
            {QStringLiteral("minimumAge"), int(rating.minimumAge())},
            {QStringLiteral("attributes"), attributes},
        });
    }
    o.insert(QStringLiteral("contentRatings"), ratings);

    // How it is delivered. NEXUS Store only installs the DEB package; other
    // bundle kinds in the metadata are reported, not used.
    QJsonArray bundles;
    const QList<AppStream::Bundle> bundleList = c.bundles();
    for (const AppStream::Bundle &b : bundleList)
        bundles.append(QJsonObject{{QStringLiteral("kind"), AppStream::Bundle::kindToString(b.kind())},
                                   {QStringLiteral("id"), b.id()}});
    o.insert(QStringLiteral("bundles"), bundles);
    o.insert(QStringLiteral("desktopIds"),
             QJsonArray::fromStringList(c.launchable(AppStream::Launchable::KindDesktopId).entries()));
    o.insert(QStringLiteral("extends"), QJsonArray::fromStringList(c.extends()));
    return o;
}

// ------------------------------------------------------------ AppListModel

AppListModel::AppListModel(Catalog *catalog, QObject *parent)
    : QAbstractListModel(parent), m_catalog(catalog)
{
    // Indexes refer to the catalogue's current index; a reload invalidates them.
    connect(catalog, &Catalog::loaded, this, [this] { setIndexes({}); });
}

int AppListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_visible.size());
}

QHash<int, QByteArray> AppListModel::roleNames() const
{
    return {
        {AppIdRole, "appId"}, {NameRole, "name"}, {SummaryRole, "summary"}, {PackageNameRole, "packageName"},
        {IconRole, "icon"}, {IconRemoteRole, "iconRemote"}, {KindRole, "kind"}, {InstalledRole, "installed"},
        {ReleaseDateRole, "releaseDate"},
    };
}

QVariant AppListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_visible.size())
        return {};
    const int i = m_visible.at(index.row());
    if (i < 0 || i >= m_catalog->entries().size())
        return {};
    const AppEntry &e = m_catalog->entries().at(i);
    switch (role) {
    case Qt::DisplayRole:
    case NameRole: return e.name;
    case AppIdRole: return e.id;
    case SummaryRole: return e.summary;
    case PackageNameRole: return e.packageName;
    case IconRole: return m_catalog->iconSource(e);
    case IconRemoteRole: {
        bool remote = false;
        m_catalog->iconSource(e, &remote);
        return remote;
    }
    case KindRole: return e.kind;
    case InstalledRole: return m_installed && !e.packageName.isEmpty() && m_installed->contains(e.packageName);
    case ReleaseDateRole:
        return e.releaseTime > 0
            ? QLocale().toString(QDateTime::fromSecsSinceEpoch(e.releaseTime).date(), QLocale::ShortFormat) : QString();
    default: break;
    }
    return {};
}

void AppListModel::rebuild()
{
    m_visible.clear();
    const QList<AppEntry> &entries = m_catalog->entries();
    for (const int i : std::as_const(m_indexes)) {
        if (i < 0 || i >= entries.size())
            continue;
        if (m_filter.isEmpty() || entries.at(i).name.contains(m_filter, Qt::CaseInsensitive)
            || entries.at(i).summary.contains(m_filter, Qt::CaseInsensitive))
            m_visible << i;
    }
}

void AppListModel::setIndexes(const QList<int> &indexes)
{
    beginResetModel();
    m_indexes = indexes;
    rebuild();
    endResetModel();
    Q_EMIT countChanged();
}

void AppListModel::setFilter(const QString &filter)
{
    const QString f = filter.trimmed();
    if (f == m_filter)
        return;
    beginResetModel();
    m_filter = f;
    rebuild();
    endResetModel();
    Q_EMIT filterChanged();
    Q_EMIT countChanged();
}

void AppListModel::setInstalledPackages(const QSet<QString> *installed)
{
    m_installed = installed;
    installedChanged();
}

void AppListModel::installedChanged()
{
    if (!m_visible.isEmpty())
        Q_EMIT dataChanged(index(0, 0), index(int(m_visible.size()) - 1, 0), {InstalledRole});
}

void AppListModel::sortByName()
{
    const QList<AppEntry> &entries = m_catalog->entries();
    beginResetModel();
    std::stable_sort(m_indexes.begin(), m_indexes.end(), [&entries](int a, int b) {
        return QString::localeAwareCompare(entries.at(a).name, entries.at(b).name) < 0;
    });
    rebuild();
    endResetModel();
}

void AppListModel::sortByRelease()
{
    const QList<AppEntry> &entries = m_catalog->entries();
    beginResetModel();
    std::stable_sort(m_indexes.begin(), m_indexes.end(), [&entries](int a, int b) {
        return entries.at(a).releaseTime > entries.at(b).releaseTime;
    });
    rebuild();
    endResetModel();
}

QJsonArray AppListModel::toJson() const
{
    QJsonArray out;
    for (const int i : m_visible) {
        QJsonObject o = m_catalog->entryJson(i);
        o.insert(QStringLiteral("installed"),
                 m_installed && m_installed->contains(m_catalog->entries().at(i).packageName));
        out.append(o);
    }
    return out;
}
