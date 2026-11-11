// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QColor>
#include <QImage>
#include <QPlainTextEdit>
#include <QQmlEngine>
#include <QQuickPaintedItem>
#include <utility>
#include <vector>

class CodeEditor;

// The text area. Measurement showed that Qt Quick's TextEdit cannot edit
// megabyte-sized files (README, "Large files"), so the editing engine is a
// QPlainTextEdit: it lays out only the visible lines. The widget is never
// put on screen; CodeEditor paints it into the QML scene and passes input
// to it.
class CodeWidget : public QPlainTextEdit
{
    Q_OBJECT
public:
    struct Colors
    {
        QColor text, background, gutter, gutterText, gutterCurrent, currentLine, selection, match, divider;
    };

    explicit CodeWidget(QWidget *parent = nullptr);

    void setColors(const Colors &colors);
    void setLineNumbers(bool visible);
    void setHighlightCurrentLine(bool on);
    void setMatches(const std::vector<std::pair<int, int>> *matches);
    void refreshDecorations();
    int gutterWidth() const;
    void paintGutter(QPaintEvent *event);

Q_SIGNALS:
    void repaintNeeded();

protected:
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void updateGutterGeometry();

    QWidget *m_gutter = nullptr;
    Colors m_colors;
    bool m_lineNumbers = true;
    bool m_highlightCurrentLine = true;
    const std::vector<std::pair<int, int>> *m_matches = nullptr;
};

// QML item showing a CodeWidget. Its API follows TextEdit where one exists
// (cursorPosition, selectionStart, select(), insert(), undo(), …).
class CodeEditor : public QQuickPaintedItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int cursorPosition READ cursorPosition WRITE setCursorPosition NOTIFY cursorChanged)
    Q_PROPERTY(int selectionStart READ selectionStart NOTIFY cursorChanged)
    Q_PROPERTY(int selectionEnd READ selectionEnd NOTIFY cursorChanged)
    Q_PROPERTY(QString selectedText READ selectedText NOTIFY cursorChanged)
    Q_PROPERTY(int length READ length NOTIFY lengthChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY undoStateChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY undoStateChanged)
    Q_PROPERTY(bool canPaste READ canPaste NOTIFY cursorChanged)
    Q_PROPERTY(bool readOnly READ readOnly WRITE setReadOnly NOTIFY readOnlyChanged)
    Q_PROPERTY(QString fontFamily READ fontFamily WRITE setFontFamily NOTIFY fontChanged)
    Q_PROPERTY(int fontPointSize READ fontPointSize WRITE setFontPointSize NOTIFY fontChanged)
    Q_PROPERTY(bool wordWrap READ wordWrap WRITE setWordWrap NOTIFY wordWrapChanged)
    Q_PROPERTY(int tabWidth READ tabWidth WRITE setTabWidth NOTIFY tabWidthChanged)
    Q_PROPERTY(bool lineNumbers READ lineNumbers WRITE setLineNumbers NOTIFY lineNumbersChanged)
    Q_PROPERTY(bool highlightCurrentLine READ highlightCurrentLine WRITE setHighlightCurrentLine NOTIFY highlightCurrentLineChanged)
    Q_PROPERTY(QColor textColor MEMBER m_text NOTIFY colorsChanged)
    Q_PROPERTY(QColor backgroundColor MEMBER m_background NOTIFY colorsChanged)
    Q_PROPERTY(QColor gutterColor MEMBER m_gutter NOTIFY colorsChanged)
    Q_PROPERTY(QColor gutterTextColor MEMBER m_gutterText NOTIFY colorsChanged)
    Q_PROPERTY(QColor gutterCurrentColor MEMBER m_gutterCurrent NOTIFY colorsChanged)
    Q_PROPERTY(QColor currentLineColor MEMBER m_currentLine NOTIFY colorsChanged)
    Q_PROPERTY(QColor selectionColor MEMBER m_selection NOTIFY colorsChanged)
    Q_PROPERTY(QColor matchColor MEMBER m_match NOTIFY colorsChanged)
    Q_PROPERTY(QColor dividerColor MEMBER m_divider NOTIFY colorsChanged)
    // Scroll state in ScrollBar terms: position and size as fractions (0–1).
    Q_PROPERTY(qreal verticalPosition READ verticalPosition NOTIFY scrollChanged)
    Q_PROPERTY(qreal verticalSize READ verticalSize NOTIFY scrollChanged)
    Q_PROPERTY(qreal horizontalPosition READ horizontalPosition NOTIFY scrollChanged)
    Q_PROPERTY(qreal horizontalSize READ horizontalSize NOTIFY scrollChanged)

public:
    explicit CodeEditor(QQuickItem *parent = nullptr);
    ~CodeEditor() override;

    void paint(QPainter *painter) override;
    void requestRepaint();
    QTextDocument *textDocument() const;
    void setMatches(const std::vector<std::pair<int, int>> *matches);
    void refreshMatches();

    int cursorPosition() const;
    void setCursorPosition(int position);
    int selectionStart() const;
    int selectionEnd() const;
    QString selectedText() const;
    int length() const;
    bool canUndo() const;
    bool canRedo() const;
    bool canPaste() const;
    bool readOnly() const;
    void setReadOnly(bool readOnly);
    QString fontFamily() const;
    void setFontFamily(const QString &family);
    int fontPointSize() const;
    void setFontPointSize(int size);
    bool wordWrap() const;
    void setWordWrap(bool wrap);
    int tabWidth() const { return m_tabWidth; }
    void setTabWidth(int width);
    bool lineNumbers() const { return m_lineNumbers; }
    void setLineNumbers(bool visible);
    bool highlightCurrentLine() const { return m_highlightLine; }
    void setHighlightCurrentLine(bool on);
    qreal verticalPosition() const;
    qreal verticalSize() const;
    qreal horizontalPosition() const;
    qreal horizontalSize() const;

    Q_INVOKABLE void select(int start, int end);
    Q_INVOKABLE void insert(int position, const QString &text);
    Q_INVOKABLE void remove(int start, int end);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void cut();
    Q_INVOKABLE void copy();
    Q_INVOKABLE void paste();
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE void centerCursor();
    Q_INVOKABLE void scrollVertically(qreal position);
    Q_INVOKABLE void scrollHorizontally(qreal position);

Q_SIGNALS:
    void cursorChanged();
    void lengthChanged();
    void undoStateChanged();
    void readOnlyChanged();
    void fontChanged();
    void wordWrapChanged();
    void tabWidthChanged();
    void lineNumbersChanged();
    void highlightCurrentLineChanged();
    void colorsChanged();
    void scrollChanged();
    void contextMenuRequested(qreal x, qreal y);

protected:
    void updatePolish() override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    bool event(QEvent *event) override;

private:
    void forwardMouse(QMouseEvent *event, QEvent::Type type);
    void applyColors();
    void applyTabWidth();

    CodeWidget *m_widget = nullptr;
    QImage m_image;
    int m_tabWidth = 4;
    bool m_lineNumbers = true;
    bool m_highlightLine = true;
    bool m_canUndo = false;
    bool m_canRedo = false;
    QColor m_text, m_background, m_gutter, m_gutterText, m_gutterCurrent, m_currentLine, m_selection, m_match, m_divider;
};
