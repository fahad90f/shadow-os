// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The one place that includes QTermWidget's headers.
//
// They use the lower-case Qt keywords ("signals:", "public slots:"), which
// this code base disables with QT_NO_KEYWORDS. The Qt headers they pull in
// are included first, so the temporary definitions below only ever apply to
// QTermWidget's own declarations.
#include <QAction>
#include <QFont>
#include <QHash>
#include <QKeyEvent>
#include <QKeySequence>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QStringList>
#include <QTextStream>
#include <QTimer>
#include <QTranslator>
#include <QVarLengthArray>
#include <QWidget>

#include <cstdio>

#define signals Q_SIGNALS
#define slots Q_SLOTS
#include <qtermwidget.h>
#undef slots
#undef signals
