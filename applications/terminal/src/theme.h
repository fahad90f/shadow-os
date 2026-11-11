// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QColor>
#include <QFont>
#include <QHash>
#include <QIcon>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

class QAbstractButton;
class QAction;
class QMenu;

// The look of the application for the current Shadow::Appearance.
//
// The window chrome is made of Qt widgets styled by a style sheet; the sheet,
// the colour table and the indicator glyphs are rendered from the design
// tokens at build time, one variant per appearance mode and accent
// (see CMakeLists.txt). Theme picks the variant that matches
// Shadow::Appearance, installs palette, font and style sheet, and switches
// live when the user changes the appearance in the Control Center.
class Theme : public QObject
{
    Q_OBJECT
public:
    static Theme *instance();

    // Install palette, application font and style sheet for the current
    // appearance. Called once at start-up; afterwards Theme follows
    // Shadow::Appearance by itself and emits changed().
    void apply();

    bool dark() const { return m_dark; }
    QString variant() const { return m_variant; }       // e.g. "dark-cyan"

    // Semantic colour by token name ("bgBase", "textSecondary", "accent", ...).
    QColor color(const QString &token) const;
    QColor color(const char *token) const { return color(QString::fromLatin1(token)); }

    // Appearance settings that are not colours.
    qreal fontScale() const;
    bool transparency() const;
    bool reduceMotion() const;

    // UI font at a token size (already multiplied by the font scale).
    QFont font(int tokenPixelSize, int weight = 0) const;
    int scaled(int tokenPixelSize) const;

    // Glyph from the SHADOW symbolic icon set, tinted for the current theme:
    // 'token' normally, 'activeToken' when hovered or highlighted, accent
    // when checked, textDisabled when disabled.
    QIcon icon(const QString &name, const QString &token = QStringLiteral("textSecondary"),
               const QString &activeToken = QStringLiteral("textPrimary")) const;
    static bool hasIcon(const QString &name);

    // Give an action or button a themed icon that is re-tinted on changes.
    void bindIcon(QAction *action, const QString &name);
    void bindIcon(QAbstractButton *button, const QString &name,
                  const QString &token = QStringLiteral("textSecondary"));

    // Prepare a menu for rounded corners where the window system can show a
    // translucent popup. Must be called before the menu is first shown.
    void prepareMenu(QMenu *menu);

    // Number of style sheet parse errors Qt reported (checked by the smoke test).
    static int styleSheetErrors();
    static void noteStyleSheetError();

Q_SIGNALS:
    void changed();

private:
    explicit Theme(QObject *parent = nullptr);
    bool eventFilter(QObject *watched, QEvent *event) override;
    void refreshIcons();

    struct IconBinding
    {
        QPointer<QObject> target;
        QString name;
        QString token;
        QString activeToken;
    };

    bool m_dark = true;
    bool m_applied = false;
    QString m_variant;
    QHash<QString, QColor> m_colors;
    QList<IconBinding> m_bindings;
};
