// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QApplication>
#include <QCommandLineParser>
#include <QQmlApplicationEngine>
#include <QString>
#include <QStringList>
#include <memory>

namespace Nexus {

struct AppInfo
{
    QString id;             // reverse-DNS id; equals the .desktop file basename and the Wayland app_id
    QString displayName;    // human-readable name, e.g. "NEXUS Files"
    QString iconName;       // icon-theme name of the application icon
    QString description;    // one line, shown by --help
    bool singleInstance = false; // a second launch activates the running instance instead
};

// Object exposed to QML as the context property "nexusApp".
class AppContext : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString id READ id CONSTANT)
    Q_PROPERTY(QString displayName READ displayName CONSTANT)
    Q_PROPERTY(QString iconName READ iconName CONSTANT)
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(QStringList arguments READ arguments NOTIFY argumentsChanged)
public:
    explicit AppContext(const AppInfo &info, QObject *parent = nullptr);
    QString id() const { return m_info.id; }
    QString displayName() const { return m_info.displayName; }
    QString iconName() const { return m_info.iconName; }
    QString version() const;
    QStringList arguments() const { return m_arguments; }
    void setArguments(const QStringList &args);

public Q_SLOTS:
    // D-Bus entry point used by a second launch of a single-instance app.
    void activate(const QStringList &arguments);

Q_SIGNALS:
    void argumentsChanged();
    // Emitted when another launch of a single-instance application hands its
    // arguments to this (already running) instance.
    void activationRequested(const QStringList &arguments);

private:
    AppInfo m_info;
    QStringList m_arguments;
};

// Common runtime of every NEXUS QML application: Qt setup, NEXUS style and
// icon providers, command line handling, single-instance activation and the
// test hooks used by the automated GUI smoke tests:
//
//   NEXUS_SMOKE_TEST=1          load the UI, render, then exit 0 — or 2 if the
//                               QML engine reported any warning or error
//   NEXUS_SCREENSHOT=<file.png> render, save a screenshot of the main window,
//                               then exit (NEXUS_SCREENSHOT_DELAY=<ms>, default 2500)
//   NEXUS_WINDOW_SIZE=<W>x<H>   force the main window size (for stable screenshots)
class App
{
public:
    App(int &argc, char **argv, const AppInfo &info);
    ~App();

    QQmlApplicationEngine &engine() { return *m_engine; }
    QCommandLineParser &parser() { return m_parser; }
    AppContext *context() { return m_context; }

    // Parse the command line (adds --help/--version). Call after adding
    // application-specific options to parser() and before run().
    // Returns false if this process should exit immediately (another
    // instance was activated); the exit code is then 0.
    bool processArguments();

    // Make a C++ object reachable from QML as a context property.
    void expose(const QString &name, QObject *object);

    // Load <uri>'s <type>.qml and run the event loop.
    int run(const QString &uri, const QString &type = QStringLiteral("Main"));

private:
    void setupTestHooks();

    AppInfo m_info;
    std::unique_ptr<QApplication> m_app;
    std::unique_ptr<QQmlApplicationEngine> m_engine;
    QCommandLineParser m_parser;
    AppContext *m_context = nullptr;
    QStringList m_qmlWarnings;
    bool m_argumentsProcessed = false;
};

} // namespace Nexus
