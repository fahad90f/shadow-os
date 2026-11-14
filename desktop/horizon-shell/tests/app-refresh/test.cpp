// Installing a package adds a .desktop file; the launcher must show it without a new login.
#include "applications.h"
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QTimer>
#include <cstdio>

static bool waitFor(const std::function<bool()> &cond, int ms)
{
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms) {
        if (cond()) return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    return cond();
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    Applications apps;
    const QString dir = qEnvironmentVariable("TEST_APPDIR");
    const QString id = QStringLiteral("org.test.newapp.desktop");
    std::printf("start: %d apps, new app present: %s\n", apps.totalCount(), apps.entry(id) ? "yes" : "no");

    QElapsedTimer clock; clock.start();
    QFile f(dir + QStringLiteral("/") + id);           // what dpkg does on install
    f.open(QIODevice::WriteOnly);
    f.write("[Desktop Entry]\nType=Application\nName=Freshly Installed\nExec=true\nCategories=Utility;\n");
    f.close();
    const bool appeared = waitFor([&] { return apps.entry(id) != nullptr; }, 20000);
    std::printf("after install: %d apps, new app present: %s (after %lld ms)\n",
                apps.totalCount(), appeared ? "yes" : "no", (long long)clock.elapsed());
    apps.setFilter(QStringLiteral("freshly"));
    const bool searchable = apps.count() == 1;
    std::printf("search 'freshly' finds it: %s\n", searchable ? "yes" : "no");
    apps.setFilter(QString());

    clock.restart();
    QFile::remove(dir + QStringLiteral("/") + id);     // what dpkg does on removal
    const bool gone = waitFor([&] { return apps.entry(id) == nullptr; }, 20000);
    std::printf("after removal: %d apps, new app present: %s (after %lld ms)\n",
                apps.totalCount(), gone ? "no" : "yes", (long long)clock.elapsed());

    const bool ok = appeared && searchable && gone;
    std::printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
