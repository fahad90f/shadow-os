// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "document.h"

#include <QColor>
#include <QPointer>
#include <QQmlEngine>
#include <QQuickPaintedItem>

// Shows the Document's picture fitted into the item and lets the user draw
// on it with the selected tool. A drag is previewed with the same painting
// code that commits it, so the preview is exact.
class AnnotationView : public QQuickPaintedItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(Document *document READ document WRITE setDocument NOTIFY documentChanged)
    // "" (view only) | "crop" | "arrow" | "rect" | "text" | "blur"
    Q_PROPERTY(QString tool READ tool WRITE setTool NOTIFY toolChanged)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)
    Q_PROPERTY(QColor frameColor MEMBER m_frameColor NOTIFY frameColorChanged)
    Q_PROPERTY(qreal zoom READ zoom NOTIFY zoomChanged)
public:
    explicit AnnotationView(QQuickItem *parent = nullptr);

    Document *document() const { return m_document; }
    void setDocument(Document *document);
    QString tool() const { return m_tool; }
    void setTool(const QString &tool);
    QColor color() const { return m_color; }
    void setColor(const QColor &color);
    qreal zoom() const;

    void paint(QPainter *painter) override;

    // Text tool: the UI asks for the text after textRequested() and hands it back.
    Q_INVOKABLE void placeText(qreal imageX, qreal imageY, const QString &text);

Q_SIGNALS:
    void documentChanged();
    void toolChanged();
    void colorChanged();
    void frameColorChanged();
    void zoomChanged();
    // The user clicked with the text tool: image position and item position.
    void textRequested(qreal imageX, qreal imageY, qreal itemX, qreal itemY);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    QRectF imageRect() const;               // where the picture is drawn, item coordinates
    QPointF toImage(const QPointF &item) const;
    void commitDrag();

    QPointer<Document> m_document;
    QString m_tool;
    QColor m_color = Qt::red;
    QColor m_frameColor = Qt::gray;
    bool m_dragging = false;
    QPointF m_from;     // image coordinates
    QPointF m_to;
};
