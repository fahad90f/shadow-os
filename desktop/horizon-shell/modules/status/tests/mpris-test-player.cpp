// SPDX-License-Identifier: GPL-3.0-or-later
//
// status-test-mpris-player — a minimal MPRIS2 media player for testing the
// shell's media back-end, media card and media keys. It plays nothing: it
// owns org.mpris.MediaPlayer2.<name> on the session bus, answers the
// org.mpris.MediaPlayer2 and .Player interfaces for a three-track play
// list, and prints every command it receives, one per line:
//
//   status-test-mpris-player [--name NAME] [--playing] [--art FILE]
//
//   call PlayPause -> Playing
//   call Next -> Paused track 2
//
// so a test can tell what the shell really sent.
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QDBusVirtualObject>
#include <QStringList>
#include <QTextStream>
#include <QUrl>
#include <QVariantMap>

namespace {

const QString kPath = QStringLiteral("/org/mpris/MediaPlayer2");
const QString kRoot = QStringLiteral("org.mpris.MediaPlayer2");
const QString kPlayer = QStringLiteral("org.mpris.MediaPlayer2.Player");
const QString kProperties = QStringLiteral("org.freedesktop.DBus.Properties");

struct Track { const char *title; const char *artist; const char *album; };
const Track kTracks[] = {
    {"First Light", "The Test Signals", "Nested Sessions"},
    {"Second Wind", "The Test Signals", "Nested Sessions"},
    {"Third Rail", "Fixture & Mock", "No Hardware"},
};
constexpr int kTrackCount = int(sizeof(kTracks) / sizeof(kTracks[0]));

class Player : public QDBusVirtualObject
{
public:
    Player(const QDBusConnection &bus, const QString &identity, bool playing, const QString &art)
        : m_bus(bus), m_identity(identity), m_status(playing ? QStringLiteral("Playing") : QStringLiteral("Paused")),
          m_art(art)
    {
    }

    QString introspect(const QString &) const override { return {}; }

    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override
    {
        const QString interface = message.interface();
        const QString member = message.member();
        const QVariantList args = message.arguments();
        if (interface == kProperties) {
            if (member == QLatin1String("GetAll")) {
                connection.send(message.createReply(QVariant(properties(args.value(0).toString()))));
                return true;
            }
            if (member == QLatin1String("Get")) {
                const QVariantMap all = properties(args.value(0).toString());
                const QString name = args.value(1).toString();
                if (!all.contains(name)) {
                    connection.send(message.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.UnknownProperty"), name));
                    return true;
                }
                connection.send(message.createReply(QVariant::fromValue(QDBusVariant(all.value(name)))));
                return true;
            }
            return false;
        }
        if (interface != kPlayer && interface != kRoot)
            return false;

        if (member == QLatin1String("PlayPause"))
            setStatus(m_status == QLatin1String("Playing") ? QStringLiteral("Paused") : QStringLiteral("Playing"));
        else if (member == QLatin1String("Play"))
            setStatus(QStringLiteral("Playing"));
        else if (member == QLatin1String("Pause"))
            setStatus(QStringLiteral("Paused"));
        else if (member == QLatin1String("Stop"))
            setStatus(QStringLiteral("Stopped"));
        else if (member == QLatin1String("Next"))
            setTrack((m_track + 1) % kTrackCount);
        else if (member == QLatin1String("Previous"))
            setTrack((m_track + kTrackCount - 1) % kTrackCount);
        else if (member == QLatin1String("Quit"))
            QCoreApplication::quit();
        else if (member != QLatin1String("Raise"))
            return false;

        QTextStream(stdout) << "call " << member << " -> " << m_status << " track " << m_track + 1 << Qt::endl;
        connection.send(message.createReply());
        return true;
    }

private:
    QVariantMap metadata() const
    {
        const Track &track = kTracks[m_track];
        QVariantMap map{
            {QStringLiteral("mpris:trackid"),
             QVariant::fromValue(QDBusObjectPath(QStringLiteral("/org/shadow/test/track/%1").arg(m_track + 1)))},
            {QStringLiteral("xesam:title"), QString::fromLatin1(track.title)},
            {QStringLiteral("xesam:artist"), QStringList{QString::fromLatin1(track.artist)}},
            {QStringLiteral("xesam:album"), QString::fromLatin1(track.album)},
        };
        if (!m_art.isEmpty())
            map.insert(QStringLiteral("mpris:artUrl"), QUrl::fromLocalFile(m_art).toString());
        return map;
    }

    QVariantMap properties(const QString &interface) const
    {
        if (interface == kRoot) {
            return {
                {QStringLiteral("Identity"), m_identity},
                {QStringLiteral("CanQuit"), true},
                {QStringLiteral("CanRaise"), false},
                {QStringLiteral("HasTrackList"), false},
                {QStringLiteral("DesktopEntry"), QStringLiteral("status-test-mpris-player")},
                {QStringLiteral("SupportedUriSchemes"), QStringList()},
                {QStringLiteral("SupportedMimeTypes"), QStringList()},
            };
        }
        if (interface == kPlayer) {
            return {
                {QStringLiteral("PlaybackStatus"), m_status},
                {QStringLiteral("Metadata"), metadata()},
                {QStringLiteral("Rate"), 1.0},
                {QStringLiteral("MinimumRate"), 1.0},
                {QStringLiteral("MaximumRate"), 1.0},
                {QStringLiteral("Volume"), 1.0},
                {QStringLiteral("Position"), qlonglong(0)},
                {QStringLiteral("CanGoNext"), true},
                {QStringLiteral("CanGoPrevious"), true},
                {QStringLiteral("CanPlay"), true},
                {QStringLiteral("CanPause"), true},
                {QStringLiteral("CanSeek"), false},
                {QStringLiteral("CanControl"), true},
            };
        }
        return {};
    }

    void announce(const QVariantMap &changed)
    {
        QDBusMessage signal = QDBusMessage::createSignal(kPath, kProperties, QStringLiteral("PropertiesChanged"));
        signal << kPlayer << changed << QStringList();
        m_bus.send(signal);
    }

    void setStatus(const QString &status)
    {
        if (status == m_status)
            return;
        m_status = status;
        announce({{QStringLiteral("PlaybackStatus"), m_status}});
    }

    void setTrack(int track)
    {
        m_track = track;
        announce({{QStringLiteral("Metadata"), metadata()}});
    }

    QDBusConnection m_bus;
    QString m_identity;
    QString m_status;
    QString m_art;
    int m_track = 0;
};

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString name = QStringLiteral("shadowtest");
    QString art;
    bool playing = false;
    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        if (args.at(i) == QLatin1String("--playing"))
            playing = true;
        else if (args.at(i) == QLatin1String("--name") && i + 1 < args.size())
            name = args.at(++i);
        else if (args.at(i) == QLatin1String("--art") && i + 1 < args.size())
            art = args.at(++i);
        else {
            QTextStream(stderr) << "usage: status-test-mpris-player [--name NAME] [--playing] [--art FILE]" << Qt::endl;
            return 2;
        }
    }

    QDBusConnection bus = QDBusConnection::sessionBus();
    Player player(bus, QStringLiteral("SHADOW Test Player (%1)").arg(name), playing, art);
    if (!bus.registerVirtualObject(kPath, &player)
        || !bus.registerService(QStringLiteral("org.mpris.MediaPlayer2.") + name)) {
        QTextStream(stderr) << "status-test-mpris-player: cannot register on the session bus" << Qt::endl;
        return 1;
    }
    QTextStream(stdout) << "ready org.mpris.MediaPlayer2." << name << Qt::endl;
    return app.exec();
}
