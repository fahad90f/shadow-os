// SPDX-License-Identifier: GPL-3.0-or-later
#include "chartrenderer.h"

#include "skills/indicators.h"
#include "tradingengine.h"

#include <QBuffer>
#include <QDateTime>
#include <QImage>
#include <QPainter>
#include <QPainterPath>

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

void paint(QPainter *painter, const QRectF &rect, const Trading::Candles &candles, int count, const Levels &levels,
           const Palette &palette, const QFont &font)
{
    painter->save();
    painter->fillRect(rect, palette.background);
    const int total = int(candles.size());
    const int shown = std::min(std::max(count, 10), total);
    if (shown < 2) {
        painter->restore();
        return;
    }
    const int first = total - shown;
    painter->setFont(font);
    const QFontMetricsF metrics(font);

    // Layout: prices on the right, times at the bottom, volume in the lowest
    // fifth of the plot.
    const double axisWidth = metrics.horizontalAdvance(priceText(candles.back().close)) + 14;
    const double axisHeight = metrics.height() + 6;
    const QRectF plot(rect.left() + 6, rect.top() + 6, rect.width() - axisWidth - 12, rect.height() - axisHeight - 12);
    if (plot.width() < 40 || plot.height() < 40) {
        painter->restore();
        return;
    }
    const double volumeHeight = plot.height() * 0.18;
    const QRectF priceArea(plot.left(), plot.top(), plot.width(), plot.height() - volumeHeight - 6);

    // Overlays are computed over the whole series so their first visible
    // values are already settled.
    const std::vector<double> closes = Trading::Indicators::closes(candles);
    const std::vector<double> fast = Trading::Indicators::ema(closes, 20);
    const std::vector<double> slow = Trading::Indicators::ema(closes, 50);
    const Trading::Indicators::Bands bands = Trading::Indicators::bollinger(closes, 20, 2.0);

    double low = candles[size_t(first)].low, high = candles[size_t(first)].high, maxVolume = 0;
    for (int i = first; i < total; ++i) {
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

    const double slot = priceArea.width() / shown;
    const double body = std::max(1.0, slot * 0.62);
    const auto xOf = [&](int index) { return priceArea.left() + (index - first + 0.5) * slot; };
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

    // Time labels, about one per 110 px.
    const int every = std::max(1, int(std::ceil(110.0 / slot)));
    const qint64 spanMs = candles.back().openTime - candles[size_t(first)].openTime;
    painter->setPen(palette.text);
    for (int i = first + every / 2; i < total; i += every) {
        const QDateTime time = QDateTime::fromMSecsSinceEpoch(candles[size_t(i)].openTime);
        const QString label = spanMs > 3 * 86400000LL ? time.toString(QStringLiteral("d MMM"))
                                                      : time.toString(QStringLiteral("HH:mm"));
        painter->drawText(QRectF(xOf(i) - 50, plot.bottom() + 4, 100, metrics.height()), Qt::AlignCenter, label);
    }

    // Overlay lines.
    painter->setRenderHint(QPainter::Antialiasing, true);
    const auto line = [&](const std::vector<double> &series, const QColor &color, qreal width) {
        QPainterPath path;
        bool started = false;
        for (int i = first; i < total; ++i) {
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
    for (int i = first; i < total; ++i) {
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
    const Trading::Candle &last = candles.back();
    level(last.close, last.close >= last.open ? palette.up : palette.down, QString());

    painter->restore();
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
    paint(&painter, QRectF(0, header.height(), image.width(), image.height() - header.height()), candles, 120, {},
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
    connect(this, &ChartItem::appearanceChanged, this, [this] { update(); });
}

void ChartItem::setSource(QObject *source)
{
    if (source == m_source)
        return;
    if (m_source)
        disconnect(m_source, nullptr, this, nullptr);
    m_source = source;
    if (auto *engine = qobject_cast<TradingEngine *>(source))
        connect(engine, &TradingEngine::chartChanged, this, [this] { update(); });
    Q_EMIT sourceChanged();
    update();
}

void ChartItem::setVisibleCandles(int count)
{
    count = std::clamp(count, 20, 300);
    if (count == m_visible)
        return;
    m_visible = count;
    Q_EMIT appearanceChanged();
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
    static const Trading::Candles none;
    ChartRenderer::paint(painter, boundingRect(), engine ? engine->candles() : none, m_visible, levels, palette, m_font);
}
