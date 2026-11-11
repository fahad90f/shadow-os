// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QColor>
#include <QImage>
#include <QList>
#include <QObject>
#include <QPointF>
#include <QRectF>

class QPainter;

// The captured picture and its edit history. Every annotation is drawn into
// the image itself, so what is shown is exactly what is saved or copied.
class Document : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool empty READ isEmpty NOTIFY changed)
    Q_PROPERTY(int width READ width NOTIFY changed)
    Q_PROPERTY(int height READ height NOTIFY changed)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY changed)
    Q_PROPERTY(int editCount READ editCount NOTIFY changed)
public:
    explicit Document(QObject *parent = nullptr);

    const QImage &image() const { return m_image; }
    bool isEmpty() const { return m_image.isNull(); }
    int width() const { return m_image.width(); }
    int height() const { return m_image.height(); }
    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    int editCount() const { return int(m_undo.size()); }

    // A new capture: replaces the image and clears the history.
    void setImage(const QImage &image);

    // Stroke width / text height that suit the size of the picture.
    qreal strokeWidth() const;
    int textPixelSize() const;

    // Edits, in image pixel coordinates. Each one is a single undo step.
    void crop(const QRectF &rect);
    void addArrow(const QPointF &from, const QPointF &to, const QColor &color);
    void addRectangle(const QRectF &rect, const QColor &color);
    void addText(const QPointF &topLeft, const QString &text, const QColor &color);
    void pixelate(const QRectF &rect);

    // The drawing primitives, shared with the live preview.
    static void paintArrow(QPainter *p, const QPointF &from, const QPointF &to, const QColor &color, qreal width);
    static void paintRectangle(QPainter *p, const QRectF &rect, const QColor &color, qreal width);
    static QImage pixelated(const QImage &source, const QRect &rect);

public Q_SLOTS:
    void undo();
    void redo();

Q_SIGNALS:
    void changed();

private:
    void commit(const QImage &next);

    QImage m_image;
    QList<QImage> m_undo;
    QList<QImage> m_redo;
};
