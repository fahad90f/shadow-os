// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "skills/candle.h"

#include <QByteArray>
#include <QColor>
#include <QFont>
#include <QPointF>
#include <QQuickPaintedItem>
#include <QVariantMap>
#include <QRectF>
#include <QString>
#include <QtQml/qqmlregistration.h>

class QPainter;

// The candlestick chart, drawn with QPainter from the same candles the
// analysts read. One painter serves two uses: the "AI chart" in the window
// (ChartItem) and the picture sent to a vision model (renderPng).
namespace ChartRenderer {

struct Palette
{
    QColor background;
    QColor grid;
    QColor text;
    QColor up;
    QColor down;
    QColor fast;   // EMA 20
    QColor slow;   // EMA 50
    QColor band;   // Bollinger bands
    QColor entry;
    QColor stop;
    QColor target;
};

struct Levels
{
    double entry = 0; // 0 = not drawn
    double stop = 0;
    double target = 0;
};

// Which part of the series is drawn, and what the pointer is over.
struct View
{
    double count = 120;       // candles across the plot
    // How far the view is moved into the past, in candles: 0 puts the newest
    // candle at the right edge (with a little room), a positive number shows
    // older candles, a negative one more empty room on the right.
    double offset = 0;
    bool crosshair = false;   // draw lines and tags at 'pointer'
    QPointF pointer;          // in the coordinates of 'rect'
};

// What paint() worked out, for whoever handles the pointer.
struct Geometry
{
    QRectF plot;              // where candles are drawn (prices and volume)
    double slot = 0;          // width of one candle
    double firstIndex = 0;    // the (fractional) index of the series at the plot's left edge
    int hovered = -1;         // index of the candle under the pointer, -1 for none
    double hoveredPrice = 0;  // the price at the pointer's height (0 outside the price area)
};

// Draw the part of 'candles' that 'view' selects into 'rect'.
Geometry paint(QPainter *painter, const QRectF &rect, const Trading::Candles &candles, const View &view,
               const Levels &levels, const Palette &palette, const QFont &font);

// A 960x540 PNG of the newest 120 candles with a title, on a dark background.
QByteArray renderPng(const Trading::Candles &candles, const QString &title);

} // namespace ChartRenderer

// The chart as a QML item. Colours are properties so the page sets them from
// the theme tokens.
class ChartItem : public QQuickPaintedItem
{
    Q_OBJECT
    QML_ELEMENT
    // The TradingEngine ("trading"); the chart repaints when its candles change.
    Q_PROPERTY(QObject *source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(int visibleCandles READ visibleCandles WRITE setVisibleCandles NOTIFY viewChanged)
    // True while the newest candle is in view at the right edge. goLive() returns there.
    Q_PROPERTY(bool live READ live NOTIFY viewChanged)
    // The oldest candle that is loaded is in view.
    Q_PROPERTY(bool atStart READ atStart NOTIFY viewChanged)
    // The candle under the pointer: {time, open, high, low, close, volume, change, changePct}, empty for none.
    Q_PROPERTY(QVariantMap hovered READ hovered NOTIFY hoveredChanged)
    Q_PROPERTY(double entry MEMBER m_entry NOTIFY appearanceChanged)
    Q_PROPERTY(double stop MEMBER m_stop NOTIFY appearanceChanged)
    Q_PROPERTY(double target MEMBER m_target NOTIFY appearanceChanged)
    Q_PROPERTY(QColor backgroundColor MEMBER m_background NOTIFY appearanceChanged)
    Q_PROPERTY(QColor gridColor MEMBER m_grid NOTIFY appearanceChanged)
    Q_PROPERTY(QColor textColor MEMBER m_text NOTIFY appearanceChanged)
    Q_PROPERTY(QColor upColor MEMBER m_up NOTIFY appearanceChanged)
    Q_PROPERTY(QColor downColor MEMBER m_down NOTIFY appearanceChanged)
    Q_PROPERTY(QColor fastColor MEMBER m_fast NOTIFY appearanceChanged)
    Q_PROPERTY(QColor slowColor MEMBER m_slow NOTIFY appearanceChanged)
    Q_PROPERTY(QColor bandColor MEMBER m_band NOTIFY appearanceChanged)
    Q_PROPERTY(QColor entryColor MEMBER m_entryColor NOTIFY appearanceChanged)
    Q_PROPERTY(QFont font MEMBER m_font NOTIFY appearanceChanged)

public:
    explicit ChartItem(QQuickItem *parent = nullptr);

    QObject *source() const { return m_source; }
    void setSource(QObject *source);
    int visibleCandles() const { return int(m_count); }
    void setVisibleCandles(int count);
    bool live() const { return m_offset < 0.5; }
    bool atStart() const;
    QVariantMap hovered() const { return m_hovered; }

    // The view: back to the newest candle; closer / further (factor < 1 shows
    // fewer candles) around the middle; sideways by a number of candles.
    Q_INVOKABLE void goLive();
    Q_INVOKABLE void zoom(double factor);
    Q_INVOKABLE void pan(double candles);

    void paint(QPainter *painter) override;

Q_SIGNALS:
    void sourceChanged();
    void appearanceChanged();
    void viewChanged();
    void hoveredChanged();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseUngrabEvent() override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void hoverLeaveEvent(QHoverEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void setView(double count, double offset);
    void zoomAt(double factor, double x);
    void setPointer(const QPointF &position, bool inside);
    void onChartChanged();
    void updateHovered();

    QObject *m_source = nullptr;
    double m_count = 120;
    double m_offset = 0;
    int m_tailSeen = 0;           // TradingEngine::tailAdded() when last looked
    qsizetype m_totalSeen = 0;
    bool m_dragging = false;
    double m_dragX = 0;
    double m_dragOffset = 0;
    bool m_pointerInside = false;
    QPointF m_pointer;
    ChartRenderer::Geometry m_geometry;
    QVariantMap m_hovered;
    double m_entry = 0;
    double m_stop = 0;
    double m_target = 0;
    QColor m_background, m_grid, m_text, m_up, m_down, m_fast, m_slow, m_band, m_entryColor;
    QFont m_font;
};
