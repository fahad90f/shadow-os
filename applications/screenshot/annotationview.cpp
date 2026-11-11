// SPDX-License-Identifier: GPL-3.0-or-later
#include "annotationview.h"

#include <QCursor>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <cmath>

AnnotationView::AnnotationView(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
    setAcceptedMouseButtons(Qt::LeftButton);
    setAntialiasing(true);
}

void AnnotationView::setDocument(Document *document)
{
    if (m_document == document)
        return;
    if (m_document)
        disconnect(m_document, nullptr, this, nullptr);
    m_document = document;
    if (m_document) {
        connect(m_document, &Document::changed, this, [this] {
            m_dragging = false;
            Q_EMIT zoomChanged();
            update();
        });
    }
    Q_EMIT documentChanged();
    Q_EMIT zoomChanged();
    update();
}

void AnnotationView::setTool(const QString &tool)
{
    if (m_tool == tool)
        return;
    m_tool = tool;
    m_dragging = false;
    setCursor(tool.isEmpty() ? QCursor(Qt::ArrowCursor) : QCursor(tool == QLatin1String("text") ? Qt::IBeamCursor : Qt::CrossCursor));
    Q_EMIT toolChanged();
    update();
}

void AnnotationView::setColor(const QColor &color)
{
    if (m_color == color)
        return;
    m_color = color;
    Q_EMIT colorChanged();
    update();
}

QRectF AnnotationView::imageRect() const
{
    if (!m_document || m_document->isEmpty() || width() <= 0 || height() <= 0)
        return {};
    const QSizeF image(m_document->width(), m_document->height());
    // Never enlarge beyond 1:1 — small captures stay crisp.
    const qreal scale = qMin(1.0, qMin(width() / image.width(), height() / image.height()));
    const QSizeF size = image * scale;
    return QRectF(QPointF(std::floor((width() - size.width()) / 2), std::floor((height() - size.height()) / 2)), size);
}

qreal AnnotationView::zoom() const
{
    const QRectF r = imageRect();
    return r.isEmpty() ? 1.0 : r.width() / m_document->width();
}

QPointF AnnotationView::toImage(const QPointF &item) const
{
    const QRectF r = imageRect();
    if (r.isEmpty())
        return {};
    const qreal s = m_document->width() / r.width();
    return QPointF(qBound(0.0, (item.x() - r.x()) * s, qreal(m_document->width())),
                   qBound(0.0, (item.y() - r.y()) * s, qreal(m_document->height())));
}

void AnnotationView::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    Q_EMIT zoomChanged();
    update();
}

void AnnotationView::paint(QPainter *painter)
{
    const QRectF r = imageRect();
    if (r.isEmpty())
        return;
    painter->setRenderHint(QPainter::SmoothPixmapTransform);
    const QImage &image = m_document->image();
    painter->drawImage(r, image, QRectF(image.rect()));
    painter->setPen(QPen(m_frameColor, 1));
    painter->setBrush(Qt::NoBrush);
    painter->drawRect(r.adjusted(-0.5, -0.5, 0.5, 0.5));

    if (!m_dragging)
        return;
    const qreal s = r.width() / image.width();
    const QRectF drag = QRectF(m_from, m_to).normalized();
    painter->save();
    painter->translate(r.topLeft());
    painter->scale(s, s);
    if (m_tool == QLatin1String("arrow")) {
        Document::paintArrow(painter, m_from, m_to, m_color, m_document->strokeWidth());
    } else if (m_tool == QLatin1String("rect")) {
        Document::paintRectangle(painter, drag, m_color, m_document->strokeWidth());
    } else if (m_tool == QLatin1String("blur")) {
        const QRect px = drag.toAlignedRect().intersected(image.rect());
        if (px.width() >= 4 && px.height() >= 4)
            painter->drawImage(px.topLeft(), Document::pixelated(image, px));
    } else if (m_tool == QLatin1String("crop")) {
        // Everything outside the selection is dimmed.
        QPainterPath outside;
        outside.addRect(QRectF(image.rect()));
        QPainterPath inside;
        inside.addRect(drag);
        painter->fillPath(outside.subtracted(inside), QColor(0, 0, 0, 140));
        QPen pen(Qt::white, 1.5 / s, Qt::DashLine);
        painter->setPen(pen);
        painter->drawRect(drag);
    }
    painter->restore();
}

void AnnotationView::mousePressEvent(QMouseEvent *event)
{
    const QRectF r = imageRect();
    if (m_tool.isEmpty() || r.isEmpty() || !r.contains(event->position())) {
        event->ignore();
        return;
    }
    const QPointF p = toImage(event->position());
    if (m_tool == QLatin1String("text")) {
        Q_EMIT textRequested(p.x(), p.y(), event->position().x(), event->position().y());
        return;
    }
    m_dragging = true;
    m_from = m_to = p;
    update();
}

void AnnotationView::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging)
        return;
    m_to = toImage(event->position());
    update();
}

void AnnotationView::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_dragging)
        return;
    m_to = toImage(event->position());
    commitDrag();
}

void AnnotationView::commitDrag()
{
    m_dragging = false;
    if (!m_document)
        return;
    const QRectF drag = QRectF(m_from, m_to).normalized();
    if (m_tool == QLatin1String("arrow"))
        m_document->addArrow(m_from, m_to, m_color);
    else if (m_tool == QLatin1String("rect"))
        m_document->addRectangle(drag, m_color);
    else if (m_tool == QLatin1String("blur"))
        m_document->pixelate(drag);
    else if (m_tool == QLatin1String("crop"))
        m_document->crop(drag);
    update();
}

void AnnotationView::placeText(qreal imageX, qreal imageY, const QString &text)
{
    if (m_document)
        m_document->addText(QPointF(imageX, imageY), text, m_color);
}
