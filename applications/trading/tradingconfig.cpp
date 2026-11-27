// SPDX-License-Identifier: GPL-3.0-or-later
#include "tradingconfig.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace {

QString configBase()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/shadow");
}

const char kNvidiaUrl[] = "https://integrate.api.nvidia.com/v1";

// The built-in panel: fourteen of NVIDIA's hosted models. Six form the
// analysis team (one per seat, see TradingEngine::teamSeats), three argue the
// other side, three read the picture and two work the numbers. Every id is in
// NVIDIA's catalog as of late November 2026; a model the service no longer
// offers is shown as unavailable and left out of the analyses.
QList<TradingConfig::Member> builtinPanel()
{
    struct Row { const char *role; const char *model; };
    static const Row rows[] = {
        {"analysis", "mistralai/mistral-large-4-1t-a52b"},
        {"analysis", "z-ai/glm-5.3-flash"},
        {"analysis", "google/gemma-3-12b-it"},
        {"analysis", "nvidia/nemotron-3.5-lightning-30b-a3b"},
        {"analysis", "poolside/laguna-xs-2.1"},
        {"analysis", "google/gemma-3-4b-it"},
        {"reasoning", "moonshotai/kimi-k3.1-pro"},
        {"reasoning", "nvidia/nemotron-3-ultra-550b-a55b"},
        {"reasoning", "stepfun-ai/step-5-preview"},
        // The models that read pictures.
        {"vision", "nvidia/nemotron-3.5-super-vl-120b-a12b"},
        {"vision", "nvidia/nemotron-3-nano-omni-30b-a3b-reasoning"},
        {"vision", "nvidia/cosmos-reason2-8b"},
        {"compute", "minimaxai/minimax-m3.1-flash"},
        {"compute", "nvidia/nemotron-3.5-bolt-e4b"},
    };
    QList<TradingConfig::Member> panel;
    for (const Row &row : rows) {
        TradingConfig::Member m;
        m.role = QLatin1String(row.role);
        m.model = QLatin1String(row.model);
        m.vision = m.role == QLatin1String("vision");
        panel.append(m);
    }
    return panel;
}

bool validModelId(const QString &model)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,60}/[A-Za-z0-9][A-Za-z0-9._:-]{0,100}$"));
    return re.match(model).hasMatch();
}

// The first line of an owner-only file.
QString readKeyFile(const QString &path)
{
    QFile f(path);
    if (!QFileInfo(path).isFile() || !f.open(QIODevice::ReadOnly))
        return {};
    const QString key = QString::fromUtf8(f.readLine(1024)).trimmed();
    const QFile::Permissions others = QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup
                                    | QFile::ReadOther | QFile::WriteOther | QFile::ExeOther;
    if (f.permissions() & others)
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
    return key;
}

// A key that plainly belongs to another service: sending it to NVIDIA would
// hand that service's credential to the wrong company.
QString foreignKeyOwner(const QString &key)
{
    if (key.startsWith(QLatin1String("sk-ant-")))
        return QStringLiteral("Anthropic");
    if (key.startsWith(QLatin1String("sk-")))
        return QStringLiteral("OpenAI");
    if (key.startsWith(QLatin1String("AIza")))
        return QStringLiteral("Google");
    return {};
}


double clampOr(double value, double low, double high, double fallback)
{
    return std::isfinite(value) && value >= low && value <= high ? value : fallback;
}

} // namespace

bool TradingConfig::validSymbol(const QString &symbol)
{
    static const QRegularExpression re(QStringLiteral("^[A-Z0-9]{5,20}$"));
    return re.match(symbol).hasMatch();
}

TradingConfig::TradingConfig(QObject *parent)
    : QObject(parent)
{
    reload();
}

QString TradingConfig::configPath() const
{
    return configBase() + QStringLiteral("/trading.conf");
}

QString TradingConfig::dataDir() const
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                        + QStringLiteral("/shadow/trading");
    QDir().mkpath(dir);
    return dir;
}

QStringList TradingConfig::intervals()
{
    return {QStringLiteral("1m"), QStringLiteral("5m"), QStringLiteral("15m"), QStringLiteral("1h"),
            QStringLiteral("4h"), QStringLiteral("1d")};
}

QString TradingConfig::higherInterval(const QString &interval)
{
    if (interval == QLatin1String("1m")) return QStringLiteral("15m");
    if (interval == QLatin1String("5m")) return QStringLiteral("1h");
    if (interval == QLatin1String("15m")) return QStringLiteral("1h");
    if (interval == QLatin1String("1h")) return QStringLiteral("4h");
    if (interval == QLatin1String("4h")) return QStringLiteral("1d");
    return {}; // nothing above a day is read
}

qint64 TradingConfig::intervalMs(const QString &interval)
{
    if (interval == QLatin1String("1m")) return 60000;
    if (interval == QLatin1String("5m")) return 300000;
    if (interval == QLatin1String("15m")) return 900000;
    if (interval == QLatin1String("1h")) return 3600000;
    if (interval == QLatin1String("4h")) return 14400000;
    if (interval == QLatin1String("1d")) return 86400000;
    return 0;
}

QStringList TradingConfig::roles()
{
    return {QStringLiteral("analysis"), QStringLiteral("reasoning"), QStringLiteral("vision"),
            QStringLiteral("compute")};
}

QString TradingConfig::publisher(const QString &model)
{
    return model.section(QLatin1Char('/'), 0, 0);
}

QString TradingConfig::displayName(const QString &model)
{
    // "nemotron-3.5-super-vl-120b-a12b" -> "Nemotron 3.5 Super VL": the size
    // and build tokens say nothing a person choosing a model needs.
    static const QRegularExpression sizeToken(QStringLiteral("^(\\d+(\\.\\d+)?[bt]|[ae]\\d+b|it)$"));
    static const QHash<QString, QString> spelled = {
        {QStringLiteral("glm"), QStringLiteral("GLM")}, {QStringLiteral("vl"), QStringLiteral("VL")},
        {QStringLiteral("minimax"), QStringLiteral("MiniMax")}, {QStringLiteral("xs"), QStringLiteral("XS")}};
    const QStringList tokens = model.section(QLatin1Char('/'), 1).split(QLatin1Char('-'), Qt::SkipEmptyParts);
    QStringList words;
    for (const QString &token : tokens) {
        if (sizeToken.match(token).hasMatch())
            continue;
        const QString known = spelled.value(token.toLower());
        words.append(!known.isEmpty() ? known : token.left(1).toUpper() + token.mid(1));
    }
    return words.isEmpty() ? model : words.join(QLatin1Char(' '));
}

QString TradingConfig::memberName(const QString &model) const
{
    const QString name = displayName(model);
    const bool shared = std::any_of(m_panel.cbegin(), m_panel.cend(), [&](const Member &other) {
        return other.model != model && displayName(other.model) == name;
    });
    if (!shared)
        return name;
    // The first size token of the id: "gemma-3-12b-it" -> "12B".
    static const QRegularExpression size(QStringLiteral("^\\d+(\\.\\d+)?[bt]$"));
    const QStringList tokens = model.section(QLatin1Char('/'), 1).split(QLatin1Char('-'), Qt::SkipEmptyParts);
    for (const QString &token : tokens) {
        if (size.match(token).hasMatch())
            return name + QLatin1Char(' ') + token.toUpper();
    }
    return name;
}

QString TradingConfig::shortName(const QString &model)
{
    return displayName(model).section(QLatin1Char(' '), 0, 0);
}

void TradingConfig::reload()
{
    const QSettings ini(configPath(), QSettings::IniFormat);

    m_baseUrl = ini.value(QStringLiteral("nvidia/base_url")).toString().trimmed();
    // The test hook: SHADOW_TRADING_NVIDIA_URL=http://127.0.0.1:port/v1
    const QByteArray urlEnv = qgetenv("SHADOW_TRADING_NVIDIA_URL");
    if (!urlEnv.isEmpty())
        m_baseUrl = QString::fromUtf8(urlEnv).trimmed();
    if (m_baseUrl.isEmpty())
        m_baseUrl = QLatin1String(kNvidiaUrl);
    while (m_baseUrl.endsWith(QLatin1Char('/')))
        m_baseUrl.chop(1);

    // The panel: the lists in the file when it has any, else the built-in one.
    m_panel.clear();
    bool panelInFile = false;
    const QStringList roleNames = roles();
    for (const QString &role : roleNames) {
        const QString key = QStringLiteral("panel/") + role;
        if (!ini.contains(key))
            continue;
        panelInFile = true;
        // QSettings reads "a, b" as a list; either way, one id per entry.
        const QStringList models = ini.value(key).toStringList().join(QLatin1Char(','))
                                       .split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString &raw : models) {
            const QString model = raw.trimmed();
            if (!validModelId(model) || !member(model).model.isEmpty() || m_panel.size() >= 24)
                continue;
            Member m;
            m.model = model;
            m.role = role;
            m.vision = role == QLatin1String("vision");
            m_panel.append(m);
        }
    }
    if (!panelInFile)
        m_panel = builtinPanel();
    const QStringList off = ini.value(QStringLiteral("panel/off")).toStringList().join(QLatin1Char(','))
                                .split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (Member &m : m_panel) {
        for (const QString &raw : off)
            m.enabled = m.enabled && raw.trimmed() != m.model;
    }

    // Which markets: crypto (Binance's public data) or forex (the currency
    // pairs and gold of Kraken's public data). Each has its own source, its
    // own list of markets and its own selection.
    m_forex = ini.value(QStringLiteral("market/kind")).toString().trimmed().toLower() == QLatin1String("forex");
    const QString dataKey = marketKey(QStringLiteral("data_url"));
    m_dataUrl = ini.value(dataKey).toString().trimmed();
    const QByteArray dataEnv = qgetenv("SHADOW_TRADING_DATA_URL");
    if (!dataEnv.isEmpty())
        m_dataUrl = QString::fromUtf8(dataEnv).trimmed();
    if (m_dataUrl.isEmpty()) // public market data, no key
        m_dataUrl = m_forex ? QStringLiteral("https://api.kraken.com") : QStringLiteral("https://data-api.binance.vision");
    while (m_dataUrl.endsWith(QLatin1Char('/')))
        m_dataUrl.chop(1);

    // The live stream belongs to the data service: the built-in one has a
    // public stream; another service has one only when its address is given.
    // "off" turns it off (prices are then asked for every few seconds).
    const bool builtinData = dataEnv.isEmpty() && ini.value(dataKey).toString().trimmed().isEmpty();
    m_streamUrl = ini.value(marketKey(QStringLiteral("stream_url"))).toString().trimmed();
    const QByteArray streamEnv = qgetenv("SHADOW_TRADING_STREAM_URL");
    if (!streamEnv.isEmpty())
        m_streamUrl = QString::fromUtf8(streamEnv).trimmed();
    if (m_streamUrl.isEmpty() && builtinData) // public market data, no key
        m_streamUrl = m_forex ? QStringLiteral("wss://ws.kraken.com") : QStringLiteral("wss://data-stream.binance.vision");
    if (m_streamUrl.compare(QLatin1String("off"), Qt::CaseInsensitive) == 0
        || !(m_streamUrl.startsWith(QLatin1String("wss://")) || m_streamUrl.startsWith(QLatin1String("ws://"))))
        m_streamUrl.clear();
    while (m_streamUrl.endsWith(QLatin1Char('/')))
        m_streamUrl.chop(1);

    m_watchlist.clear();
    // QSettings reads "a, b" as a list (and a list has no text of its own).
    const QStringList listed = ini.value(marketKey(QStringLiteral("watchlist"))).toStringList().join(QLatin1Char(','))
                                   .split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &raw : listed) {
        const QString symbol = raw.trimmed().toUpper();
        if (validSymbol(symbol) && !m_watchlist.contains(symbol) && m_watchlist.size() < 12)
            m_watchlist.append(symbol);
    }
    if (m_watchlist.isEmpty() && m_forex) {
        // The pairs with the dollar that trade often enough there to chart,
        // and gold (the PAX Gold token against the dollar).
        m_watchlist = {QStringLiteral("EURUSD"), QStringLiteral("GBPUSD"), QStringLiteral("AUDUSD"),
                       QStringLiteral("USDCAD"), QStringLiteral("USDCHF"), QStringLiteral("PAXGUSD")};
    } else if (m_watchlist.isEmpty()) {
        // Gold and the euro as they trade on a crypto exchange: the PAX Gold
        // token and EUR against USDT. The currency pairs themselves are the
        // forex markets (market/kind=forex).
        m_watchlist = {QStringLiteral("BTCUSDT"), QStringLiteral("ETHUSDT"), QStringLiteral("SOLUSDT"),
                       QStringLiteral("PAXGUSDT"), QStringLiteral("EURUSDT")};
    }
    m_symbol = ini.value(marketKey(QStringLiteral("symbol"))).toString().trimmed().toUpper();
    if (!m_watchlist.contains(m_symbol))
        m_symbol = m_watchlist.first();
    m_interval = ini.value(QStringLiteral("market/interval")).toString().trimmed();
    if (!intervals().contains(m_interval))
        m_interval = QStringLiteral("15m");

    // Off until the user turns it on.
    m_autoTrade = ini.value(QStringLiteral("engine/auto_trade"), false).toBool();
    m_autoAnalyze = ini.value(QStringLiteral("engine/auto_analyze"), true).toBool();
    m_teamReview = ini.value(QStringLiteral("engine/team_review"), true).toBool();
    // The desk: a quick model that reads every message, a deeper one that
    // answers questions; "" = the built-in choice.
    m_deskFast = ini.value(QStringLiteral("desk/fast_model")).toString().trimmed();
    if (!validModelId(m_deskFast))
        m_deskFast.clear();
    m_deskDeep = ini.value(QStringLiteral("desk/deep_model")).toString().trimmed();
    if (!validModelId(m_deskDeep))
        m_deskDeep.clear();
    m_deskFollow = ini.value(QStringLiteral("desk/follow_chart"), true).toBool();

    const Trading::RiskSettings d;
    const auto number = [&ini](const char *key, double fallback) {
        bool ok = false;
        const double v = ini.value(QString::fromLatin1(key)).toString().toDouble(&ok);
        return ok ? v : fallback;
    };
    // The file may ask for less than the window offers (a test does not wait minutes).
    m_modelTimeout = int(clampOr(number("nvidia/timeout_seconds", 180), 2, kTimeoutMax, 180));
    m_modelParallel = int(clampOr(number("nvidia/parallel", 6), 1, 24, 6));
    // SBT Model 1, the strategy trades come from.
    const Trading::Sbt::Settings sd;
    m_sbt = sd;
    m_sbt.swingStrength = int(clampOr(number("sbt/swing_strength", sd.swingStrength), 1, 5, sd.swingStrength));
    m_sbt.zoneBody = ini.value(QStringLiteral("sbt/zone")).toString().trimmed().toLower() == QLatin1String("body");
    m_sbt.stopBufferAtr = clampOr(number("sbt/stop_buffer_atr", sd.stopBufferAtr), 0, 2, sd.stopBufferAtr);
    m_sbt.requirePdArray = ini.value(QStringLiteral("sbt/require_pd_array"), true).toBool();
    m_sbt.pdDistanceAtr = clampOr(number("sbt/pd_distance_atr", sd.pdDistanceAtr), 0.2, 5, sd.pdDistanceAtr);
    m_sbt.maxArmedCandles = int(clampOr(number("sbt/max_wait_candles", sd.maxArmedCandles), 10, 1000, sd.maxArmedCandles));
    m_sbtAiConfirm = ini.value(QStringLiteral("sbt/ai_confirm"), true).toBool();
    m_risk = d;
    m_risk.riskPerTradePct = clampOr(number("risk/risk_per_trade_pct", d.riskPerTradePct), 0.1, 5, d.riskPerTradePct);
    m_risk.maxDailyLossPct = clampOr(number("risk/max_daily_loss_pct", d.maxDailyLossPct), 0.5, 20, d.maxDailyLossPct);
    m_risk.maxDrawdownPct = clampOr(number("risk/max_drawdown_pct", d.maxDrawdownPct), 1, 50, d.maxDrawdownPct);
    m_risk.minRewardRisk = clampOr(number("risk/min_reward_risk", d.minRewardRisk), 1, 10, d.minRewardRisk);
    m_risk.minConfidence = clampOr(number("risk/min_confidence", d.minConfidence), 30, 100, d.minConfidence);
    m_risk.minAgreeing = int(clampOr(number("risk/min_agreeing", d.minAgreeing), 1, 5, d.minAgreeing));
    m_risk.maxOpenPositions = int(clampOr(number("risk/max_open_positions", d.maxOpenPositions), 1, 10,
                                          d.maxOpenPositions));
    m_risk.cooldownMinutes = int(clampOr(number("risk/cooldown_minutes", d.cooldownMinutes), 0, 1440,
                                         d.cooldownMinutes));

    const Trading::PaperSettings pd;
    m_paper = pd;
    m_paper.startingBalance = clampOr(number("paper/starting_balance", pd.startingBalance), 100, 1e7,
                                      pd.startingBalance);
    m_paper.feePct = clampOr(number("paper/fee_pct", pd.feePct), 0, 1, pd.feePct);
    m_paper.slippagePct = clampOr(number("paper/slippage_pct", pd.slippagePct), 0, 1, pd.slippagePct);
}

void TradingConfig::store(const QString &key, const QVariant &value)
{
    QDir().mkpath(configBase());
    QSettings ini(configPath(), QSettings::IniFormat);
    ini.setValue(key, value);
    ini.sync();
    // Settings are not secret, but they are nobody else's business either.
    QFile::setPermissions(configPath(), QFile::ReadOwner | QFile::WriteOwner);
}

TradingConfig::Member TradingConfig::member(const QString &model) const
{
    for (const Member &m : m_panel) {
        if (m.model == model)
            return m;
    }
    return {};
}

void TradingConfig::storePanel(const QList<Member> &panel)
{
    QDir().mkpath(configBase());
    QSettings ini(configPath(), QSettings::IniFormat);
    QStringList off;
    const QStringList roleNames = roles();
    for (const QString &role : roleNames) {
        QStringList models;
        for (const Member &m : panel) {
            if (m.role == role)
                models.append(m.model);
        }
        ini.setValue(QStringLiteral("panel/") + role, models.join(QStringLiteral(", ")));
    }
    for (const Member &m : panel) {
        if (!m.enabled)
            off.append(m.model);
    }
    ini.setValue(QStringLiteral("panel/off"), off.join(QStringLiteral(", ")));
    ini.sync();
    QFile::setPermissions(configPath(), QFile::ReadOwner | QFile::WriteOwner);
    reload();
    Q_EMIT changed();
}

void TradingConfig::setMemberEnabled(const QString &model, bool enabled)
{
    QList<Member> panel = m_panel;
    for (Member &m : panel) {
        if (m.model == model) {
            if (m.enabled == enabled)
                return;
            m.enabled = enabled;
            storePanel(panel);
            return;
        }
    }
}

QString TradingConfig::addMember(const QString &model, const QString &role)
{
    const QString id = model.trimmed();
    if (!roles().contains(role))
        return tr("Unknown role.");
    if (!validModelId(id))
        return tr("A model is named publisher/model, for example moonshotai/kimi-k3.1-pro.");
    if (!member(id).model.isEmpty())
        return tr("%1 is already on the panel.").arg(displayName(id));
    if (m_panel.size() >= 24)
        return tr("The panel is full (24 models).");
    QList<Member> panel = m_panel;
    Member m;
    m.model = id;
    m.role = role;
    panel.append(m);
    storePanel(panel);
    return {};
}

void TradingConfig::removeMember(const QString &model)
{
    QList<Member> panel;
    for (const Member &m : std::as_const(m_panel)) {
        if (m.model != model)
            panel.append(m);
    }
    if (panel.size() != m_panel.size())
        storePanel(panel);
}

void TradingConfig::resetPanel()
{
    QDir().mkpath(configBase());
    {
        QSettings ini(configPath(), QSettings::IniFormat);
        ini.remove(QStringLiteral("panel"));
        ini.sync();
    }
    reload();
    Q_EMIT changed();
}

QString TradingConfig::keyFilePath() const
{
    return configBase() + QStringLiteral("/trading/nvidia.key");
}

QString TradingConfig::savedKey() const
{
    return readKeyFile(keyFilePath());
}

// The key saved in the Shadow Assistant, when it is an NVIDIA key: someone who
// set the assistant up does not have to paste the same key twice.
QString TradingConfig::assistantKey() const
{
    const QString key = readKeyFile(configBase() + QStringLiteral("/assistant.key"));
    return key.startsWith(QLatin1String("nvapi-")) ? key : QString();
}

// A key from the environment or from a file is used only if it is not plainly
// another company's: that one is passed over, never sent to NVIDIA.
static bool usableKey(const QString &key)
{
    return !key.isEmpty() && foreignKeyOwner(key).isEmpty();
}

QString TradingConfig::keySource() const
{
    if (usableKey(QString::fromUtf8(qgetenv("NVIDIA_API_KEY")).trimmed()))
        return QStringLiteral("environment");
    if (usableKey(savedKey()))
        return QStringLiteral("saved");
    if (!assistantKey().isEmpty())
        return QStringLiteral("assistant");
    return {};
}

QString TradingConfig::apiKey() const
{
    const QString fromEnv = QString::fromUtf8(qgetenv("NVIDIA_API_KEY")).trimmed();
    if (usableKey(fromEnv))
        return fromEnv;
    const QString saved = savedKey();
    return usableKey(saved) ? saved : assistantKey();
}

QString TradingConfig::keyHint() const
{
    const QString key = apiKey();
    return key.size() >= 12 ? key.right(4) : QString();
}

QString TradingConfig::saveKey(const QString &key)
{
    const QString k = key.trimmed();
    if (k.isEmpty())
        return tr("Paste the key first.");
    if (k.size() < 20 || k.size() > 512)
        return tr("That does not look like an API key (NVIDIA keys start with nvapi- and are about 70 characters).");
    for (const QChar c : k) {
        if (c.unicode() <= 0x20 || c.unicode() >= 0x7f)
            return tr("The key contains a space or a character that cannot be part of a key. Copy it again.");
    }
    if (k.contains(QLatin1String("XXXXXXXX")))
        return tr("That is an example, not a real key.");
    // The key goes to NVIDIA and nowhere else; another company's key is
    // refused here rather than sent to NVIDIA.
    const QString owner = foreignKeyOwner(k);
    if (!owner.isEmpty())
        return tr("That is a key for %1, not for NVIDIA. NVIDIA keys start with nvapi-.").arg(owner);

    const QString path = keyFilePath();
    const QString dir = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(dir))
        return tr("Could not create %1.").arg(dir);
    QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    // Created readable by its owner only, written in full, then moved into
    // place in one step: there is no moment at which anyone else could read it.
    const QByteArray target = QFile::encodeName(path);
    const QByteArray tmp = target + ".new-" + QByteArray::number(qint64(QCoreApplication::applicationPid()));
    ::unlink(tmp.constData());
    const int fd = ::open(tmp.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return tr("Could not write %1: %2").arg(path, QString::fromLocal8Bit(std::strerror(errno)));
    const QByteArray data = k.toUtf8() + '\n';
    int problem = 0;
    qsizetype written = 0;
    while (problem == 0 && written < data.size()) {
        const ssize_t n = ::write(fd, data.constData() + written, size_t(data.size() - written));
        if (n < 0) {
            if (errno != EINTR)
                problem = errno;
        } else {
            written += n;
        }
    }
    if (problem == 0 && ::fsync(fd) != 0)
        problem = errno;
    if (::close(fd) != 0 && problem == 0)
        problem = errno;
    if (problem == 0 && ::rename(tmp.constData(), target.constData()) != 0)
        problem = errno;
    if (problem != 0) {
        ::unlink(tmp.constData());
        return tr("Could not write %1: %2").arg(path, QString::fromLocal8Bit(std::strerror(problem)));
    }
    Q_EMIT keysChanged();
    return {};
}

void TradingConfig::removeKey()
{
    QFile::remove(keyFilePath());
    Q_EMIT keysChanged();
}

QString TradingConfig::marketKey(const QString &name) const
{
    return (m_forex ? QStringLiteral("market/forex_") : QStringLiteral("market/")) + name;
}

void TradingConfig::setForex(bool forex)
{
    if (forex == m_forex)
        return;
    store(QStringLiteral("market/kind"), forex ? QStringLiteral("forex") : QStringLiteral("crypto"));
    reload(); // the other source, list and selection
    Q_EMIT changed();
}

void TradingConfig::setSymbol(const QString &symbol)
{
    if (!m_watchlist.contains(symbol) || symbol == m_symbol)
        return;
    store(marketKey(QStringLiteral("symbol")), symbol);
    m_symbol = symbol;
    Q_EMIT changed();
}

QString TradingConfig::addMarket(const QString &raw)
{
    const QString symbol = raw.trimmed().toUpper();
    if (!validSymbol(symbol))
        return tr("\"%1\" is not a market name. Use the data service's name, such as %2.")
            .arg(raw.trimmed(), m_forex ? QStringLiteral("EURUSD") : QStringLiteral("DOGEUSDT"));
    if (m_watchlist.contains(symbol))
        return tr("%1 is already in the list.").arg(symbol);
    if (m_watchlist.size() >= kMaxMarkets)
        return tr("The list holds %1 markets at most. Remove one first.").arg(kMaxMarkets);
    m_watchlist.append(symbol);
    store(marketKey(QStringLiteral("watchlist")), m_watchlist.join(QStringLiteral(", ")));
    Q_EMIT changed();
    return {};
}

QString TradingConfig::removeMarket(const QString &symbol)
{
    if (!m_watchlist.contains(symbol))
        return tr("%1 is not in the list.").arg(symbol);
    if (m_watchlist.size() <= 1)
        return tr("The last market cannot be removed.");
    m_watchlist.removeAll(symbol);
    store(marketKey(QStringLiteral("watchlist")), m_watchlist.join(QStringLiteral(", ")));
    if (m_symbol == symbol) {
        m_symbol = m_watchlist.first();
        store(marketKey(QStringLiteral("symbol")), m_symbol);
    }
    Q_EMIT changed();
    return {};
}

void TradingConfig::setInterval(const QString &interval)
{
    if (!intervals().contains(interval) || interval == m_interval)
        return;
    store(QStringLiteral("market/interval"), interval);
    m_interval = interval;
    Q_EMIT changed();
}

void TradingConfig::setAutoTrade(bool on)
{
    if (on == m_autoTrade)
        return;
    store(QStringLiteral("engine/auto_trade"), on);
    m_autoTrade = on;
    Q_EMIT changed();
}

void TradingConfig::setAutoAnalyze(bool on)
{
    if (on == m_autoAnalyze)
        return;
    store(QStringLiteral("engine/auto_analyze"), on);
    m_autoAnalyze = on;
    Q_EMIT changed();
}

void TradingConfig::setTeamReview(bool on)
{
    if (on == m_teamReview)
        return;
    store(QStringLiteral("engine/team_review"), on);
    m_teamReview = on;
    Q_EMIT changed();
}

QString TradingConfig::deskFastModel() const
{
    return m_deskFast.isEmpty() ? QLatin1String(kDeskFast) : m_deskFast;
}

QString TradingConfig::deskDeepModel() const
{
    return m_deskDeep.isEmpty() ? QLatin1String(kDeskDeep) : m_deskDeep;
}

QString TradingConfig::setDeskModel(const QString &which, const QString &model)
{
    const QString id = model.trimmed();
    if (!id.isEmpty() && !validModelId(id))
        return tr("That is not a model id.");
    if (which == QLatin1String("fast")) {
        store(QStringLiteral("desk/fast_model"), id);
        m_deskFast = id;
    } else if (which == QLatin1String("deep")) {
        store(QStringLiteral("desk/deep_model"), id);
        m_deskDeep = id;
    } else {
        return tr("Unknown desk model.");
    }
    Q_EMIT changed();
    return {};
}

Trading::Sbt::Settings TradingConfig::sbt() const
{
    Trading::Sbt::Settings s = m_sbt;
    s.minRewardRisk = m_risk.minRewardRisk; // one minimum for the whole desk
    return s;
}

bool TradingConfig::setSbt(const QString &name, const QVariant &value)
{
    if (name == QLatin1String("ai_confirm") || name == QLatin1String("require_pd_array")) {
        store(QStringLiteral("sbt/") + name, value.toBool());
    } else if (name == QLatin1String("zone")) {
        const QString zone = value.toString();
        if (zone != QLatin1String("wick") && zone != QLatin1String("body"))
            return false;
        store(QStringLiteral("sbt/zone"), zone);
    } else {
        struct Range { const char *name; double low; double high; };
        static const Range ranges[] = {{"swing_strength", 1, 5}, {"stop_buffer_atr", 0, 2},
                                       {"pd_distance_atr", 0.2, 5}, {"max_wait_candles", 10, 1000}};
        bool known = false;
        for (const Range &r : ranges) {
            if (name != QLatin1String(r.name))
                continue;
            const double v = value.toDouble();
            if (!std::isfinite(v) || v < r.low || v > r.high)
                return false;
            store(QStringLiteral("sbt/") + name, v);
            known = true;
        }
        if (!known)
            return false;
    }
    reload();
    Q_EMIT changed();
    return true;
}

void TradingConfig::setDeskFollowChart(bool on)
{
    if (on == m_deskFollow)
        return;
    store(QStringLiteral("desk/follow_chart"), on);
    m_deskFollow = on;
    Q_EMIT changed();
}

QString TradingConfig::setModelTimeoutSeconds(int seconds)
{
    if (seconds < kTimeoutMin || seconds > kTimeoutMax)
        return tr("Between %1 and %2 seconds.").arg(kTimeoutMin).arg(kTimeoutMax);
    if (seconds == m_modelTimeout)
        return {};
    store(QStringLiteral("nvidia/timeout_seconds"), seconds);
    m_modelTimeout = seconds;
    Q_EMIT changed();
    return {};
}

bool TradingConfig::setRisk(const QString &name, double value)
{
    // The same ranges reload() applies when it reads the file.
    struct Range { const char *name; double low; double high; bool integer; };
    static const Range ranges[] = {
        {"risk_per_trade_pct", 0.1, 5, false}, {"max_daily_loss_pct", 0.5, 20, false},
        {"max_drawdown_pct", 1, 50, false},    {"min_reward_risk", 1, 10, false},
        {"min_confidence", 30, 100, false},    {"min_agreeing", 1, 5, true},
        {"max_open_positions", 1, 10, true},   {"cooldown_minutes", 0, 1440, true},
    };
    for (const Range &range : ranges) {
        if (name != QLatin1String(range.name))
            continue;
        if (!std::isfinite(value) || value < range.low || value > range.high)
            return false;
        store(QStringLiteral("risk/") + name, range.integer ? std::floor(value) : value);
        reload();
        Q_EMIT changed();
        return true;
    }
    return false;
}

bool TradingConfig::setPaper(const QString &name, double value)
{
    const bool known = name == QLatin1String("starting_balance") || name == QLatin1String("fee_pct")
                       || name == QLatin1String("slippage_pct");
    if (!known || !std::isfinite(value))
        return false;
    const bool inRange = name == QLatin1String("starting_balance") ? value >= 100 && value <= 1e7
                                                                   : value >= 0 && value <= 1;
    if (!inRange)
        return false;
    store(QStringLiteral("paper/") + name, value);
    reload();
    Q_EMIT changed();
    return true;
}
