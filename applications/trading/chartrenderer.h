// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "skills/candle.h"

#include <QByteArray>
#include <QColor>
#include <QFont>
#include <QQuickPaintedItem>
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

// Draw the newest 'count' candles of 'candles' into 'rect'.
void paint(QPainter *painter, const QRectF &rect, const Trading::Candles &candles, int count, const Levels &levels,
           const Palette &palette, const QFont &font);

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
    Q_PROPERTY(int visibleCandles READ visibleCandles WRITE setVisibleCandles NOTIFY appearanceChanged)
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
    int visibleCandles() const { return m_visible; }
    void setVisibleCandles(int count);

    void paint(QPainter *painter) override;

Q_SIGNALS:
    void sourceChanged();
    void appearanceChanged();

private:
    QObject *m_source = nullptr;
    int m_visible = 120;
    double m_entry = 0;
    double m_stop = 0;
    double m_target = 0;
    QColor m_background, m_grid, m_text, m_up, m_down, m_fast, m_slow, m_band, m_entryColor;
    QFont m_font;
};
