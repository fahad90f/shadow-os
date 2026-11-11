// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui.h"

#include "theme.h"
#include "tokens.h"

#include <QComboBox>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariantAnimation>

namespace Ui {

QLabel *label(const QString &text, const char *role, QWidget *parent)
{
    auto *l = new QLabel(text, parent);
    if (role)
        l->setProperty("nxRole", QString::fromLatin1(role));
    return l;
}

void setRole(QWidget *widget, const char *role)
{
    setProperty(widget, "nxRole", QString::fromLatin1(role));
}

QPushButton *button(const QString &text, const char *kind, QWidget *parent)
{
    auto *b = new QPushButton(text, parent);
    b->setProperty("nxKind", QString::fromLatin1(kind));
    b->setFixedHeight(Tokens::ControlHeight);
    b->setMinimumWidth(Tokens::ControlHeight * 2 + Tokens::SpaceXs);
    // Which button reacts to Return is decided per dialog, not by focus.
    b->setAutoDefault(false);
    return b;
}

QToolButton *iconButton(const QString &icon, const QString &toolTip, QWidget *parent)
{
    auto *b = new QToolButton(parent);
    Theme::instance()->bindIcon(b, icon);
    b->setIconSize(QSize(Tokens::IconSmall + Tokens::SpaceXxs, Tokens::IconSmall + Tokens::SpaceXxs));
    b->setFixedSize(Tokens::ControlHeight - Tokens::SpaceXs, Tokens::ControlHeight - Tokens::SpaceXs);
    b->setAutoRaise(true);
    b->setToolTip(toolTip);
    b->setAccessibleName(toolTip);
    // Reachable with Tab, but a click must not take the keyboard away from
    // the terminal.
    b->setFocusPolicy(Qt::TabFocus);
    return b;
}

QLineEdit *lineEdit(QWidget *parent)
{
    auto *e = new QLineEdit(parent);
    e->setFixedHeight(Tokens::ControlHeight);
    return e;
}

QComboBox *comboBox(QWidget *parent)
{
    auto *c = new QComboBox(parent);
    c->setFixedHeight(Tokens::ControlHeight);
    // A plain list view with the styled delegate, so the popup rows follow
    // the ::item rules of the style sheet.
    auto *view = new QListView(c);
    view->setUniformItemSizes(true);
    c->setView(view);
    c->setItemDelegate(new QStyledItemDelegate(c));
    c->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    c->setMaxVisibleItems(12);
    return c;
}

QSpinBox *spinBox(int minimum, int maximum, QWidget *parent)
{
    auto *s = new QSpinBox(parent);
    s->setRange(minimum, maximum);
    s->setFixedHeight(Tokens::ControlHeight);
    s->setAccelerated(true);
    return s;
}

QListWidgetItem *listItem(QListWidget *list, const QString &text)
{
    auto *item = new QListWidgetItem(text, list);
    item->setSizeHint(QSize(0, Tokens::ControlHeight + Tokens::SpaceXs));
    return item;
}

QMenu *menu(QWidget *parent)
{
    auto *m = new QMenu(parent);
    Theme::instance()->prepareMenu(m);
    return m;
}

QMenu *subMenu(QMenu *parent, const QString &title)
{
    auto *m = new QMenu(title, parent);
    Theme::instance()->prepareMenu(m);
    parent->addMenu(m);
    return m;
}

QFrame *divider(QWidget *parent)
{
    auto *line = new QFrame(parent);
    line->setObjectName(QStringLiteral("RowDivider"));
    line->setFixedHeight(Tokens::BorderWidth);
    return line;
}

QFrame *card(QWidget *parent)
{
    auto *frame = new QFrame(parent);
    frame->setObjectName(QStringLiteral("Card"));
    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(Tokens::SpaceLg, Tokens::SpaceXxs, Tokens::SpaceLg, Tokens::SpaceXxs);
    layout->setSpacing(0);
    return frame;
}

QVBoxLayout *cardLayout(QFrame *card)
{
    return qobject_cast<QVBoxLayout *>(card->layout());
}

QWidget *addRow(QFrame *card, const QString &title, const QString &description, QWidget *control)
{
    QVBoxLayout *layout = cardLayout(card);
    if (layout->count() > 0)
        layout->addWidget(divider(card));

    auto *row = new QWidget(card);
    row->setMinimumHeight(description.isEmpty() ? Tokens::RowHeight : Tokens::RowHeight + Tokens::SpaceMd);
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(0, Tokens::SpaceSm, 0, Tokens::SpaceSm);
    h->setSpacing(Tokens::SpaceLg);

    auto *texts = new QVBoxLayout;
    texts->setSpacing(Tokens::SpaceXxs);
    QLabel *titleLabel = label(title, "rowTitle", row);
    texts->addStretch(1);
    texts->addWidget(titleLabel);
    if (!description.isEmpty()) {
        QLabel *descriptionLabel = label(description, "caption", row);
        descriptionLabel->setWordWrap(true);
        texts->addWidget(descriptionLabel);
    }
    texts->addStretch(1);
    h->addLayout(texts, 1);

    if (control) {
        control->setParent(row);
        h->addWidget(control, 0, Qt::AlignVCenter);
        if (control->accessibleName().isEmpty())
            control->setAccessibleName(title);
        if (!description.isEmpty() && control->accessibleDescription().isEmpty())
            control->setAccessibleDescription(description);
        titleLabel->setBuddy(control);
    }
    layout->addWidget(row);
    return row;
}

void repolish(QWidget *widget)
{
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

void setProperty(QWidget *widget, const char *name, const QVariant &value)
{
    if (widget->property(name) == value)
        return;
    widget->setProperty(name, value);
    repolish(widget);
}

// ---- Switch ---------------------------------------------------------------------

namespace {
// Geometry of the SHADOW switch (desktop/ui/style/Switch.qml).
constexpr int kTrackWidth = 40;
constexpr int kTrackHeight = 22;
constexpr int kKnob = 16;
constexpr int kKnobInset = 3;
} // namespace

Switch::Switch(QWidget *parent)
    : QAbstractButton(parent)
    , m_animation(new QVariantAnimation(this))
{
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        m_position = value.toReal();
        update();
    });
}

QSize Switch::sizeHint() const
{
    return QSize(kTrackWidth + 2 * Tokens::SpaceXxs, kTrackHeight + 2 * Tokens::SpaceXxs);
}

void Switch::animateTo(bool on)
{
    const qreal target = on ? 1.0 : 0.0;
    m_animation->stop();
    if (!isVisible() || Theme::instance()->reduceMotion()) {
        m_position = target;
        update();
        return;
    }
    m_animation->setDuration(Tokens::MotionNormal);
    m_animation->setStartValue(m_position);
    m_animation->setEndValue(target);
    m_animation->start();
}

void Switch::checkStateSet()
{
    QAbstractButton::checkStateSet();
    animateTo(isChecked());
}

void Switch::nextCheckState()
{
    QAbstractButton::nextCheckState();
    animateTo(isChecked());
}

void Switch::paintEvent(QPaintEvent *)
{
    const Theme *theme = Theme::instance();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (!isEnabled())
        p.setOpacity(Tokens::OpacityDisabled);

    QRectF track(0, 0, kTrackWidth, kTrackHeight);
    track.moveCenter(QRectF(rect()).center());

    auto mix = [](const QColor &a, const QColor &b, qreal t) {
        return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                                a.blueF() + (b.blueF() - a.blueF()) * t);
    };
    const QColor accent = theme->color("accent");
    const QColor fill = mix(theme->color("bgPressed"), accent, m_position);
    const QColor border = hasFocus() ? accent : mix(theme->color("border"), accent, m_position);
    const QColor knob = mix(theme->color("textSecondary"), theme->color("textOnAccent"), m_position);

    const qreal borderWidth = hasFocus() ? Tokens::FocusRing : Tokens::BorderWidth;
    p.setPen(QPen(border, borderWidth));
    p.setBrush(fill);
    const QRectF inner = track.adjusted(borderWidth / 2, borderWidth / 2, -borderWidth / 2, -borderWidth / 2);
    p.drawRoundedRect(inner, inner.height() / 2, inner.height() / 2);

    const qreal travel = kTrackWidth - kKnob - 2 * kKnobInset;
    const QRectF knobRect(track.left() + kKnobInset + travel * m_position, track.top() + kKnobInset, kKnob, kKnob);
    p.setPen(Qt::NoPen);
    p.setBrush(knob);
    p.drawEllipse(knobRect);
}

// ---- Dialog -----------------------------------------------------------------------

Dialog::Dialog(const QString &title, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(title);
    setModal(true);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    m_header = new QWidget(this);
    auto *headerLayout = new QVBoxLayout(m_header);
    headerLayout->setContentsMargins(Tokens::SpaceXl, Tokens::SpaceXl, Tokens::SpaceXl, Tokens::SpaceSm);
    headerLayout->setSpacing(Tokens::SpaceXs);
    m_title = label(title, "title", m_header);
    m_subtitle = label(QString(), "secondary", m_header);
    m_subtitle->setWordWrap(true);
    m_subtitle->hide();
    headerLayout->addWidget(m_title);
    headerLayout->addWidget(m_subtitle);
    outer->addWidget(m_header);

    auto *body = new QWidget(this);
    m_content = new QVBoxLayout(body);
    m_content->setContentsMargins(Tokens::SpaceXl, Tokens::SpaceSm, Tokens::SpaceXl, Tokens::SpaceXl);
    m_content->setSpacing(Tokens::SpaceMd);
    outer->addWidget(body, 1);

    auto *footer = new QWidget(this);
    footer->setObjectName(QStringLiteral("DialogFooter"));
    footer->setAttribute(Qt::WA_StyledBackground, true);
    m_footer = new QHBoxLayout(footer);
    m_footer->setContentsMargins(Tokens::SpaceXl, Tokens::SpaceMd, Tokens::SpaceXl, Tokens::SpaceMd);
    m_footer->setSpacing(Tokens::SpaceSm);
    m_footer->addStretch(1);
    outer->addWidget(footer);
}

void Dialog::setSubtitle(const QString &text)
{
    m_subtitle->setText(text);
    m_subtitle->setVisible(!text.isEmpty());
}

void Dialog::hideHeader()
{
    m_header->hide();
    m_content->setContentsMargins(0, 0, 0, 0);
}

QPushButton *Dialog::addFooterButton(const QString &text, const char *kind)
{
    QPushButton *b = button(text, kind, this);
    m_footer->addWidget(b);
    return b;
}

void Dialog::addFooterWidget(QWidget *widget)
{
    m_footer->addWidget(widget);
}

void Dialog::addFooterStretch()
{
    // The layout starts with a stretch (right-aligned buttons). A group that
    // was added before this call ends up on the left: move the stretch behind it.
    QLayoutItem *first = m_footer->takeAt(0);
    delete first;
    m_footer->addStretch(1);
}

// ---- confirmations ------------------------------------------------------------------

bool confirm(QWidget *parent, const QString &title, const QString &message, const QString &confirmText,
             bool destructive)
{
    Dialog dialog(title, parent);
    dialog.setSubtitle(message);
    dialog.content()->setContentsMargins(Tokens::SpaceXl, 0, Tokens::SpaceXl, Tokens::SpaceXs);
    dialog.setFixedWidth(Tokens::SidebarWidth * 2 - Tokens::SpaceXxl);

    QPushButton *cancel = dialog.addFooterButton(QDialog::tr("Cancel"));
    QPushButton *accept = dialog.addFooterButton(confirmText, destructive ? "danger" : "primary");
    QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    QObject::connect(accept, &QPushButton::clicked, &dialog, &QDialog::accept);
    // A destructive action must be chosen deliberately: Return cancels.
    QPushButton *preferred = destructive ? cancel : accept;
    preferred->setDefault(true);
    preferred->setFocus();
    return dialog.exec() == QDialog::Accepted;
}

bool confirmPaste(QWidget *parent, const QString &text, bool controlsRemoved)
{
    const int lines = int(text.count(QLatin1Char('\n'))) + 1;
    Dialog dialog(lines > 1 ? QDialog::tr("Paste %n lines?", nullptr, lines) : QDialog::tr("Paste this text?"),
                  parent);
    dialog.setSubtitle(lines > 1
        ? QDialog::tr("Each line is entered as if you had typed it and pressed Return. "
                      "Text copied from a web page can contain commands you did not see.")
        : QDialog::tr("This is what would be entered. Text copied from a web page can contain "
                      "characters you did not see."));
    dialog.setFixedWidth(Tokens::SidebarWidth * 2 + Tokens::SpaceXxl * 2);

    auto *preview = new QPlainTextEdit(&dialog);
    preview->setReadOnly(true);
    preview->setLineWrapMode(QPlainTextEdit::NoWrap);
    preview->setPlainText(text);
    QFont mono(QString::fromLatin1(Tokens::FontMono));
    mono.setStyleHint(QFont::Monospace);
    mono.setPixelSize(Theme::instance()->scaled(Tokens::FontSizeSmall));
    preview->setFont(mono);
    preview->setFixedHeight(Tokens::RowHeight * 4);
    preview->setAccessibleName(QDialog::tr("Text to paste"));
    dialog.content()->addWidget(preview);

    if (controlsRemoved) {
        QLabel *note = label(QDialog::tr("Control characters were removed from this text."), "error", &dialog);
        note->setWordWrap(true);
        dialog.content()->addWidget(note);
    }

    QPushButton *cancel = dialog.addFooterButton(QDialog::tr("Cancel"));
    QPushButton *paste = dialog.addFooterButton(QDialog::tr("Paste"), "primary");
    QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    QObject::connect(paste, &QPushButton::clicked, &dialog, &QDialog::accept);
    // Pasting has to be a decision, not a reflex: Return cancels.
    cancel->setDefault(true);
    cancel->setFocus();
    return dialog.exec() == QDialog::Accepted;
}

} // namespace Ui
