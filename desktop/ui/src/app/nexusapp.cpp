// SPDX-License-Identifier: GPL-3.0-or-later
#include "nexusapp.h"

#include "../iconprovider.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QFont>
#include <QIcon>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickWindow>
#include <QTimer>

#include <cstdio>

namespace Nexus {

namespace {
const char *kDBusPath = "/org/nexus/Application";
const char *kDBusInterface = "org.nexus.Application";
} // namespace

AppContext::AppContext(const AppInfo &info, QObject *parent)
    : QObject(parent), m_info(info)
{
}

QString AppContext::version() const
{
    return QStringLiteral(NEXUS_VERSION);
}

void AppContext::setArguments(const QStringList &args)
{
    m_arguments = args;
    Q_EMIT argumentsChanged();
}

void AppContext::activate(const QStringList &arguments)
{
    Q_EMIT activationRequested(arguments);
}

App::App(int &argc, char **argv, const AppInfo &info)
    : m_info(info)
{
    // The NEXUS Qt Quick Controls style; an explicit user/test override wins.
    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE"))
        qputenv("QT_QUICK_CONTROLS_STYLE", "org.nexus.style");
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    m_app = std::make_unique<QApplication>(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("nexus"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("nexus.org"));
    QCoreApplication::setApplicationName(info.id.section(QLatin1Char('.'), -1));
    QCoreApplication::setApplicationVersion(QStringLiteral(NEXUS_VERSION));
    QGuiApplication::setApplicationDisplayName(info.displayName);
    QGuiApplication::setDesktopFileName(info.id);
    if (!info.iconName.isEmpty())
        QGuiApplication::setWindowIcon(QIcon::fromTheme(info.iconName));

    QFont font(QStringLiteral("Inter Variable"));
    font.setPixelSize(13);
    font.setStyleHint(QFont::SansSerif);
    QGuiApplication::setFont(font);

    m_parser.setApplicationDescription(info.description.isEmpty() ? info.displayName : info.description);
    m_parser.addHelpOption();
    m_parser.addVersionOption();

    m_context = new AppContext(info, m_app.get());

    m_engine = std::make_unique<QQmlApplicationEngine>();
    m_engine->addImageProvider(QStringLiteral("nxicon"), new SymbolicIconProvider);
    m_engine->addImageProvider(QStringLiteral("nxtheme"), new ThemeIconProvider);
    m_engine->rootContext()->setContextProperty(QStringLiteral("nexusApp"), m_context);

    QObject::connect(m_engine.get(), &QQmlEngine::warnings, m_app.get(),
                     [this](const QList<QQmlError> &warnings) {
        for (const QQmlError &w : warnings)
            m_qmlWarnings << w.toString();
    });
}

App::~App()
{
    // The engine must go before the application object.
    m_engine.reset();
}

bool App::processArguments()
{
    m_parser.process(*m_app);
    m_argumentsProcessed = true;
    m_context->setArguments(m_parser.positionalArguments());

    if (!m_info.singleInstance || !qEnvironmentVariableIsEmpty("NEXUS_SMOKE_TEST")
        || !qEnvironmentVariableIsEmpty("NEXUS_SCREENSHOT"))
        return true;

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return true; // no session bus (e.g. recovery console): just run

    if (bus.registerService(m_info.id)) {
        bus.registerObject(QString::fromLatin1(kDBusPath), QString::fromLatin1(kDBusInterface),
                           m_context, QDBusConnection::ExportAllSlots);
        return true;
    }

    // Another instance owns the name: hand over our arguments and leave.
    QDBusInterface other(m_info.id, QString::fromLatin1(kDBusPath), QString::fromLatin1(kDBusInterface), bus);
    other.call(QDBus::NoBlock, QStringLiteral("activate"), m_parser.positionalArguments());
    return false;
}

void App::expose(const QString &name, QObject *object)
{
    m_engine->rootContext()->setContextProperty(name, object);
}

void App::setupTestHooks()
{
    const bool smoke = qEnvironmentVariableIntValue("NEXUS_SMOKE_TEST") == 1;
    const QString shot = qEnvironmentVariable("NEXUS_SCREENSHOT");
    const QString forcedSize = qEnvironmentVariable("NEXUS_WINDOW_SIZE");

    QQuickWindow *window = nullptr;
    const auto roots = m_engine->rootObjects();
    for (QObject *o : roots) {
        if ((window = qobject_cast<QQuickWindow *>(o)))
            break;
    }

    if (window && !forcedSize.isEmpty()) {
        const QStringList wh = forcedSize.split(QLatin1Char('x'));
        if (wh.size() == 2) {
            window->setWidth(wh.at(0).toInt());
            window->setHeight(wh.at(1).toInt());
        }
    }

    if (!smoke && shot.isEmpty())
        return;

    bool ok = false;
    int delay = qEnvironmentVariableIntValue("NEXUS_SCREENSHOT_DELAY", &ok);
    if (!ok || delay <= 0)
        delay = smoke && shot.isEmpty() ? 1500 : 2500;

    QTimer::singleShot(delay, m_app.get(), [this, window, shot, smoke] {
        int rc = 0;
        if (!shot.isEmpty()) {
            if (!window) {
                std::fprintf(stderr, "NEXUS-SCREENSHOT-FAILED no window\n");
                rc = 3;
            } else {
                const QImage image = window->grabWindow();
                if (image.isNull() || !image.save(shot)) {
                    std::fprintf(stderr, "NEXUS-SCREENSHOT-FAILED %s\n", qPrintable(shot));
                    rc = 3;
                } else {
                    std::printf("NEXUS-SCREENSHOT-SAVED %s %dx%d\n", qPrintable(shot), image.width(), image.height());
                }
            }
        }
        if (smoke) {
            if (!m_qmlWarnings.isEmpty()) {
                for (const QString &w : std::as_const(m_qmlWarnings))
                    std::fprintf(stderr, "QML-WARNING %s\n", qPrintable(w));
                std::fprintf(stderr, "NEXUS-SMOKE-FAILED %s (%d QML warnings)\n",
                             qPrintable(m_info.id), int(m_qmlWarnings.size()));
                rc = rc ? rc : 2;
            } else if (rc == 0) {
                std::printf("NEXUS-SMOKE-OK %s\n", qPrintable(m_info.id));
            }
        }
        std::fflush(stdout);
        std::fflush(stderr);
        QCoreApplication::exit(rc);
    });
}

int App::run(const QString &uri, const QString &type)
{
    if (!m_argumentsProcessed && !processArguments())
        return 0;

    QObject::connect(m_engine.get(), &QQmlApplicationEngine::objectCreationFailed, m_app.get(), [this] {
        for (const QString &w : std::as_const(m_qmlWarnings))
            std::fprintf(stderr, "QML-ERROR %s\n", qPrintable(w));
        std::fprintf(stderr, "NEXUS-LOAD-FAILED %s\n", qPrintable(m_info.id));
        QCoreApplication::exit(1);
    }, Qt::QueuedConnection);

    m_engine->loadFromModule(uri, type);
    if (m_engine->rootObjects().isEmpty())
        return 1;

    setupTestHooks();
    return m_app->exec();
}

} // namespace Nexus
