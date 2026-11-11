// SPDX-License-Identifier: GPL-3.0-or-later
#include "document.h"

#include <QFont>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>

#include <cmath>

namespace {
constexpr int kMaxUndo = 30;
}

Document::Document(QObject *parent)
    : QObject(parent)
{
}

void Document::setImage(const QImage &image)
{
    m_image = image;
    m_undo.clear();
    m_redo.clear();
    Q_EMIT changed();
}

qreal Document::strokeWidth() const
{
    return qMax(3.0, qMax(m_image.width(), m_image.height()) / 400.0);
}

int Document::textPixelSize() const
{
    return qMax(18, qRound(qMax(m_image.width(), m_image.height()) / 60.0));
}

void Document::commit(const QImage &next)
{
    m_undo.append(m_image);
    while (m_undo.size() > kMaxUndo)
        m_undo.removeFirst();
    m_redo.clear();
    m_image = next;
    Q_EMIT changed();
}

void Document::undo()
{
    if (m_undo.isEmpty())
        return;
    m_redo.append(m_image);
    m_image = m_undo.takeLast();
    Q_EMIT changed();
}

void Document::redo()
{
    if (m_redo.isEmpty())
        return;
    m_undo.append(m_image);
    m_image = m_redo.takeLast();
    Q_EMIT changed();
}

void Document::crop(const QRectF &rect)
{
    const QRect r = rect.normalized().toAlignedRect().intersected(m_image.rect());
    if (r.width() < 2 || r.height() < 2 || r == m_image.rect())
        return;
    QImage next = m_image.copy(r);
    next.setDevicePixelRatio(m_image.devicePixelRatio());
    commit(next);
}

void Document::paintArrow(QPainter *p, const QPointF &from, const QPointF &to, const QColor &color, qreal width)
{
    const QLineF line(from, to);
    if (line.length() < 1.0)
        return;
    const qreal head = qMin(line.length() * 0.6, width * 5.0);
    const qreal angle = std::atan2(to.y() - from.y(), to.x() - from.x());
    const QPointF back(to.x() - head * std::cos(angle), to.y() - head * std::sin(angle));
    const QPointF normal(-std::sin(angle), std::cos(angle));
    QPolygonF tip;
    tip << to << back + normal * head * 0.5 << back - normal * head * 0.5;

    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p->drawLine(from, back);
    p->setPen(QPen(color, 1.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p->setBrush(color);
    p->drawPolygon(tip);
    p->restore();
}

void Document::paintRectangle(QPainter *p, const QRectF &rect, const QColor &color, qreal width)
{
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setPen(QPen(color, width, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
    p->setBrush(Qt::NoBrush);
    p->drawRect(rect.normalized());
    p->restore();
}

QImage Document::pixelated(const QImage &source, const QRect &rect)
{
    const QRect r = rect.intersected(source.rect());
    if (r.isEmpty())
        return {};
    // Block size follows the picture so the result is unreadable at any resolution.
    const int block = qMax(10, qMax(source.width(), source.height()) / 120);
    const QImage part = source.copy(r);
    const QSize small(qMax(1, r.width() / block), qMax(1, r.height() / block));
    return part.scaled(small, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
               .scaled(r.size(), Qt::IgnoreAspectRatio, Qt::FastTransformation);
}

void Document::addArrow(const QPointF &from, const QPointF &to, const QColor &color)
{
    if (isEmpty() || QLineF(from, to).length() < 3.0)
        return;
    QImage next = m_image;
    next.setDevicePixelRatio(1.0);
    QPainter p(&next);
    paintArrow(&p, from, to, color, strokeWidth());
    p.end();
    next.setDevicePixelRatio(m_image.devicePixelRatio());
    commit(next);
}

void Document::addRectangle(const QRectF &rect, const QColor &color)
{
    const QRectF r = rect.normalized();
    if (isEmpty() || r.width() < 3 || r.height() < 3)
        return;
    QImage next = m_image;
    next.setDevicePixelRatio(1.0);
    QPainter p(&next);
    paintRectangle(&p, r, color, strokeWidth());
    p.end();
    next.setDevicePixelRatio(m_image.devicePixelRatio());
    commit(next);
}

void Document::addText(const QPointF &topLeft, const QString &text, const QColor &color)
{
    if (isEmpty() || text.trimmed().isEmpty())
        return;
    QImage next = m_image;
    next.setDevicePixelRatio(1.0);
    QPainter p(&next);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    QFont font = QGuiApplication::font();
    font.setPixelSize(textPixelSize());
    font.setWeight(QFont::DemiBold);
    // An outline in the contrasting shade keeps the text legible on any background.
    QPainterPath path;
    const QFontMetricsF fm(font);
    qreal y = topLeft.y() + fm.ascent();
    const QStringList lines = text.split(u'\n');
    for (const QString &line : lines) {
        path.addText(QPointF(topLeft.x(), y), font, line);
        y += fm.lineSpacing();
    }
    const QColor outline = color.lightnessF() > 0.5 ? QColor(0, 0, 0, 200) : QColor(255, 255, 255, 220);
    p.setPen(QPen(outline, qMax(2.0, font.pixelSize() / 8.0), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawPath(path);
    p.end();
    next.setDevicePixelRatio(m_image.devicePixelRatio());
    commit(next);
}

void Document::pixelate(const QRectF &rect)
{
    const QRect r = rect.normalized().toAlignedRect().intersected(m_image.rect());
    if (isEmpty() || r.width() < 4 || r.height() < 4)
        return;
    QImage next = m_image;
    next.setDevicePixelRatio(1.0);
    QPainter p(&next);
    p.drawImage(r.topLeft(), pixelated(m_image, r));
    p.end();
    next.setDevicePixelRatio(m_image.devicePixelRatio());
    commit(next);
}
