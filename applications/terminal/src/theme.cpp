// SPDX-License-Identifier: GPL-3.0-or-later
#include "theme.h"

#include "tokens.h"

#include <appearance.h>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QEvent>
#include <QFile>
#include <QIconEngine>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QRegularExpression>
#include <QStyleFactory>
#include <QSvgRenderer>

#include <atomic>

namespace {

std::atomic<int> g_styleSheetErrors{0};

QString iconPath(const QString &name)
{
    // This application's own glyphs first, then the shared NEXUS set that is
    // compiled into the UI library.
    const QString own = QStringLiteral(":/terminal/icons/") + name + QStringLiteral(".svg");
    if (QFile::exists(own))
        return own;
    return QStringLiteral(":/nexus/icons/") + name + QStringLiteral(".svg");
}

// Renders a "currentColor" SVG glyph in a colour that depends on the icon
// mode, at whatever size and device pixel ratio the widget asks for.
class SymbolicIconEngine : public QIconEngine
{
public:
    SymbolicIconEngine(const QByteArray &svg, const QColor &normal, const QColor &active,
                       const QColor &checked, const QColor &disabled)
        : m_svg(svg), m_normal(normal), m_active(active), m_checked(checked), m_disabled(disabled)
    {
    }

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State state) override
    {
        QByteArray svg = m_svg;
        svg.replace("currentColor", colorFor(mode, state).name(QColor::HexRgb).toLatin1());
        QSvgRenderer renderer(svg);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        // Keep the glyph square inside whatever rectangle the style hands us.
        const int side = qMin(rect.width(), rect.height());
        QRectF target(0, 0, side, side);
        target.moveCenter(QRectF(rect).center());
        renderer.render(painter, target);
        painter->restore();
    }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override
    {
        const QSize deviceSize = (QSizeF(size) * scale).toSize();
        QPixmap pm(deviceSize.isEmpty() ? QSize(1, 1) : deviceSize);
        pm.fill(Qt::transparent);
        QPainter painter(&pm);
        paint(&painter, QRect(QPoint(0, 0), pm.size()), mode, state);
        painter.end();
        pm.setDevicePixelRatio(scale);
        return pm;
    }

    QIconEngine *clone() const override
    {
        return new SymbolicIconEngine(m_svg, m_normal, m_active, m_checked, m_disabled);
    }

private:
    QColor colorFor(QIcon::Mode mode, QIcon::State state) const
    {
        if (mode == QIcon::Disabled)
            return m_disabled;
        if (state == QIcon::On)
            return m_checked;
        if (mode == QIcon::Active || mode == QIcon::Selected)
            return m_active;
        return m_normal;
    }

    QByteArray m_svg;
    QColor m_normal, m_active, m_checked, m_disabled;
};

QRegion roundedRegion(const QRect &rect, int radius)
{
    QPainterPath path;
    path.addRoundedRect(QRectF(rect), radius, radius);
    return QRegion(path.toFillPolygon().toPolygon());
}

} // namespace

Theme *Theme::instance()
{
    static Theme *self = new Theme(qApp);
    return self;
}

Theme::Theme(QObject *parent)
    : QObject(parent)
{
    auto *appearance = Nexus::Appearance::instance();
    connect(appearance, &Nexus::Appearance::darkChanged, this, &Theme::apply);
    connect(appearance, &Nexus::Appearance::accentChanged, this, &Theme::apply);
    connect(appearance, &Nexus::Appearance::fontScaleChanged, this, &Theme::apply);
    // Transparency and motion do not change the style sheet, but the terminal
    // panes react to them through changed().
    connect(appearance, &Nexus::Appearance::transparencyChanged, this, &Theme::changed);
    connect(appearance, &Nexus::Appearance::reduceMotionChanged, this, &Theme::changed);
}

int Theme::styleSheetErrors()
{
    return g_styleSheetErrors.load();
}

void Theme::noteStyleSheetError()
{
    ++g_styleSheetErrors;
}

qreal Theme::fontScale() const
{
    return Nexus::Appearance::instance()->fontScale();
}

bool Theme::transparency() const
{
    return Nexus::Appearance::instance()->transparency();
}

bool Theme::reduceMotion() const
{
    return Nexus::Appearance::instance()->reduceMotion();
}

int Theme::scaled(int tokenPixelSize) const
{
    return qMax(1, qRound(tokenPixelSize * fontScale()));
}

QFont Theme::font(int tokenPixelSize, int weight) const
{
    QFont f(QString::fromLatin1(Tokens::FontUi));
    f.setStyleHint(QFont::SansSerif);
    f.setPixelSize(scaled(tokenPixelSize));
    if (weight > 0)
        f.setWeight(QFont::Weight(weight));
    return f;
}

QColor Theme::color(const QString &token) const
{
    const auto it = m_colors.constFind(token);
    if (it != m_colors.constEnd())
        return it.value();
    qWarning("nexus-terminal: unknown colour token '%s'", qPrintable(token));
    return QColor(Qt::magenta);
}

void Theme::apply()
{
    const auto *appearance = Nexus::Appearance::instance();
    m_dark = appearance->dark();
    const QString mode = m_dark ? QStringLiteral("dark") : QStringLiteral("light");

    // An accent this build does not know (newer token file than binary) falls
    // back to the default accent instead of an unstyled window.
    QString variant = mode + QLatin1Char('-') + appearance->accent();
    if (!QFile::exists(QStringLiteral(":/terminal/theme/%1.json").arg(variant)))
        variant = mode + QLatin1Char('-') + QString::fromLatin1(Tokens::AccentDefault);
    m_variant = variant;

    // ---- colour table -----------------------------------------------------
    m_colors.clear();
    QFile table(QStringLiteral(":/terminal/theme/%1.json").arg(variant));
    if (table.open(QIODevice::ReadOnly)) {
        const QJsonObject obj = QJsonDocument::fromJson(table.readAll()).object();
        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
            const QColor c(it.value().toString());
            if (c.isValid())
                m_colors.insert(it.key(), c);
        }
    } else {
        qWarning("nexus-terminal: theme colour table '%s' is missing", qPrintable(variant));
    }

    // ---- palette -------------------------------------------------------------
    // Everything the style sheet does not address explicitly (plain labels,
    // QTermWidget's scroll bar, standard dialogs) takes its colours from here.
    QPalette pal;
    const QColor text = color("textPrimary");
    const QColor disabled = color("textDisabled");
    QColor selection = color("accent");
    selection.setAlphaF(0.28f);
    pal.setColor(QPalette::Window, color("bgBase"));
    pal.setColor(QPalette::WindowText, text);
    pal.setColor(QPalette::Base, color("bgInput"));
    pal.setColor(QPalette::AlternateBase, color("bgSurface"));
    pal.setColor(QPalette::Text, text);
    pal.setColor(QPalette::Button, color("bgRaised"));
    pal.setColor(QPalette::ButtonText, text);
    pal.setColor(QPalette::BrightText, color("textOnAccent"));
    pal.setColor(QPalette::ToolTipBase, color("bgOverlay"));
    pal.setColor(QPalette::ToolTipText, text);
    pal.setColor(QPalette::PlaceholderText, color("textMuted"));
    pal.setColor(QPalette::Highlight, selection);
    pal.setColor(QPalette::HighlightedText, text);
    pal.setColor(QPalette::Link, color("accent"));
    pal.setColor(QPalette::LinkVisited, color("accent"));
    pal.setColor(QPalette::Light, color("borderStrong"));
    pal.setColor(QPalette::Midlight, color("border"));
    pal.setColor(QPalette::Mid, color("border"));
    pal.setColor(QPalette::Dark, color("borderSubtle"));
    pal.setColor(QPalette::Shadow, color("shadow"));
    for (const QPalette::ColorRole role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        pal.setColor(QPalette::Disabled, role, disabled);

    // ---- style sheet ------------------------------------------------------------
    QString sheet;
    QFile qss(QStringLiteral(":/terminal/style/%1.qss").arg(variant));
    if (qss.open(QIODevice::ReadOnly))
        sheet = QString::fromUtf8(qss.readAll());
    else
        qWarning("nexus-terminal: style sheet '%s' is missing", qPrintable(variant));

    // The sheet carries the token font sizes; apply the user's font scale.
    const qreal scale = fontScale();
    if (!qFuzzyCompare(scale, 1.0)) {
        static const QRegularExpression fontSize(QStringLiteral("font-size:\\s*(\\d+)px"));
        QString scaledSheet;
        scaledSheet.reserve(sheet.size());
        qsizetype last = 0;
        auto it = fontSize.globalMatch(sheet);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            scaledSheet += QStringView(sheet).mid(last, m.capturedStart() - last);
            scaledSheet += QStringLiteral("font-size: %1px").arg(qRound(m.captured(1).toInt() * scale));
            last = m.capturedEnd();
        }
        scaledSheet += QStringView(sheet).mid(last);
        sheet = scaledSheet;
    }

    if (!m_applied) {
        // Fusion is the neutral base the style sheet is written against; the
        // platform style would otherwise leak its own frames and metrics.
        QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
        m_applied = true;
    }
    QApplication::setPalette(pal);
    // Setting the default font also drops the per-class fonts a platform
    // theme hands out for menus, labels, buttons, ...: one UI font for all.
    QApplication::setFont(font(Tokens::FontSizeBody));
    qApp->setStyleSheet(sheet);

    refreshIcons();
    Q_EMIT changed();
}

bool Theme::hasIcon(const QString &name)
{
    return QFile::exists(iconPath(name));
}

QIcon Theme::icon(const QString &name, const QString &token, const QString &activeToken) const
{
    QFile file(iconPath(name));
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning("nexus-terminal: unknown symbolic icon '%s'", qPrintable(name));
        return QIcon();
    }
    return QIcon(new SymbolicIconEngine(file.readAll(), color(token), color(activeToken),
                                        color("accent"), color("textDisabled")));
}

void Theme::bindIcon(QAction *action, const QString &name)
{
    // In menus the highlighted entry shows its glyph in the accent colour.
    const QString token = QStringLiteral("textSecondary");
    const QString active = QStringLiteral("accent");
    action->setIcon(icon(name, token, active));
    m_bindings.append({action, name, token, active});
}

void Theme::bindIcon(QAbstractButton *button, const QString &name, const QString &token)
{
    const QString active = QStringLiteral("textPrimary");
    button->setIcon(icon(name, token, active));
    m_bindings.append({button, name, token, active});
}

void Theme::refreshIcons()
{
    for (auto it = m_bindings.begin(); it != m_bindings.end();) {
        if (!it->target) {
            it = m_bindings.erase(it);
            continue;
        }
        const QIcon themed = icon(it->name, it->token, it->activeToken);
        if (auto *action = qobject_cast<QAction *>(it->target.data()))
            action->setIcon(themed);
        else if (auto *button = qobject_cast<QAbstractButton *>(it->target.data()))
            button->setIcon(themed);
        ++it;
    }
}

void Theme::prepareMenu(QMenu *menu)
{
    // Rounded corners need pixels outside the border to be see-through. That
    // takes a translucent window, which must be requested before the native
    // window exists. On X11 the shape mask additionally cuts the corners, so
    // the result is right with and without a compositing manager. Other
    // platforms keep the square menu the style sheet draws by default.
    const QString platform = QGuiApplication::platformName();
    const bool wayland = platform.startsWith(QLatin1String("wayland"));
    const bool x11 = platform == QLatin1String("xcb");
    if (!wayland && !x11)
        return;
    menu->setAttribute(Qt::WA_TranslucentBackground);
    menu->setWindowFlag(Qt::NoDropShadowWindowHint);
    menu->setProperty("nxRounded", true);
    if (x11)
        menu->installEventFilter(this);
}

bool Theme::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Resize) {
        if (auto *menu = qobject_cast<QMenu *>(watched))
            menu->setMask(roundedRegion(menu->rect(), Tokens::RadiusMd));
    }
    return QObject::eventFilter(watched, event);
}
