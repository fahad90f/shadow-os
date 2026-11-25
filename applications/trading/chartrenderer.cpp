// SPDX-License-Identifier: GPL-3.0-or-later
#include "chartrenderer.h"

#include "skills/indicators.h"
#include "tradingengine.h"

#include <QBuffer>
#include <QDateTime>
#include <QImage>
#include <QPainter>
#include <QCursor>
#include <QHoverEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainterPath>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace ChartRenderer {

namespace {

// A price with as many decimals as its size needs: 92641.32, 1.1587, 0.004213.
QString priceText(double v)
{
    const double a = std::fabs(v);
    const int decimals = a >= 1000 ? 2 : a >= 100 ? 2 : a >= 1 ? 4 : 6;
    return QString::number(v, 'f', decimals);
}

// A round step that gives about 'lines' grid lines over 'range'.
double niceStep(double range, int lines)
{
    if (!(range > 0) || lines < 1)
        return 1;
    const double raw = range / lines;
    const double magnitude = std::pow(10.0, std::floor(std::log10(raw)));
    const double fraction = raw / magnitude;
    return (fraction < 1.5 ? 1 : fraction < 3.5 ? 2 : fraction < 7.5 ? 5 : 10) * magnitude;
}

} // namespace

Geometry paint(QPainter *painter, const QRectF &rect, const Trading::Candles &candles, const View &view,
               const Levels &levels, const Palette &palette, const QFont &font)
{
    Geometry geometry;
    painter->save();
    painter->fillRect(rect, palette.background);
    const int total = int(candles.size());
    if (total < 2) {
        painter->restore();
        return geometry;
    }
    painter->setFont(font);
    const QFontMetricsF metrics(font);

    // Layout: prices on the right, times at the bottom, volume in the lowest
    // fifth of the plot.
    const double axisWidth = metrics.horizontalAdvance(priceText(candles.back().close)) + 14;
    const double axisHeight = metrics.height() + 6;
    const QRectF plot(rect.left() + 6, rect.top() + 6, rect.width() - axisWidth - 12, rect.height() - axisHeight - 12);
    if (plot.width() < 40 || plot.height() < 40) {
        painter->restore();
        return geometry;
    }
    const double volumeHeight = plot.height() * 0.18;
    const QRectF priceArea(plot.left(), plot.top(), plot.width(), plot.height() - volumeHeight - 6);

    // The window: 'count' slots across the plot; the newest candle sits
    // 'offset' slots (plus a little room) left of the right edge.
    const double count = std::clamp(view.count, 10.0, 5000.0);
    const double slot = priceArea.width() / count;
    const double room = std::min(3.0, count * 0.04);
    const double rightIndex = (total - 1) - view.offset + room;   // index at the right edge
    const double firstIndex = rightIndex - count + 1;             // index whose slot starts at the left edge
    const int first = std::clamp(int(std::floor(firstIndex)), 0, total - 1);
    const int last = std::clamp(int(std::ceil(rightIndex)), 0, total - 1);
    const auto xOf = [&](double index) { return priceArea.left() + (index - firstIndex + 0.5) * slot; };
    geometry.plot = plot;
    geometry.slot = slot;
    geometry.firstIndex = firstIndex;
    if (first > last || xOf(last) < priceArea.left() - slot || xOf(first) > priceArea.right() + slot) {
        // Moved entirely off the series (cannot happen through the item, which limits the offset).
        painter->restore();
        return geometry;
    }
    const double body = std::max(1.0, slot * 0.62);

    // Overlays are computed over the whole series so their first visible
    // values are already settled.
    const std::vector<double> closes = Trading::Indicators::closes(candles);
    const std::vector<double> fast = Trading::Indicators::ema(closes, 20);
    const std::vector<double> slow = Trading::Indicators::ema(closes, 50);
    const Trading::Indicators::Bands bands = Trading::Indicators::bollinger(closes, 20, 2.0);

    // The price scale fits what is in view.
    double low = candles[size_t(first)].low, high = candles[size_t(first)].high, maxVolume = 0;
    for (int i = first; i <= last; ++i) {
        const Trading::Candle &c = candles[size_t(i)];
        low = std::min(low, c.low);
        high = std::max(high, c.high);
        maxVolume = std::max(maxVolume, c.volume);
        for (const double v : {bands.upper[size_t(i)], bands.lower[size_t(i)]}) {
            if (std::isfinite(v)) {
                low = std::min(low, v);
                high = std::max(high, v);
            }
        }
    }
    // Trade levels stay in view when they are near the visible prices.
    for (const double level : {levels.entry, levels.stop, levels.target}) {
        if (level > 0 && level > low - (high - low) && level < high + (high - low)) {
            low = std::min(low, level);
            high = std::max(high, level);
        }
    }
    double span = high - low;
    if (!(span > 0))
        span = std::max(high * 0.001, 1e-9);
    low -= span * 0.04;
    high += span * 0.04;
    span = high - low;
    const auto yOf = [&](double price) { return priceArea.bottom() - (price - low) / span * priceArea.height(); };

    // Grid and price labels.
    painter->setPen(QPen(palette.grid, 1));
    const double step = niceStep(span, 6);
    for (double level = std::ceil(low / step) * step; level < high; level += step) {
        const double y = std::round(yOf(level)) + 0.5;
        painter->drawLine(QPointF(priceArea.left(), y), QPointF(priceArea.right(), y));
        painter->setPen(palette.text);
        painter->drawText(QRectF(priceArea.right() + 6, y - metrics.height() / 2, axisWidth, metrics.height()),
                          Qt::AlignLeft | Qt::AlignVCenter, priceText(level));
        painter->setPen(QPen(palette.grid, 1));
    }

    // Time labels, about one per 110 px, at candle positions that do not
    // move while the view is dragged (multiples of the spacing).
    const int every = std::max(1, int(std::ceil(110.0 / slot)));
    const qint64 spanMs = candles[size_t(last)].openTime - candles[size_t(first)].openTime;
    const auto timeText = [spanMs](qint64 ms, bool full) {
        const QDateTime time = QDateTime::fromMSecsSinceEpoch(ms);
        if (full)
            return time.toString(QStringLiteral("d MMM yyyy  HH:mm"));
        return spanMs > 3 * 86400000LL ? time.toString(QStringLiteral("d MMM")) : time.toString(QStringLiteral("HH:mm"));
    };
    painter->setPen(palette.text);
    painter->setClipRect(QRectF(plot.left(), plot.top(), plot.width(), rect.bottom() - plot.top()));
    QDate labelled;
    for (int i = (first / every) * every; i <= last; i += every) {
        if (i < first)
            continue;
        // The first label of each day carries the date.
        const qint64 ms = candles[size_t(i)].openTime;
        const QDate day = QDateTime::fromMSecsSinceEpoch(ms).date();
        const QString label = labelled.isValid() && day != labelled
            ? QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("d MMM")) : timeText(ms, false);
        labelled = day;
        painter->drawText(QRectF(xOf(i) - 50, plot.bottom() + 4, 100, metrics.height()), Qt::AlignCenter, label);
    }

    // Everything drawn in the plot stays in the plot (candles that are half
    // out of view at an edge).
    painter->setClipRect(plot.adjusted(0, -1, 0, 1));

    // Overlay lines.
    painter->setRenderHint(QPainter::Antialiasing, true);
    const auto line = [&](const std::vector<double> &series, const QColor &color, qreal width) {
        QPainterPath path;
        bool started = false;
        for (int i = first; i <= last; ++i) {
            const double v = series[size_t(i)];
            if (!std::isfinite(v)) {
                started = false;
                continue;
            }
            const QPointF point(xOf(i), yOf(v));
            if (started)
                path.lineTo(point);
            else
                path.moveTo(point);
            started = true;
        }
        painter->setPen(QPen(color, width));
        painter->drawPath(path);
    };
    line(bands.upper, palette.band, 1.0);
    line(bands.lower, palette.band, 1.0);
    line(slow, palette.slow, 1.4);
    line(fast, palette.fast, 1.4);
    painter->setRenderHint(QPainter::Antialiasing, false);

    // Volume and candles.
    for (int i = first; i <= last; ++i) {
        const Trading::Candle &c = candles[size_t(i)];
        const bool up = c.close >= c.open;
        const QColor color = up ? palette.up : palette.down;
        const double x = xOf(i);
        if (maxVolume > 0) {
            QColor faint = color;
            faint.setAlphaF(0.35f);
            const double h = c.volume / maxVolume * volumeHeight;
            painter->fillRect(QRectF(x - body / 2, plot.bottom() - h, body, h), faint);
        }
        painter->setPen(QPen(color, 1));
        painter->drawLine(QPointF(std::round(x) + 0.5, yOf(c.high)), QPointF(std::round(x) + 0.5, yOf(c.low)));
        const double top = yOf(std::max(c.open, c.close));
        const double bottom = yOf(std::min(c.open, c.close));
        painter->fillRect(QRectF(x - body / 2, top, body, std::max(1.0, bottom - top)), color);
    }
    painter->setClipping(false);

    // A dashed line with a filled price tag on the axis.
    const auto level = [&](double price, const QColor &color, const QString &name) {
        if (!(price > low && price < high))
            return;
        const double y = std::round(yOf(price)) + 0.5;
        QPen pen(color, 1, Qt::DashLine);
        painter->setPen(pen);
        painter->drawLine(QPointF(priceArea.left(), y), QPointF(priceArea.right(), y));
        const QRectF tag(priceArea.right() + 2, y - metrics.height() / 2 - 1, axisWidth + 2, metrics.height() + 2);
        painter->fillRect(tag, color);
        painter->setPen(palette.background);
        painter->drawText(tag, Qt::AlignCenter, priceText(price));
        if (!name.isEmpty()) {
            // At the right end of the line, next to its price, clear of the legend.
            painter->setPen(color);
            painter->drawText(QRectF(priceArea.right() - 204, y - metrics.height() - 1, 200, metrics.height()),
                              Qt::AlignRight | Qt::AlignVCenter, name);
        }
    };
    level(levels.target, palette.target, QStringLiteral("TP"));
    level(levels.stop, palette.stop, QStringLiteral("SL"));
    level(levels.entry, palette.entry, QStringLiteral("Entry"));
    // The last price: its line and tag, wherever the view is.
    const Trading::Candle &newest = candles.back();
    level(newest.close, newest.close >= newest.open ? palette.up : palette.down, QString());

    // The crosshair: a line through the candle under the pointer with its
    // time, and a line at the pointer's height with the price there.
    if (view.crosshair && plot.contains(view.pointer)) {
        const int index = int(std::floor((view.pointer.x() - priceArea.left()) / slot + firstIndex));
        QPen pen(palette.text, 1, Qt::DotLine);
        painter->setPen(pen);
        const double y = std::round(view.pointer.y()) + 0.5;
        painter->drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        const bool onCandle = index >= 0 && index < total;
        const double x = std::round(onCandle ? xOf(index) : view.pointer.x()) + 0.5;
        painter->drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        if (view.pointer.y() <= priceArea.bottom()) {
            geometry.hoveredPrice = low + (priceArea.bottom() - view.pointer.y()) / priceArea.height() * span;
            const QRectF tag(priceArea.right() + 2, y - metrics.height() / 2 - 1, axisWidth + 2, metrics.height() + 2);
            painter->fillRect(tag, palette.text);
            painter->setPen(palette.background);
            painter->drawText(tag, Qt::AlignCenter, priceText(geometry.hoveredPrice));
        }
        if (onCandle) {
            geometry.hovered = index;
            const QString label = timeText(candles[size_t(index)].openTime, true);
            const double width = metrics.horizontalAdvance(label) + 12;
            const double left = std::clamp(x - width / 2, plot.left(), plot.right() - width);
            const QRectF tag(left, plot.bottom() + 2, width, metrics.height() + 4);
            painter->fillRect(tag, palette.text);
            painter->setPen(palette.background);
            painter->drawText(tag, Qt::AlignCenter, label);
        }
    }

    painter->restore();
    return geometry;
}

QByteArray renderPng(const Trading::Candles &candles, const QString &title)
{
    QImage image(960, 540, QImage::Format_RGB32);
    Palette palette;
    palette.background = QColor(0x0f, 0x11, 0x17);
    palette.grid = QColor(0x24, 0x28, 0x33);
    palette.text = QColor(0xa9, 0xb1, 0xc2);
    palette.up = QColor(0x26, 0xc2, 0x81);
    palette.down = QColor(0xef, 0x53, 0x50);
    palette.fast = QColor(0xf5, 0xb9, 0x42);
    palette.slow = QColor(0xb3, 0x7f, 0xeb);
    palette.band = QColor(0x4f, 0x8f, 0xe8);
    palette.entry = palette.text;
    palette.stop = palette.down;
    palette.target = palette.up;
    QFont font;
    font.setPixelSize(13);
    QPainter painter(&image);
    const QRectF header(0, 0, image.width(), 30);
    painter.fillRect(image.rect(), palette.background);
    View view;
    view.count = std::min<double>(120, std::max<size_t>(candles.size(), 10));
    paint(&painter, QRectF(0, header.height(), image.width(), image.height() - header.height()), candles, view, {},
          palette, font);
    painter.setFont(font);
    painter.setPen(palette.text);
    // The legend is part of the picture: a model cannot ask what a line is.
    painter.drawText(header.adjusted(12, 0, -12, 0), Qt::AlignLeft | Qt::AlignVCenter,
                     title + QStringLiteral("   yellow: EMA 20   violet: EMA 50   blue: Bollinger bands (20, 2)"));
    painter.end();
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return png;
}

} // namespace ChartRenderer

ChartItem::ChartItem(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
    setAcceptedMouseButtons(Qt::LeftButton);
    setAcceptHoverEvents(true);
    setFlag(ItemIsFocusScope, false);
    setActiveFocusOnTab(true);
    connect(this, &ChartItem::appearanceChanged, this, [this] { update(); });
    connect(this, &ChartItem::viewChanged, this, [this] { update(); });
}

void ChartItem::setSource(QObject *source)
{
    if (source == m_source)
        return;
    if (m_source)
        disconnect(m_source, nullptr, this, nullptr);
    m_source = source;
    if (auto *engine = qobject_cast<TradingEngine *>(source)) {
        connect(engine, &TradingEngine::chartChanged, this, &ChartItem::onChartChanged);
        connect(engine, &TradingEngine::chartReset, this, [this] {
            // Another market: its newest candles, at the zoom that was chosen.
            m_tailSeen = 0;
            m_offset = 0;
            Q_EMIT viewChanged();
        });
        m_tailSeen = engine->tailAdded();
    }
    Q_EMIT sourceChanged();
    update();
}

void ChartItem::onChartChanged()
{
    const auto *engine = qobject_cast<TradingEngine *>(m_source);
    if (!engine)
        return;
    // A view of the past keeps showing the same candles when new ones arrive.
    const int added = engine->tailAdded() - m_tailSeen;
    m_tailSeen = engine->tailAdded();
    const qsizetype total = qsizetype(engine->chartCandles().size());
    if (added > 0 && !live())
        m_offset += added;
    // Older candles arrived, or the view moved with the new ones.
    if (total != m_totalSeen || (added > 0 && !live()))
        Q_EMIT viewChanged();
    m_totalSeen = total;
    update();
}

void ChartItem::setView(double count, double offset)
{
    const auto *engine = qobject_cast<TradingEngine *>(m_source);
    const double total = engine ? double(engine->chartCandles().size()) : 0;
    count = std::clamp(count, 20.0, 2000.0);
    // Into the past until a tenth of the plot still shows candles; into the
    // future until half of it is empty.
    offset = std::clamp(offset, -count * 0.5, std::max(0.0, total - count * 0.1));
    if (qFuzzyCompare(count + 1, m_count + 1) && qFuzzyCompare(offset + 1, m_offset + 1))
        return;
    m_count = count;
    m_offset = offset;
    Q_EMIT viewChanged();
    // Near the oldest candle that is loaded: fetch what came before it.
    if (auto *loader = qobject_cast<TradingEngine *>(m_source)) {
        if (total > 0 && total - 1 - m_offset - m_count < 60 && loader->hasOlder() && !loader->loadingOlder())
            loader->loadOlder();
    }
}

bool ChartItem::atStart() const
{
    const auto *engine = qobject_cast<TradingEngine *>(m_source);
    return engine && double(engine->chartCandles().size()) - 1 - m_offset - m_count < 1;
}

void ChartItem::setVisibleCandles(int count)
{
    setView(count, m_offset);
}

void ChartItem::goLive()
{
    setView(m_count, 0);
}

void ChartItem::zoom(double factor)
{
    zoomAt(factor, m_geometry.plot.isValid() ? m_geometry.plot.center().x() : width() / 2);
}

void ChartItem::pan(double candles)
{
    setView(m_count, m_offset + candles);
}

// Show 'factor' times as many candles, keeping the candle at x where it is.
void ChartItem::zoomAt(double factor, double x)
{
    if (!(m_geometry.slot > 0) || !m_geometry.plot.isValid()) {
        setView(m_count * factor, m_offset);
        return;
    }
    const double count = std::clamp(m_count * factor, 20.0, 2000.0);
    // Slots between x and the right edge, before and after: the difference
    // is how far the right edge has to move.
    const double fromRight = (m_geometry.plot.right() - x) / m_geometry.slot;
    const double slotAfter = m_geometry.plot.width() / count;
    const double fromRightAfter = (m_geometry.plot.right() - x) / slotAfter;
    const double roomBefore = std::min(3.0, m_count * 0.04), roomAfter = std::min(3.0, count * 0.04);
    setView(count, m_offset + (fromRight - roomBefore) - (fromRightAfter - roomAfter));
}

void ChartItem::setPointer(const QPointF &position, bool inside)
{
    m_pointer = position;
    m_pointerInside = inside;
    update();
}

void ChartItem::updateHovered()
{
    QVariantMap now;
    const auto *engine = qobject_cast<TradingEngine *>(m_source);
    if (engine && m_pointerInside && m_geometry.hovered >= 0
        && m_geometry.hovered < int(engine->chartCandles().size())) {
        const Trading::Candle &c = engine->chartCandles()[size_t(m_geometry.hovered)];
        now = {{QStringLiteral("time"), double(c.openTime)}, {QStringLiteral("open"), c.open},
               {QStringLiteral("high"), c.high}, {QStringLiteral("low"), c.low}, {QStringLiteral("close"), c.close},
               {QStringLiteral("volume"), c.volume}, {QStringLiteral("change"), c.close - c.open},
               {QStringLiteral("changePct"), c.open > 0 ? (c.close - c.open) / c.open * 100.0 : 0.0}};
    }
    if (now != m_hovered) {
        m_hovered = now;
        Q_EMIT hoveredChanged();
    }
}

void ChartItem::mousePressEvent(QMouseEvent *event)
{
    forceActiveFocus(Qt::MouseFocusReason);
    // The drag belongs to the chart: the page around it must not take it
    // over to scroll.
    setKeepMouseGrab(true);
    m_dragging = true;
    m_dragX = event->position().x();
    m_dragOffset = m_offset;
    setCursor(Qt::ClosedHandCursor);
    event->accept();
}

void ChartItem::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging && m_geometry.slot > 0) {
        // Dragging to the right pulls older candles into view.
        setView(m_count, m_dragOffset + (event->position().x() - m_dragX) / m_geometry.slot);
    }
    setPointer(event->position(), true);
    event->accept();
}

void ChartItem::mouseReleaseEvent(QMouseEvent *event)
{
    m_dragging = false;
    unsetCursor();
    event->accept();
}

void ChartItem::mouseUngrabEvent()
{
    m_dragging = false;
    unsetCursor();
}

void ChartItem::mouseDoubleClickEvent(QMouseEvent *event)
{
    m_dragging = false;
    setView(120, 0);
    event->accept();
}

void ChartItem::wheelEvent(QWheelEvent *event)
{
    const QPoint angle = event->angleDelta();
    if (angle.x() != 0 && std::abs(angle.x()) > std::abs(angle.y())) {
        // A sideways wheel or a two-finger swipe moves through time.
        pan(angle.x() / 120.0 * std::max(1.0, m_count * 0.08));
    } else if (event->modifiers() & Qt::ShiftModifier) {
        pan(angle.y() / 120.0 * std::max(1.0, m_count * 0.08));
    } else if (angle.y() != 0) {
        // Towards the user: more candles (further away).
        zoomAt(std::pow(0.85, angle.y() / 120.0), event->position().x());
    }
    event->accept();
}

void ChartItem::hoverMoveEvent(QHoverEvent *event)
{
    setPointer(event->position(), true);
}

void ChartItem::hoverLeaveEvent(QHoverEvent *)
{
    setPointer(m_pointer, false);
}

void ChartItem::keyPressEvent(QKeyEvent *event)
{
    const double stepSize = std::max(1.0, m_count * (event->modifiers() & Qt::ShiftModifier ? 0.5 : 0.1));
    switch (event->key()) {
    case Qt::Key_Left: pan(stepSize); break;
    case Qt::Key_Right: pan(-stepSize); break;
    case Qt::Key_Plus:
    case Qt::Key_Equal: zoom(0.8); break;
    case Qt::Key_Minus: zoom(1.25); break;
    case Qt::Key_Home:
    case Qt::Key_End: goLive(); break;
    default: QQuickPaintedItem::keyPressEvent(event); return;
    }
    event->accept();
}

void ChartItem::paint(QPainter *painter)
{
    const auto *engine = qobject_cast<TradingEngine *>(m_source);
    ChartRenderer::Palette palette;
    palette.background = m_background;
    palette.grid = m_grid;
    palette.text = m_text;
    palette.up = m_up;
    palette.down = m_down;
    palette.fast = m_fast;
    palette.slow = m_slow;
    palette.band = m_band;
    palette.entry = m_entryColor;
    palette.stop = m_down;
    palette.target = m_up;
    ChartRenderer::Levels levels;
    levels.entry = m_entry;
    levels.stop = m_stop;
    levels.target = m_target;
    ChartRenderer::View view;
    view.count = m_count;
    view.offset = m_offset;
    view.crosshair = m_pointerInside && !m_dragging;
    view.pointer = m_pointer;
    static const Trading::Candles none;
    m_geometry = ChartRenderer::paint(painter, boundingRect(), engine ? engine->chartCandles() : none, view, levels,
                                      palette, m_font);
    // After painting: the item must not change its properties while it paints.
    QMetaObject::invokeMethod(this, &ChartItem::updateHovered, Qt::QueuedConnection);
}
