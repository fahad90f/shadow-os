// SPDX-License-Identifier: GPL-3.0-or-later
#include "codeeditor.h"

#include <QApplication>
#include <QClipboard>
#include <QGuiApplication>
#include <QPainter>
#include <QQuickWindow>
#include <QScrollBar>
#include <QTextBlock>

namespace {

class Gutter : public QWidget
{
public:
    explicit Gutter(CodeWidget *editor) : QWidget(editor), m_editor(editor) {}
protected:
    void paintEvent(QPaintEvent *event) override { m_editor->paintGutter(event); }
private:
    CodeWidget *m_editor;
};

const int kGutterPadding = 12;

} // namespace

// ---- CodeWidget -------------------------------------------------------------

CodeWidget::CodeWidget(QWidget *parent)
    : QPlainTextEdit(parent)
{
    m_gutter = new Gutter(this);
    setFrameShape(QFrame::NoFrame);
    // Scroll bars are drawn by the QML side in the SHADOW style.
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setCursorWidth(2);
    setContextMenuPolicy(Qt::NoContextMenu);
    document()->setDocumentMargin(4);

    connect(this, &QPlainTextEdit::blockCountChanged, this, &CodeWidget::updateGutterGeometry);
    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect &, int) {
        m_gutter->update();
        Q_EMIT repaintNeeded();
    });
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &CodeWidget::refreshDecorations);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, &CodeWidget::refreshDecorations);
    updateGutterGeometry();
}

void CodeWidget::setColors(const Colors &colors)
{
    m_colors = colors;
    QPalette p = palette();
    for (const QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        p.setColor(group, QPalette::Base, colors.background);
        p.setColor(group, QPalette::Window, colors.background);
        p.setColor(group, QPalette::Text, colors.text);
        p.setColor(group, QPalette::Highlight, colors.selection);
        p.setColor(group, QPalette::HighlightedText, colors.text);
    }
    setPalette(p);
    refreshDecorations();
    m_gutter->update();
    Q_EMIT repaintNeeded();
}

void CodeWidget::setLineNumbers(bool visible)
{
    m_lineNumbers = visible;
    updateGutterGeometry();
}

void CodeWidget::setHighlightCurrentLine(bool on)
{
    m_highlightCurrentLine = on;
    refreshDecorations();
}

void CodeWidget::setMatches(const std::vector<std::pair<int, int>> *matches)
{
    m_matches = matches;
    refreshDecorations();
}

int CodeWidget::gutterWidth() const
{
    if (!m_lineNumbers)
        return 0;
    int digits = 1;
    for (int n = qMax(1, blockCount()); n >= 10; n /= 10)
        ++digits;
    return fontMetrics().horizontalAdvance(QLatin1Char('9')) * qMax(3, digits) + kGutterPadding * 2;
}

void CodeWidget::updateGutterGeometry()
{
    const int width = gutterWidth();
    setViewportMargins(width, 0, 0, 0);
    const QRect area = contentsRect();
    m_gutter->setGeometry(QRect(area.left(), area.top(), width, area.height()));
    m_gutter->setVisible(width > 0);
    Q_EMIT repaintNeeded();
}

void CodeWidget::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);
    updateGutterGeometry();
    refreshDecorations();
}

void CodeWidget::changeEvent(QEvent *event)
{
    QPlainTextEdit::changeEvent(event);
    if (event->type() == QEvent::FontChange)
        updateGutterGeometry();
}

void CodeWidget::mousePressEvent(QMouseEvent *event)
{
    // Pressing inside a selection would arm a text drag, and a drag started
    // by a widget that is not on screen cannot work: place the cursor instead.
    if (event->button() == Qt::LeftButton && textCursor().hasSelection() && !(event->modifiers() & Qt::ShiftModifier)) {
        const int position = cursorForPosition(event->position().toPoint()).position();
        const QTextCursor current = textCursor();
        if (position >= current.selectionStart() && position <= current.selectionEnd()) {
            QTextCursor cursor = current;
            cursor.setPosition(position);
            setTextCursor(cursor);
        }
    }
    QPlainTextEdit::mousePressEvent(event);
}

// Current-line bar and the search matches that are on screen.
void CodeWidget::refreshDecorations()
{
    QList<QTextEdit::ExtraSelection> selections;
    if (m_highlightCurrentLine && m_colors.currentLine.isValid()) {
        QTextEdit::ExtraSelection line;
        line.format.setBackground(m_colors.currentLine);
        line.format.setProperty(QTextFormat::FullWidthSelection, true);
        line.cursor = textCursor();
        line.cursor.clearSelection();
        selections.append(line);
    }
    if (m_matches && !m_matches->empty()) {
        const int first = cursorForPosition(QPoint(0, 0)).position();
        const int last = cursorForPosition(QPoint(viewport()->width(), viewport()->height())).position();
        auto it = std::lower_bound(m_matches->begin(), m_matches->end(), first,
                                   [](const std::pair<int, int> &m, int pos) { return m.second < pos; });
        const int limit = document()->characterCount();
        for (int shown = 0; it != m_matches->end() && it->first <= last && shown < 2000; ++it, ++shown) {
            if (it->second > limit)
                break;
            QTextEdit::ExtraSelection match;
            match.format.setBackground(m_colors.match);
            match.cursor = QTextCursor(document());
            match.cursor.setPosition(it->first);
            match.cursor.setPosition(it->second, QTextCursor::KeepAnchor);
            selections.append(match);
        }
    }
    setExtraSelections(selections);
}

void CodeWidget::paintGutter(QPaintEvent *event)
{
    QPainter painter(m_gutter);
    painter.fillRect(event->rect(), m_colors.gutter);
    painter.setPen(m_colors.divider);
    painter.drawLine(m_gutter->width() - 1, event->rect().top(), m_gutter->width() - 1, event->rect().bottom());
    painter.setFont(font());

    const int currentBlock = textCursor().blockNumber();
    QTextBlock block = firstVisibleBlock();
    int number = block.blockNumber();
    qreal top = blockBoundingGeometry(block).translated(contentOffset()).top();
    const int lineHeight = fontMetrics().height();
    while (block.isValid() && top <= event->rect().bottom()) {
        const qreal height = blockBoundingRect(block).height();
        if (block.isVisible() && top + height >= event->rect().top()) {
            painter.setPen(number == currentBlock ? m_colors.gutterCurrent : m_colors.gutterText);
            painter.drawText(QRectF(0, top, m_gutter->width() - kGutterPadding, lineHeight),
                             Qt::AlignRight | Qt::AlignVCenter, QString::number(number + 1));
        }
        top += height;
        block = block.next();
        ++number;
    }
}

// ---- CodeEditor -------------------------------------------------------------

CodeEditor::CodeEditor(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
    setFlag(ItemAcceptsInputMethod, true);
    setFlag(ItemIsFocusScope, false);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton | Qt::MiddleButton);
    setActiveFocusOnTab(true);
    setCursor(Qt::IBeamCursor);
    setOpaquePainting(true);

    m_widget = new CodeWidget;
    // Alive and laid out like a visible widget, but never mapped to the screen.
    m_widget->setAttribute(Qt::WA_DontShowOnScreen, true);
    // It must not count as an open window, or the application would keep
    // running after its real window is closed.
    m_widget->setAttribute(Qt::WA_QuitOnClose, false);
    m_widget->resize(640, 480);
    m_widget->show();

    connect(m_widget, &CodeWidget::repaintNeeded, this, &CodeEditor::requestRepaint);
    connect(m_widget, &QPlainTextEdit::cursorPositionChanged, this, &CodeEditor::cursorChanged);
    connect(m_widget, &QPlainTextEdit::selectionChanged, this, &CodeEditor::cursorChanged);
    connect(m_widget, &QPlainTextEdit::selectionChanged, this, &CodeEditor::requestRepaint);
    connect(m_widget, &QPlainTextEdit::textChanged, this, &CodeEditor::lengthChanged);
    connect(m_widget, &QPlainTextEdit::undoAvailable, this, [this](bool available) { m_canUndo = available; Q_EMIT undoStateChanged(); });
    connect(m_widget, &QPlainTextEdit::redoAvailable, this, [this](bool available) { m_canRedo = available; Q_EMIT undoStateChanged(); });
    for (QScrollBar *bar : {m_widget->verticalScrollBar(), m_widget->horizontalScrollBar()}) {
        connect(bar, &QScrollBar::valueChanged, this, [this] { Q_EMIT scrollChanged(); requestRepaint(); });
        connect(bar, &QScrollBar::rangeChanged, this, [this] { Q_EMIT scrollChanged(); requestRepaint(); });
    }
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &CodeEditor::cursorChanged);
    connect(this, &CodeEditor::colorsChanged, this, &CodeEditor::applyColors);
}

CodeEditor::~CodeEditor()
{
    delete m_widget;
}

QTextDocument *CodeEditor::textDocument() const
{
    return m_widget->document();
}

void CodeEditor::setMatches(const std::vector<std::pair<int, int>> *matches)
{
    m_widget->setMatches(matches);
    requestRepaint();
}

void CodeEditor::refreshMatches()
{
    m_widget->refreshDecorations();
    requestRepaint();
}

void CodeEditor::requestRepaint()
{
    polish();
    update();
}

// Widgets may only paint on the GUI thread, and paint() can run on the
// scene graph's render thread: render here, hand over the finished image.
void CodeEditor::updatePolish()
{
    const qreal ratio = window() ? window()->effectiveDevicePixelRatio() : 1.0;
    const QSize pixels = (QSizeF(m_widget->size()) * ratio).toSize();
    if (pixels.isEmpty())
        return;
    if (m_image.size() != pixels) {
        m_image = QImage(pixels, QImage::Format_RGB32);
    }
    m_image.setDevicePixelRatio(ratio);
    m_image.fill(m_background.isValid() ? m_background : QColor(Qt::black));
    QPainter painter(&m_image);
    m_widget->render(&painter, QPoint(), QRegion(), QWidget::DrawWindowBackground | QWidget::DrawChildren);
}

void CodeEditor::paint(QPainter *painter)
{
    if (!m_image.isNull())
        painter->drawImage(QRectF(0, 0, width(), height()), m_image);
}

void CodeEditor::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    const QSize size = newGeometry.size().toSize();
    if (size.width() > 0 && size.height() > 0 && size != m_widget->size()) {
        m_widget->resize(size);
        Q_EMIT scrollChanged();
        requestRepaint();
    }
}

void CodeEditor::applyColors()
{
    m_widget->setColors({m_text, m_background, m_gutter, m_gutterText, m_gutterCurrent, m_currentLine, m_selection, m_match, m_divider});
    setFillColor(m_background);
    requestRepaint();
}

// ---- properties -------------------------------------------------------------

int CodeEditor::cursorPosition() const { return m_widget->textCursor().position(); }

void CodeEditor::setCursorPosition(int position)
{
    QTextCursor cursor = m_widget->textCursor();
    cursor.setPosition(qBound(0, position, m_widget->document()->characterCount() - 1));
    m_widget->setTextCursor(cursor);
}

int CodeEditor::selectionStart() const { return m_widget->textCursor().selectionStart(); }
int CodeEditor::selectionEnd() const { return m_widget->textCursor().selectionEnd(); }

QString CodeEditor::selectedText() const
{
    QString text = m_widget->textCursor().selectedText();
    text.replace(QChar(QChar::ParagraphSeparator), QLatin1Char('\n'));
    return text;
}

int CodeEditor::length() const { return m_widget->document()->characterCount() - 1; }
bool CodeEditor::canUndo() const { return m_canUndo; }
bool CodeEditor::canRedo() const { return m_canRedo; }
bool CodeEditor::canPaste() const { return m_widget->canPaste(); }
bool CodeEditor::readOnly() const { return m_widget->isReadOnly(); }

void CodeEditor::setReadOnly(bool readOnly)
{
    if (readOnly == m_widget->isReadOnly())
        return;
    m_widget->setReadOnly(readOnly);
    // A read-only QPlainTextEdit hides its cursor; keep it for keyboard users.
    if (readOnly)
        m_widget->setTextInteractionFlags(m_widget->textInteractionFlags() | Qt::TextSelectableByKeyboard);
    Q_EMIT readOnlyChanged();
}

QString CodeEditor::fontFamily() const { return m_widget->font().family(); }

void CodeEditor::setFontFamily(const QString &family)
{
    QFont font = m_widget->font();
    if (font.family() == family)
        return;
    font.setFamily(family);
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    // Source code is shown character by character: no "!=" → "≠" ligatures.
    font.setFeature(QFont::Tag("liga"), 0);
    font.setFeature(QFont::Tag("calt"), 0);
    m_widget->setFont(font);
    applyTabWidth();
    Q_EMIT fontChanged();
    requestRepaint();
}

int CodeEditor::fontPointSize() const { return m_widget->font().pointSize(); }

void CodeEditor::setFontPointSize(int size)
{
    QFont font = m_widget->font();
    size = qBound(4, size, 96);
    if (font.pointSize() == size)
        return;
    font.setPointSize(size);
    m_widget->setFont(font);
    applyTabWidth();
    Q_EMIT fontChanged();
    requestRepaint();
}

bool CodeEditor::wordWrap() const { return m_widget->lineWrapMode() == QPlainTextEdit::WidgetWidth; }

void CodeEditor::setWordWrap(bool wrap)
{
    if (wrap == wordWrap())
        return;
    m_widget->setLineWrapMode(wrap ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
    m_widget->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    Q_EMIT wordWrapChanged();
    Q_EMIT scrollChanged();
    requestRepaint();
}

void CodeEditor::setTabWidth(int width)
{
    width = qBound(1, width, 16);
    if (width == m_tabWidth)
        return;
    m_tabWidth = width;
    applyTabWidth();
    Q_EMIT tabWidthChanged();
    requestRepaint();
}

void CodeEditor::applyTabWidth()
{
    m_widget->setTabStopDistance(QFontMetricsF(m_widget->font()).horizontalAdvance(QLatin1Char(' ')) * m_tabWidth);
}

void CodeEditor::setLineNumbers(bool visible)
{
    if (visible == m_lineNumbers)
        return;
    m_lineNumbers = visible;
    m_widget->setLineNumbers(visible);
    Q_EMIT lineNumbersChanged();
    requestRepaint();
}

void CodeEditor::setHighlightCurrentLine(bool on)
{
    if (on == m_highlightLine)
        return;
    m_highlightLine = on;
    m_widget->setHighlightCurrentLine(on);
    Q_EMIT highlightCurrentLineChanged();
    requestRepaint();
}

qreal CodeEditor::verticalPosition() const
{
    const QScrollBar *bar = m_widget->verticalScrollBar();
    const qreal total = bar->maximum() - bar->minimum() + bar->pageStep();
    return total > 0 ? (bar->value() - bar->minimum()) / total : 0;
}

qreal CodeEditor::verticalSize() const
{
    const QScrollBar *bar = m_widget->verticalScrollBar();
    const qreal total = bar->maximum() - bar->minimum() + bar->pageStep();
    return total > 0 ? qMin(1.0, bar->pageStep() / total) : 1;
}

qreal CodeEditor::horizontalPosition() const
{
    const QScrollBar *bar = m_widget->horizontalScrollBar();
    const qreal total = bar->maximum() - bar->minimum() + bar->pageStep();
    return total > 0 ? (bar->value() - bar->minimum()) / total : 0;
}

qreal CodeEditor::horizontalSize() const
{
    const QScrollBar *bar = m_widget->horizontalScrollBar();
    const qreal total = bar->maximum() - bar->minimum() + bar->pageStep();
    return total > 0 ? qMin(1.0, bar->pageStep() / total) : 1;
}

void CodeEditor::scrollVertically(qreal position)
{
    QScrollBar *bar = m_widget->verticalScrollBar();
    const qreal total = bar->maximum() - bar->minimum() + bar->pageStep();
    bar->setValue(bar->minimum() + qRound(position * total));
}

void CodeEditor::scrollHorizontally(qreal position)
{
    QScrollBar *bar = m_widget->horizontalScrollBar();
    const qreal total = bar->maximum() - bar->minimum() + bar->pageStep();
    bar->setValue(bar->minimum() + qRound(position * total));
}

// ---- editing ----------------------------------------------------------------

void CodeEditor::select(int start, int end)
{
    const int limit = m_widget->document()->characterCount() - 1;
    QTextCursor cursor = m_widget->textCursor();
    cursor.setPosition(qBound(0, start, limit));
    cursor.setPosition(qBound(0, end, limit), QTextCursor::KeepAnchor);
    m_widget->setTextCursor(cursor);
}

void CodeEditor::insert(int position, const QString &text)
{
    QTextCursor cursor = m_widget->textCursor();
    cursor.setPosition(qBound(0, position, m_widget->document()->characterCount() - 1));
    cursor.insertText(text);
    m_widget->setTextCursor(cursor);
}

void CodeEditor::remove(int start, int end)
{
    const int limit = m_widget->document()->characterCount() - 1;
    QTextCursor cursor = m_widget->textCursor();
    cursor.setPosition(qBound(0, start, limit));
    cursor.setPosition(qBound(0, end, limit), QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
    m_widget->setTextCursor(cursor);
}

void CodeEditor::undo() { m_widget->undo(); }
void CodeEditor::redo() { m_widget->redo(); }
void CodeEditor::cut() { m_widget->cut(); }
void CodeEditor::copy() { m_widget->copy(); }
void CodeEditor::paste() { m_widget->paste(); }
void CodeEditor::selectAll() { m_widget->selectAll(); }
void CodeEditor::centerCursor() { m_widget->centerCursor(); requestRepaint(); }

// ---- input ------------------------------------------------------------------

bool CodeEditor::event(QEvent *event)
{
    if (event->type() == QEvent::ShortcutOverride) {
        // Editing keys (Ctrl+Z, Ctrl+C, …) belong to the text, not to window shortcuts.
        QCoreApplication::sendEvent(m_widget, event);
        return event->isAccepted();
    }
    return QQuickPaintedItem::event(event);
}

void CodeEditor::keyPressEvent(QKeyEvent *event)
{
    QCoreApplication::sendEvent(m_widget, event);
    requestRepaint();
}

void CodeEditor::keyReleaseEvent(QKeyEvent *event)
{
    QCoreApplication::sendEvent(m_widget, event);
}

void CodeEditor::forwardMouse(QMouseEvent *event, QEvent::Type type)
{
    // Coordinates of the item are those of the widget; its viewport starts
    // after the line-number gutter.
    const QPoint inViewport = m_widget->viewport()->mapFrom(m_widget, event->position().toPoint());
    QMouseEvent copy(type, QPointF(inViewport), event->globalPosition(), event->button(), event->buttons(), event->modifiers());
    QCoreApplication::sendEvent(m_widget->viewport(), &copy);
    event->accept();
    requestRepaint();
}

void CodeEditor::mousePressEvent(QMouseEvent *event)
{
    forceActiveFocus(Qt::MouseFocusReason);
    if (event->button() == Qt::RightButton) {
        Q_EMIT contextMenuRequested(event->position().x(), event->position().y());
        event->accept();
        return;
    }
    forwardMouse(event, QEvent::MouseButtonPress);
}

void CodeEditor::mouseMoveEvent(QMouseEvent *event) { forwardMouse(event, QEvent::MouseMove); }

void CodeEditor::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::RightButton) {
        event->accept();
        return;
    }
    forwardMouse(event, QEvent::MouseButtonRelease);
}

void CodeEditor::mouseDoubleClickEvent(QMouseEvent *event) { forwardMouse(event, QEvent::MouseButtonDblClick); }

void CodeEditor::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        event->ignore();        // zoom is a window shortcut
        return;
    }
    QWheelEvent copy(event->position(), event->globalPosition(), event->pixelDelta(), event->angleDelta(), event->buttons(),
                     event->modifiers(), event->phase(), event->inverted());
    QCoreApplication::sendEvent(m_widget->viewport(), &copy);
    event->accept();
    requestRepaint();
}

void CodeEditor::focusInEvent(QFocusEvent *event)
{
    QQuickPaintedItem::focusInEvent(event);
    // The widget never has window-system focus; tell it by hand so that it
    // shows and blinks the cursor.
    QFocusEvent focus(QEvent::FocusIn, Qt::OtherFocusReason);
    QCoreApplication::sendEvent(m_widget, &focus);
    requestRepaint();
}

void CodeEditor::focusOutEvent(QFocusEvent *event)
{
    QQuickPaintedItem::focusOutEvent(event);
    QFocusEvent focus(QEvent::FocusOut, Qt::OtherFocusReason);
    QCoreApplication::sendEvent(m_widget, &focus);
    requestRepaint();
}

void CodeEditor::inputMethodEvent(QInputMethodEvent *event)
{
    QCoreApplication::sendEvent(m_widget, event);
    requestRepaint();
}

QVariant CodeEditor::inputMethodQuery(Qt::InputMethodQuery query) const
{
    if (query == Qt::ImEnabled)
        return !m_widget->isReadOnly();
    QVariant value = m_widget->inputMethodQuery(query);
    if (query == Qt::ImCursorRectangle || query == Qt::ImAnchorRectangle) {
        // Reported relative to the viewport; the item also contains the gutter.
        const QPoint offset = m_widget->viewport()->mapTo(m_widget, QPoint(0, 0));
        value = value.toRectF().translated(offset);
    }
    return value;
}
