// SPDX-License-Identifier: GPL-3.0-or-later
#include "desk.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace Trading::Desk {

namespace {

std::string lower(const std::string &s)
{
    std::string out = s;
    for (char &c : out)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool isWordChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0;
}

// 'phrase' in 'text' (both lower case) as whole words.
bool has(const std::string &text, const std::string &phrase)
{
    size_t at = 0;
    while ((at = text.find(phrase, at)) != std::string::npos) {
        const bool startOk = at == 0 || !isWordChar(text[at - 1]);
        const size_t end = at + phrase.size();
        const bool endOk = end >= text.size() || !isWordChar(text[end]);
        if (startOk && endOk)
            return true;
        ++at;
    }
    return false;
}

bool hasAny(const std::string &text, std::initializer_list<const char *> phrases)
{
    for (const char *p : phrases) {
        if (has(text, p))
            return true;
    }
    return false;
}

// Whole words starting with 'prefix' ("consolidat" finds "consolidating").
bool hasPrefix(const std::string &text, const std::string &prefix)
{
    size_t at = 0;
    while ((at = text.find(prefix, at)) != std::string::npos) {
        if (at == 0 || !isWordChar(text[at - 1]))
            return true;
        ++at;
    }
    return false;
}

struct Token
{
    std::string raw;   // as written, punctuation around it removed
    std::string low;
    bool percentAfter = false; // "5 %" written with a space
};

std::vector<Token> tokenize(const std::string &text)
{
    std::vector<Token> out;
    std::string current;
    const auto flush = [&] {
        if (current.empty())
            return;
        // Punctuation around a word is not part of it; '$' in front of a
        // price and '%' after a number are.
        size_t b = 0, e = current.size();
        while (b < e && !isWordChar(current[b]) && current[b] != '$' && current[b] != '@' && current[b] != '~')
            ++b;
        while (e > b && !isWordChar(current[e - 1]) && current[e - 1] != '%')
            --e;
        if (e > b) {
            Token t;
            t.raw = current.substr(b, e - b);
            t.low = lower(t.raw);
            out.push_back(t);
        }
        current.clear();
    };
    for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c)))
            flush();
        else
            current += c;
    }
    flush();
    for (size_t i = 0; i + 1 < out.size(); ++i) {
        if (out[i + 1].low == "%" || out[i + 1].low == "percent" || out[i + 1].low == "pct")
            out[i].percentAfter = true;
    }
    return out;
}

const std::set<std::string> &cryptoBases()
{
    static const std::set<std::string> bases = {
        "btc", "eth", "sol", "xrp", "doge", "bnb", "ada", "avax", "ltc", "trx", "shib", "pepe", "sui", "matic",
        "pol", "inj", "tia", "wif", "bch", "xlm", "hbar", "fil", "icp", "aave", "mkr", "paxg", "xaut", "xbt",
        "link", "dot", "ton", "arb", "op", "near", "atom", "uni", "apt", "sei", "etc"};
    return bases;
}

// Bases that are also ordinary English words: taken only when written in
// capitals or with a quote currency ("NEAR", "linkusdt"), never "near 95k".
bool ambiguousBase(const std::string &base)
{
    static const std::set<std::string> words = {"link", "dot", "ton", "arb", "op", "near", "atom", "uni",
                                                "apt", "sei", "etc", "sol", "pol"};
    return words.count(base) > 0;
}

bool isCurrency(const std::string &c)
{
    static const std::set<std::string> codes = {"eur", "usd", "gbp", "jpy", "chf", "cad", "aud", "nzd",
                                                "xau", "xag", "cnh", "sek", "nok", "try", "zar", "mxn"};
    return codes.count(c) > 0;
}

std::string upper(const std::string &s)
{
    std::string out = s;
    for (char &c : out)
        c = char(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

bool allCaps(const std::string &s)
{
    bool letter = false;
    for (char c : s) {
        if (std::islower(static_cast<unsigned char>(c)))
            return false;
        letter = letter || std::isalpha(static_cast<unsigned char>(c));
    }
    return letter;
}

void addMention(std::vector<Mention> *out, const std::string &base, const std::string &quote, const std::string &text)
{
    for (const Mention &m : *out) {
        if (m.base == base && (m.quote == quote || quote.empty()))
            return;
    }
    out->push_back({base, quote, text});
}

void readMarkets(const std::vector<Token> &tokens, std::vector<Mention> *out)
{
    static const std::vector<std::pair<const char *, const char *>> names = {
        {"bitcoin", "BTC"}, {"ether", "ETH"}, {"ethereum", "ETH"}, {"solana", "SOL"}, {"ripple", "XRP"},
        {"dogecoin", "DOGE"}, {"cardano", "ADA"}, {"litecoin", "LTC"}, {"gold", "XAU"}, {"silver", "XAG"}};
    static const std::vector<std::tuple<const char *, const char *, const char *>> nicknames = {
        {"cable", "GBP", "USD"}, {"fiber", "EUR", "USD"}, {"loonie", "USD", "CAD"}, {"swissy", "USD", "CHF"},
        {"aussie", "AUD", "USD"}, {"euro", "EUR", "USD"}, {"yen", "USD", "JPY"}};
    static const std::vector<std::string> quotes = {"usdt", "usdc", "busd", "usd", "perp"};

    for (const Token &t : tokens) {
        const std::string &w = t.low;
        bool done = false;
        for (const auto &[name, base] : names) {
            if (w == name) {
                addMention(out, base, base == std::string("XAU") || base == std::string("XAG") ? "USD" : "",
                           t.raw);
                done = true;
            }
        }
        for (const auto &[name, base, quote] : nicknames) {
            if (w == name) {
                addMention(out, base, quote, t.raw);
                done = true;
            }
        }
        if (done)
            continue;
        // "btc/usdt", "eur/usd"
        const size_t slash = w.find('/');
        if (slash != std::string::npos) {
            const std::string a = w.substr(0, slash), b = w.substr(slash + 1);
            if ((cryptoBases().count(a) || isCurrency(a)) && (isCurrency(b) || b == "usdt" || b == "usdc"))
                addMention(out, upper(a == "xbt" ? std::string("btc") : a), upper(b), t.raw);
            continue;
        }
        if (cryptoBases().count(w)) {
            if (ambiguousBase(w) && !allCaps(t.raw))
                continue;
            std::string base = upper(w);
            if (base == "XBT")
                base = "BTC";
            if (base == "PAXG" || base == "XAUT") {
                addMention(out, "XAU", "USD", t.raw);
                continue;
            }
            addMention(out, base, "", t.raw);
            continue;
        }
        // "btcusdt", "ethusd", "xauusd", "eurusd"
        bool matched = false;
        for (const std::string &q : quotes) {
            if (w.size() > q.size() && w.compare(w.size() - q.size(), q.size(), q) == 0) {
                std::string base = w.substr(0, w.size() - q.size());
                if (cryptoBases().count(base) || isCurrency(base)) {
                    if (base == "xbt")
                        base = "btc";
                    if (base == "paxg" || base == "xaut")
                        base = "xau";
                    addMention(out, upper(base), q == "perp" ? "" : upper(q), t.raw);
                    matched = true;
                }
                break;
            }
        }
        if (matched)
            continue;
        if (w.size() == 6 && isCurrency(w.substr(0, 3)) && isCurrency(w.substr(3)) && w.substr(0, 3) != w.substr(3))
            addMention(out, upper(w.substr(0, 3)), upper(w.substr(3)), t.raw);
    }
}

bool inList(const std::string &w, std::initializer_list<const char *> list)
{
    for (const char *s : list) {
        if (w == s)
            return true;
    }
    return false;
}

const char *kindOf(const std::string &w)
{
    if (inList(w, {"support", "supports", "floor", "demand"}))
        return "support";
    if (inList(w, {"resistance", "ceiling", "supply"}))
        return "resistance";
    if (inList(w, {"stop", "sl", "stoploss", "stop-loss", "invalidation", "invalidated"}))
        return "stop";
    if (inList(w, {"target", "targets", "tp", "tp1", "tp2", "tp3", "profit", "objective"}))
        return "target";
    if (inList(w, {"entry", "enter", "entering", "fill", "filled"}))
        return "entry";
    return nullptr;
}

const char *relationOf(const std::string &w)
{
    if (inList(w, {"approaching", "approaches", "nearing", "near", "toward", "towards", "testing", "tests",
                   "test", "retest", "retesting", "heading", "eyeing"}))
        return "approaching";
    if (inList(w, {"above", "over", "reclaim", "reclaimed", "reclaiming"}))
        return "above";
    if (inList(w, {"below", "under", "beneath", "lost", "losing"}))
        return "below";
    if (inList(w, {"break", "breaks", "broke", "breaking", "breakout", "breakdown"}))
        return "break";
    if (inList(w, {"reject", "rejected", "rejecting", "rejection"}))
        return "reject";
    if (inList(w, {"at", "around", "@", "~", "hit", "hits", "reached", "reach", "tagged", "holding", "holds",
                   "held", "hold", "from", "to", "is", "touching", "touched", "between", "and"}))
        return "at";
    return nullptr;
}

// Words before a number that make it something other than a price.
bool notAPriceContext(const std::string &w)
{
    return inList(w, {"rsi", "ema", "sma", "ma", "macd", "period", "length", "stoch", "stochastic", "cci", "adx",
                      "leverage", "lev", "x", "in", "since", "year", "atr", "bb", "rank", "number", "#", "risk",
                      "risking", "r", "rr", "last", "next", "every", "only", "about"});
}

// Words after a number that make it something other than a price.
bool notAPriceAfter(const std::string &w)
{
    return inList(w, {"ema", "sma", "ma", "period", "x", "times", "year", "years", "day", "days", "hours", "hour",
                      "minutes", "minute", "min", "mins", "candles", "candle", "bars", "bar", "trades", "trade",
                      "lot", "lots", "contracts", "weeks", "week", "months", "month", "percent", "pct", "people",
                      "models", "analysts", "times", "seconds", "sec", "rsi", "r"});
}

// "at" style words count as a relation only right before the number; the
// stronger words count within three words.
bool strongRelation(const char *r)
{
    return r && std::string(r) != "at";
}

void readLevels(const std::vector<Token> &tokens, std::vector<LevelNote> *out)
{
    for (size_t i = 0; i < tokens.size(); ++i) {
        const Token &t = tokens[i];
        if (t.low.find('%') != std::string::npos || t.percentAfter)
            continue;
        // A range "94,500-95,000" is two levels.
        std::vector<std::string> parts;
        const size_t dash = t.raw.find_first_of("-");
        if (dash != std::string::npos && dash > 0 && dash + 1 < t.raw.size()) {
            parts.push_back(t.raw.substr(0, dash));
            parts.push_back(t.raw.substr(dash + 1));
        } else {
            parts.push_back(t.raw);
        }
        std::vector<double> values;
        bool formatted = false;
        for (const std::string &p : parts) {
            double v = 0;
            if (!parsePrice(p, &v))
                continue;
            values.push_back(v);
            const std::string lp = lower(p);
            formatted = formatted || p.find(',') != std::string::npos || lp.back() == 'k' || p.front() == '$';
        }
        if (values.empty() || values.size() != parts.size())
            continue;
        const std::string next1 = i + 1 < tokens.size() ? tokens[i + 1].low : "";
        if (notAPriceAfter(next1))
            continue;
        // "RSI is at 72", "the 200 EMA": an indicator word up to three words
        // back, with nothing in between that makes the number a price.
        bool other = false;
        for (size_t back = 1; back <= 3 && back <= i; ++back) {
            const std::string &w = tokens[i - back].low;
            if (kindOf(w) || strongRelation(relationOf(w)))
                break;
            if (notAPriceContext(w)) {
                other = true;
                break;
            }
        }
        if (other)
            continue;
        // A year ("in 2025") or a count is not a price; plain small numbers
        // need words that make them one.
        const char *kind = nullptr;
        const char *relation = nullptr;
        for (size_t back = 1; back <= 3 && back <= i; ++back) {
            const std::string &w = tokens[i - back].low;
            if (!kind)
                kind = kindOf(w);
            const char *r = relationOf(w);
            if (!relation && (back == 1 || strongRelation(r)))
                relation = r;
        }
        for (size_t ahead = 1; ahead <= 2 && i + ahead < tokens.size(); ++ahead) {
            if (!kind)
                kind = kindOf(tokens[i + ahead].low);
        }
        // "take profit at 98k": "profit" is the kind.
        if (!kind && !relation && !formatted)
            continue;
        // A bare "is 5" or "at 3" is too weak for a small whole number.
        if (!kind && relation && std::string(relation) == "at" && !formatted && values.front() < 10
            && t.raw.find('.') == std::string::npos)
            continue;
        for (double v : values) {
            LevelNote n;
            n.price = v;
            n.kind = kind ? kind : "level";
            n.relation = relation && std::string(relation) != "at" ? relation : (relation ? "at" : "");
            bool dup = false;
            for (const LevelNote &o : *out)
                dup = dup || std::fabs(o.price - v) <= v * 0.0005;
            if (!dup)
                out->push_back(n);
        }
    }
}

void addObservation(std::vector<Observation> *out, const std::string &topic, const std::string &text, Side side)
{
    for (const Observation &o : *out) {
        if (o.text == text)
            return;
    }
    out->push_back({topic, text, side});
}

// The number after a word, within three words ("rsi is at 72").
bool numberAfter(const std::vector<Token> &tokens, const std::string &word, double *value)
{
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].low != word)
            continue;
        for (size_t j = i + 1; j < tokens.size() && j <= i + 3; ++j) {
            std::string w = tokens[j].low;
            if (!w.empty() && w.back() == '%')
                w.pop_back();
            char *end = nullptr;
            const double v = std::strtod(w.c_str(), &end);
            if (end && *end == '\0' && !w.empty() && std::isdigit(static_cast<unsigned char>(w[0])))
                return *value = v, true;
        }
    }
    return false;
}

std::string number0(double v)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.0f", v);
    return buf;
}

void readObservations(const std::string &text, const std::vector<Token> &tokens, Reading *r)
{
    std::vector<Observation> &out = r->observations;
    const bool rsi = has(text, "rsi");
    const bool macd = has(text, "macd");
    const bool obv = has(text, "obv");

    // Divergences belong to the indicator they are seen on.
    const bool bearDiv = hasAny(text, {"bearish divergence", "bear divergence", "bearish div", "bear div",
                                       "negative divergence", "hidden bearish divergence"});
    const bool bullDiv = hasAny(text, {"bullish divergence", "bull divergence", "bullish div", "bull div",
                                       "positive divergence", "hidden bullish divergence"});
    const std::string on = rsi ? "RSI" : macd ? "MACD" : obv ? "OBV" : "";
    const std::string divTopic = rsi ? "rsi" : macd ? "macd" : "divergence";
    if (bearDiv)
        addObservation(&out, divTopic, (on.empty() ? "Bearish divergence" : on + " bearish divergence"), Side::Bearish);
    if (bullDiv)
        addObservation(&out, divTopic, (on.empty() ? "Bullish divergence" : on + " bullish divergence"), Side::Bullish);
    if (!bearDiv && !bullDiv && hasPrefix(text, "divergen"))
        addObservation(&out, divTopic, (on.empty() ? "Divergence" : on + " divergence") + std::string(" (direction not said)"),
                       Side::Neutral);

    if (rsi) {
        double v = 0;
        const bool value = numberAfter(tokens, "rsi", &v) && v >= 0 && v <= 100;
        if (has(text, "overbought") || (value && v >= 70))
            addObservation(&out, "rsi", value ? "RSI overbought (" + number0(v) + ")" : "RSI overbought", Side::Bearish);
        else if (has(text, "oversold") || (value && v <= 30))
            addObservation(&out, "rsi", value ? "RSI oversold (" + number0(v) + ")" : "RSI oversold", Side::Bullish);
        else if (value)
            addObservation(&out, "rsi", "RSI at " + number0(v), Side::Neutral);
        if (hasAny(text, {"rsi rising", "rsi is rising", "rsi climbing", "rsi turning up", "rsi curling up"}))
            addObservation(&out, "rsi", "RSI rising", Side::Bullish);
        if (hasAny(text, {"rsi falling", "rsi is falling", "rsi dropping", "rsi turning down", "rsi rolling over"}))
            addObservation(&out, "rsi", "RSI falling", Side::Bearish);
    } else {
        if (has(text, "overbought"))
            addObservation(&out, "sentiment", "Overbought", Side::Bearish);
        if (has(text, "oversold"))
            addObservation(&out, "sentiment", "Oversold", Side::Bullish);
    }

    if (macd) {
        if (hasAny(text, {"bullish cross", "bullish crossover", "crossed up", "cross up", "crossing up", "crossed above",
                          "flipped positive", "turned positive", "turning positive", "histogram rising",
                          "histogram growing", "golden cross"}))
            addObservation(&out, "macd", "MACD bullish cross", Side::Bullish);
        if (hasAny(text, {"bearish cross", "bearish crossover", "crossed down", "cross down", "crossing down",
                          "crossed below", "crossunder", "flipped negative", "turned negative", "turning negative",
                          "histogram falling", "histogram shrinking", "death cross"}))
            addObservation(&out, "macd", "MACD bearish cross", Side::Bearish);
    } else {
        if (has(text, "golden cross"))
            addObservation(&out, "moving averages", "Golden cross", Side::Bullish);
        if (has(text, "death cross"))
            addObservation(&out, "moving averages", "Death cross", Side::Bearish);
    }
    const bool ma = hasAny(text, {"ema", "sma", "ma", "moving average", "200", "50 ema", "200 ema"});
    if (ma && hasAny(text, {"above the 200", "above 200", "above the ema", "above the 200 ema", "above the moving average",
                            "above the ma", "above the 50", "reclaimed the ema"}))
        addObservation(&out, "moving averages", "Price above its moving average", Side::Bullish);
    if (ma && hasAny(text, {"below the 200", "below 200", "below the ema", "below the 200 ema", "below the moving average",
                            "below the ma", "below the 50", "lost the ema"}))
        addObservation(&out, "moving averages", "Price below its moving average", Side::Bearish);

    if (hasAny(text, {"bollinger", "bb"})) {
        if (has(text, "squeeze"))
            addObservation(&out, "bollinger", "Bollinger squeeze", Side::Neutral);
        if (has(text, "upper band"))
            addObservation(&out, "bollinger", "At the upper Bollinger band", Side::Neutral);
        if (has(text, "lower band"))
            addObservation(&out, "bollinger", "At the lower Bollinger band", Side::Neutral);
    }

    if (has(text, "volume")) {
        if (hasAny(text, {"rising", "increasing", "spike", "spiking", "high", "heavy", "surge", "surging", "big", "strong"}))
            addObservation(&out, "volume", "Volume rising", Side::Neutral);
        else if (hasAny(text, {"low", "declining", "falling", "drying", "weak", "thin", "fading"}))
            addObservation(&out, "volume", "Volume falling", Side::Neutral);
    }

    // Structure and chart patterns.
    if (hasAny(text, {"inverse head and shoulders", "inverse h&s", "inverted head and shoulders"}))
        addObservation(&out, "pattern", "Inverse head and shoulders", Side::Bullish);
    else if (hasAny(text, {"head and shoulders", "h&s"}))
        addObservation(&out, "pattern", "Head and shoulders", Side::Bearish);
    if (hasAny(text, {"double top", "triple top"}))
        addObservation(&out, "pattern", "Double top", Side::Bearish);
    if (hasAny(text, {"double bottom", "triple bottom"}))
        addObservation(&out, "pattern", "Double bottom", Side::Bullish);
    if (hasAny(text, {"bullish engulfing"}))
        addObservation(&out, "pattern", "Bullish engulfing candle", Side::Bullish);
    if (hasAny(text, {"bearish engulfing"}))
        addObservation(&out, "pattern", "Bearish engulfing candle", Side::Bearish);
    if (hasAny(text, {"shooting star"}))
        addObservation(&out, "pattern", "Shooting star candle", Side::Bearish);
    if (hasAny(text, {"hammer"}))
        addObservation(&out, "pattern", "Hammer candle", Side::Bullish);
    if (hasAny(text, {"higher highs", "higher lows", "higher high", "higher low"}))
        addObservation(&out, "structure", "Higher highs / higher lows", Side::Bullish);
    if (hasAny(text, {"lower highs", "lower lows", "lower high", "lower low"}))
        addObservation(&out, "structure", "Lower highs / lower lows", Side::Bearish);
    if (hasAny(text, {"breakout", "broke out", "breaking out", "broken out"}))
        addObservation(&out, "structure", "Breakout", Side::Bullish);
    if (hasAny(text, {"breakdown", "broke down", "breaking down", "broken down"}))
        addObservation(&out, "structure", "Breakdown", Side::Bearish);
    if (hasAny(text, {"rejected", "rejection", "rejecting", "got rejected"}))
        addObservation(&out, "structure", "Rejection", Side::Bearish);
    if (hasAny(text, {"bounce", "bounced", "bouncing"}))
        addObservation(&out, "structure", "Bounce", Side::Bullish);
    if (hasAny(text, {"liquidity sweep", "swept", "stop hunt", "liquidity grab"}))
        addObservation(&out, "structure", "Liquidity sweep", Side::Neutral);
    if (hasAny(text, {"order block", "ob"}))
        addObservation(&out, "structure", "Order block", Side::Neutral);
    if (hasAny(text, {"fair value gap", "fvg", "imbalance"}))
        addObservation(&out, "structure", "Fair value gap", Side::Neutral);
    if (hasAny(text, {"break of structure", "bos"}))
        addObservation(&out, "structure", "Break of structure", Side::Neutral);
    if (hasAny(text, {"change of character", "choch"}))
        addObservation(&out, "structure", "Change of character", Side::Neutral);
    if (hasPrefix(text, "consolidat") || hasAny(text, {"ranging", "range bound", "range-bound", "sideways", "chop", "choppy"}))
        addObservation(&out, "structure", "Ranging", Side::Neutral);

    if (hasAny(text, {"uptrend", "up trend", "trending up", "trend is up", "trend up"}))
        addObservation(&out, "trend", "Uptrend", Side::Bullish);
    if (hasAny(text, {"downtrend", "down trend", "trending down", "trend is down", "trend down"}))
        addObservation(&out, "trend", "Downtrend", Side::Bearish);

    if (has(text, "funding")) {
        if (hasAny(text, {"positive", "high", "elevated", "rising"}))
            addObservation(&out, "funding", "Funding positive", Side::Bearish);
        else if (hasAny(text, {"negative", "low", "falling"}))
            addObservation(&out, "funding", "Funding negative", Side::Bullish);
    }
    static const std::vector<std::pair<const char *, const char *>> macro = {
        {"cpi", "CPI release"}, {"fomc", "FOMC"}, {"fed", "The Fed"}, {"nfp", "Jobs report (NFP)"},
        {"jobs report", "Jobs report"}, {"rate cut", "Rate cut"}, {"rate hike", "Rate hike"},
        {"etf", "ETF flows"}, {"halving", "Halving"}, {"news", "News"}};
    for (const auto &[word, label] : macro) {
        if (has(text, word))
            addObservation(&out, "macro", label, Side::Neutral);
    }
}

// Which way the message's own claims lean.
Side readLean(const std::string &text, const std::vector<Observation> &observations)
{
    int score = 0;
    for (const Observation &o : observations)
        score += o.side == Side::Bullish ? 1 : o.side == Side::Bearish ? -1 : 0;
    // "bullish"/"bearish" said on their own, not as part of a phrase read above.
    const auto standalone = [&](const std::string &word) {
        int n = 0;
        size_t at = 0;
        while ((at = text.find(word, at)) != std::string::npos) {
            const size_t end = at + word.size();
            const bool start = at == 0 || !isWordChar(text[at - 1]);
            const bool stop = end >= text.size() || !isWordChar(text[end]);
            if (start && stop) {
                const std::string rest = text.substr(end, 16);
                const bool phrase = rest.find(" div") == 0 || rest.find(" cross") == 0 || rest.find(" engulf") == 0;
                n += phrase ? 0 : 1;
            }
            at = end;
        }
        return n;
    };
    score += standalone("bullish") - standalone("bearish");
    if (hasAny(text, {"pump", "pumping", "rally", "rallying", "moon", "mooning", "ripping", "squeeze higher"}))
        ++score;
    if (hasAny(text, {"dump", "dumping", "crash", "crashing", "selloff", "sell-off", "bleeding", "tanking"}))
        --score;
    return score > 0 ? Side::Bullish : score < 0 ? Side::Bearish : Side::None;
}

std::string readTradeSide(const std::vector<Token> &tokens)
{
    for (size_t i = 0; i < tokens.size(); ++i) {
        const std::string &w = tokens[i].low;
        const std::string prev = i > 0 ? tokens[i - 1].low : "";
        const std::string next = i + 1 < tokens.size() ? tokens[i + 1].low : "";
        if (inList(w, {"short", "shorting", "shorts", "sell", "selling"})) {
            if (inList(next, {"term", "time", "squeeze", "while"}) || inList(prev, {"in", "the"}))
                continue;
            return "short";
        }
        if (inList(w, {"long", "longing", "longs", "buy", "buying"})) {
            if (inList(prev, {"how", "so", "too", "as", "very", "for", "the"}) || inList(next, {"term", "time", "ago", "run", "while"}))
                continue;
            return "long";
        }
    }
    return "";
}

bool readQuestion(const std::string &raw, const std::string &text, const std::vector<Token> &tokens)
{
    if (raw.find('?') != std::string::npos)
        return true;
    if (tokens.empty())
        return false;
    const std::string &first = tokens.front().low;
    if (inList(first, {"what", "what's", "whats", "why", "how", "when", "where", "which", "who", "should", "shall",
                       "would", "could", "can", "do", "does", "did", "is", "are", "will", "any", "thoughts",
                       "analyze", "analyse", "explain", "compare", "check", "give", "tell", "show", "summarize",
                       "summarise", "plan", "rate", "evaluate", "assess", "review"}))
        return true;
    if (hasAny(text, {"what do you think", "your view", "your take", "your opinion", "thoughts on", "should i",
                      "can you", "could you", "give me", "tell me", "help me", "let me know", "advise"}))
        return true;
    // Roman Urdu / Hindi, as the desk's owner also writes.
    return hasAny(text, {"kya", "kia", "batao", "btao", "bataen", "bataein", "bataiye", "chahiye", "chaiye",
                         "kaisa", "kaise", "kab", "kahan", "kyun", "kyon", "lagta"});
}

bool readTradeDecision(const std::string &text)
{
    return hasAny(text, {"enter", "entering", "entry", "short", "shorting", "long", "longing", "buy", "sell",
                         "position", "trade", "open", "close", "exit", "take profit", "tp", "sl", "stop loss",
                         "stop", "hold", "add", "scale in", "go in", "get in", "lena", "lun", "loon", "bechna"});
}

std::vector<std::string> readFacts(const std::string &raw, const std::string &text, bool question, bool decision)
{
    std::vector<std::string> facts;
    if (!question || decision || raw.size() > 120)
        return facts;
    if (hasAny(text, {"think", "should", "opinion", "view", "thoughts", "good", "bad", "better", "expect", "predict",
                      "why", "explain", "worth", "likely", "probability", "chance", "analyze", "analyse", "plan",
                      "lagta", "chahiye"})
        || hasPrefix(text, "analy"))
        return facts;
    if (hasAny(text, {"price", "trading at", "how much", "where is", "where's", "quote", "rate", "worth", "kitna",
                      "kitne", "kitni"}))
        facts.push_back("price");
    if (has(text, "rsi"))
        facts.push_back("rsi");
    if (has(text, "macd"))
        facts.push_back("macd");
    if (has(text, "trend"))
        facts.push_back("trend");
    if (hasAny(text, {"signal", "team", "analysts", "consensus"}))
        facts.push_back("signal");
    if (hasAny(text, {"position", "my trade", "pnl", "p&l", "p/l", "profit"}))
        facts.push_back("position");
    if (hasAny(text, {"levels", "my levels", "support", "resistance"}))
        facts.push_back("levels");
    return facts;
}

void trimTo(std::vector<Entry> *v, size_t max)
{
    if (v->size() > max)
        v->erase(v->begin(), v->begin() + long(v->size() - max));
}

const char *levelWord(const std::string &kind)
{
    if (kind == "support")
        return "support";
    if (kind == "resistance")
        return "resistance";
    if (kind == "entry")
        return "entry";
    if (kind == "stop")
        return "stop";
    if (kind == "target")
        return "target";
    return "level";
}

int kindRank(const std::string &kind)
{
    return kind == "level" || kind.empty() ? 0 : 1;
}

} // namespace

const char *sideName(Side side)
{
    switch (side) {
    case Side::Bullish: return "bullish";
    case Side::Bearish: return "bearish";
    case Side::Neutral: return "neutral";
    case Side::None: break;
    }
    return "";
}

Side sideFromName(const std::string &name)
{
    const std::string n = lower(name);
    if (n == "bullish" || n == "bull" || n == "long" || n == "up" || n == "buy")
        return Side::Bullish;
    if (n == "bearish" || n == "bear" || n == "short" || n == "down" || n == "sell")
        return Side::Bearish;
    if (n == "neutral" || n == "flat" || n == "hold" || n == "mixed" || n == "range" || n == "sideways")
        return Side::Neutral;
    return Side::None;
}

const char *sourceName(Source source)
{
    switch (source) {
    case Source::You: return "you";
    case Source::Model: return "model";
    case Source::Market: return "market";
    case Source::Team: return "team";
    }
    return "you";
}

Source sourceFromName(const std::string &name)
{
    if (name == "model")
        return Source::Model;
    if (name == "market")
        return Source::Market;
    if (name == "team")
        return Source::Team;
    return Source::You;
}

bool parsePrice(const std::string &token, double *value)
{
    std::string s = token;
    while (!s.empty() && (s.front() == '$' || s.front() == '@' || s.front() == '~'))
        s.erase(s.begin());
    if (s.empty() || !std::isdigit(static_cast<unsigned char>(s.front())))
        return false;
    double scale = 1;
    const char last = char(std::tolower(static_cast<unsigned char>(s.back())));
    if (last == 'k') {
        scale = 1000;
        s.pop_back();
    }
    // Thousands separators: "95,000" and "1,234,567.5", never "95,0".
    std::string digits;
    int sinceComma = -1;
    bool decimal = false;
    for (char c : s) {
        if (c == ',') {
            if (decimal || (sinceComma >= 0 && sinceComma != 3))
                return false;
            sinceComma = 0;
            continue;
        }
        if (c == '.') {
            if (decimal || (sinceComma >= 0 && sinceComma != 3))
                return false;
            decimal = true;
            sinceComma = -1;
            digits += c;
            continue;
        }
        if (!std::isdigit(static_cast<unsigned char>(c)))
            return false;
        if (sinceComma >= 0)
            ++sinceComma;
        digits += c;
    }
    if (sinceComma >= 0 && sinceComma != 3)
        return false;
    if (digits.empty() || digits.back() == '.')
        return false;
    char *end = nullptr;
    const double v = std::strtod(digits.c_str(), &end) * scale;
    if (!end || *end != '\0' || !std::isfinite(v) || v <= 0)
        return false;
    *value = v;
    return true;
}

Reading read(const std::string &raw)
{
    Reading r;
    const std::string text = lower(raw);
    const std::vector<Token> tokens = tokenize(raw);
    readMarkets(tokens, &r.markets);
    readLevels(tokens, &r.levels);
    readObservations(text, tokens, &r);
    r.lean = readLean(text, r.observations);
    r.tradeSide = readTradeSide(tokens);
    r.question = readQuestion(raw, text, tokens);
    r.tradeDecision = r.question && readTradeDecision(text);
    r.facts = readFacts(raw, text, r.question, r.tradeDecision);
    return r;
}

std::string formatPrice(double price)
{
    if (!std::isfinite(price))
        return "";
    char buf[64];
    if (std::fabs(price) >= 1000) {
        std::snprintf(buf, sizeof buf, "%.2f", price);
    } else if (std::fabs(price) >= 1) {
        std::snprintf(buf, sizeof buf, "%.5f", price);
    } else {
        std::snprintf(buf, sizeof buf, "%.6f", price);
    }
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0')
            s.pop_back();
        if (!s.empty() && s.back() == '.')
            s.pop_back();
    }
    // Thousands separators in the whole part.
    const bool negative = !s.empty() && s.front() == '-';
    const size_t start = negative ? 1 : 0;
    size_t dot = s.find('.');
    if (dot == std::string::npos)
        dot = s.size();
    std::string whole = s.substr(start, dot - start);
    std::string grouped;
    for (size_t i = 0; i < whole.size(); ++i) {
        if (i > 0 && (whole.size() - i) % 3 == 0)
            grouped += ',';
        grouped += whole[i];
    }
    return (negative ? "-" : "") + grouped + s.substr(dot);
}

double planRewardRisk(const Plan &plan)
{
    if (plan.entry <= 0 || plan.stop <= 0 || plan.target <= 0)
        return 0;
    if (plan.side == "long" && plan.stop < plan.entry && plan.entry < plan.target)
        return (plan.target - plan.entry) / (plan.entry - plan.stop);
    if (plan.side == "short" && plan.target < plan.entry && plan.entry < plan.stop)
        return (plan.entry - plan.target) / (plan.stop - plan.entry);
    return 0;
}

std::string planProblem(const Plan &plan)
{
    if (plan.side.empty())
        return "no direction yet";
    if (plan.entry <= 0)
        return "no entry yet";
    if (plan.stop <= 0)
        return "no stop yet";
    if (plan.target <= 0)
        return "no target yet";
    if (plan.side == "long") {
        if (plan.stop >= plan.entry)
            return "the stop of a long must be below the entry";
        if (plan.target <= plan.entry)
            return "the target of a long must be above the entry";
    } else {
        if (plan.stop <= plan.entry)
            return "the stop of a short must be above the entry";
        if (plan.target >= plan.entry)
            return "the target of a short must be below the entry";
    }
    return "";
}

Lean leanOf(const Book &book)
{
    Lean lean;
    for (const Entry &e : book.entries) {
        if (e.source != Source::You)
            continue;
        lean.bullish += e.side == Side::Bullish;
        lean.bearish += e.side == Side::Bearish;
    }
    lean.side = lean.bullish > lean.bearish ? Side::Bullish
              : lean.bearish > lean.bullish ? Side::Bearish
              : (lean.bullish > 0 ? Side::Neutral : Side::None);
    return lean;
}

// ---- DeskState --------------------------------------------------------------------

Book &DeskState::book(const std::string &market)
{
    Book &b = m_books[market];
    b.market = market;
    return b;
}

const Book *DeskState::find(const std::string &market) const
{
    const auto it = m_books.find(market);
    return it == m_books.end() ? nullptr : &it->second;
}

std::vector<Change> DeskState::focus(const std::string &market, const std::string &name, long long now)
{
    std::vector<Change> changes;
    Book &b = book(market);
    if (!name.empty())
        b.name = name;
    if (market == m_current)
        return changes;
    // What was said before any market was named belongs to the first one named.
    if (m_current.empty()) {
        const auto loose = m_books.find("");
        if (loose != m_books.end() && !market.empty()) {
            for (Entry &e : loose->second.entries)
                b.entries.push_back(e);
            for (Level &l : loose->second.levels)
                b.levels.push_back(l);
            if (!loose->second.plan.side.empty() && b.plan.side.empty())
                b.plan = loose->second.plan;
            m_books.erase(loose);
        }
    }
    m_current = market;
    changes.push_back({now, market, "market", "Now discussing " + (name.empty() ? market : name), Source::You});
    record(changes);
    return changes;
}

bool DeskState::addEntry(const std::string &market, Entry entry, std::vector<Change> *changes)
{
    Book &b = book(market);
    for (Entry &e : b.entries) {
        if (e.topic == entry.topic && lower(e.text) == lower(entry.text)) {
            e.time = entry.time;
            e.message = entry.message ? entry.message : e.message;
            if (e.source == Source::You && entry.source != Source::You)
                return false; // the person's own word stays theirs
            e.by = entry.by;
            return false;
        }
    }
    b.entries.push_back(entry);
    trimTo(&b.entries, kMaxEntries);
    b.updated = entry.time;
    if (changes) {
        const char *s = sideName(entry.side);
        changes->push_back({entry.time, market, "observation",
                            entry.text + (*s && entry.side != Side::Neutral ? std::string(" (") + s + ")" : ""),
                            entry.source});
    }
    return true;
}

bool DeskState::addLevel(const std::string &market, Level level, std::vector<Change> *changes)
{
    Book &b = book(market);
    for (Level &l : b.levels) {
        if (std::fabs(l.price - level.price) <= level.price * 0.0005) {
            l.time = level.time;
            if (!level.relation.empty())
                l.relation = level.relation;
            if (kindRank(level.kind) > kindRank(l.kind) || (kindRank(level.kind) == 1 && level.kind != l.kind)) {
                l.kind = level.kind;
                if (changes)
                    changes->push_back({level.time, market, "level",
                                        formatPrice(level.price) + " is now " + levelWord(level.kind), level.source});
                return true;
            }
            return false;
        }
    }
    b.levels.push_back(level);
    if (b.levels.size() > kMaxLevels) {
        const auto oldest = std::min_element(b.levels.begin(), b.levels.end(),
                                             [](const Level &x, const Level &y) { return x.time < y.time; });
        b.levels.erase(oldest);
    }
    std::sort(b.levels.begin(), b.levels.end(), [](const Level &x, const Level &y) { return x.price > y.price; });
    b.updated = level.time;
    if (changes)
        changes->push_back({level.time, market, "level",
                            std::string(levelWord(level.kind)) + " " + formatPrice(level.price)
                                + (level.relation.empty() || level.relation == "at" ? "" : " (" + level.relation + ")"),
                            level.source});
    return true;
}

void DeskState::setPlan(const std::string &market, const Plan &plan, std::vector<Change> *changes)
{
    Book &b = book(market);
    Plan merged = b.plan;
    bool changed = false;
    if (!plan.side.empty() && plan.side != merged.side) {
        merged.side = plan.side;
        changed = true;
    }
    const auto take = [&](double v, double *field) {
        if (v > 0 && std::fabs(v - *field) > v * 1e-9) {
            *field = v;
            changed = true;
        }
    };
    take(plan.entry, &merged.entry);
    take(plan.stop, &merged.stop);
    take(plan.target, &merged.target);
    if (!plan.note.empty() && plan.note != merged.note) {
        merged.note = plan.note;
        changed = true;
    }
    if (!changed)
        return;
    merged.source = plan.source;
    merged.by = plan.by;
    merged.time = plan.time;
    b.plan = merged;
    b.updated = plan.time;
    if (changes) {
        std::string text = merged.side.empty() ? std::string("Plan") : (merged.side == "long" ? "Long" : "Short");
        if (merged.entry > 0)
            text += " entry " + formatPrice(merged.entry);
        if (merged.stop > 0)
            text += ", stop " + formatPrice(merged.stop);
        if (merged.target > 0)
            text += ", target " + formatPrice(merged.target);
        const double rr = planRewardRisk(merged);
        if (rr > 0) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.2f", rr);
            text += std::string(", reward/risk ") + buf;
        }
        changes->push_back({plan.time, market, "plan", text, plan.source});
    }
}

void DeskState::setBias(const std::string &market, const Bias &bias, std::vector<Change> *changes)
{
    Book &b = book(market);
    const bool changed = b.bias.side != bias.side;
    b.bias = bias;
    b.updated = bias.time;
    if (changed && changes) {
        std::string text = "Bias ";
        text += bias.side == Side::None ? "unclear" : sideName(bias.side);
        if (!bias.by.empty())
            text += " (" + bias.by + ")";
        changes->push_back({bias.time, market, "bias", text, bias.source});
    }
}

void DeskState::addConclusion(const std::string &market, Entry conclusion, std::vector<Change> *changes)
{
    Book &b = book(market);
    if (!b.conclusions.empty() && b.conclusions.back().text == conclusion.text)
        return;
    b.conclusions.push_back(conclusion);
    trimTo(&b.conclusions, kMaxConclusions);
    b.updated = conclusion.time;
    if (changes)
        changes->push_back({conclusion.time, market, "conclusion", conclusion.text, conclusion.source});
}

std::vector<Change> DeskState::apply(const Reading &reading, long long message, long long now)
{
    std::vector<Change> changes;
    const std::string market = m_current;
    for (const Observation &o : reading.observations) {
        Entry e;
        e.topic = o.topic;
        e.text = o.text;
        e.side = o.side;
        e.source = Source::You;
        e.message = message;
        e.time = now;
        addEntry(market, e, &changes);
    }
    Plan plan;
    plan.source = Source::You;
    plan.time = now;
    for (const LevelNote &n : reading.levels) {
        Level l;
        l.price = n.price;
        l.kind = n.kind;
        l.relation = n.relation;
        l.source = Source::You;
        l.message = message;
        l.time = now;
        addLevel(market, l, &changes);
        if (n.kind == "entry")
            plan.entry = n.price;
        else if (n.kind == "stop")
            plan.stop = n.price;
        else if (n.kind == "target")
            plan.target = n.price;
    }
    if (!reading.tradeSide.empty())
        plan.side = reading.tradeSide;
    if (!plan.side.empty() || plan.entry > 0 || plan.stop > 0 || plan.target > 0)
        setPlan(market, plan, &changes);
    record(changes);
    return changes;
}

std::vector<Change> DeskState::priceUpdate(const std::string &market, double price, double nearPct, long long now)
{
    std::vector<Change> changes;
    const auto it = m_books.find(market);
    if (it == m_books.end() || !(price > 0) || !std::isfinite(price))
        return changes;
    Book &b = it->second;
    for (Level &l : b.levels) {
        const double distance = (price - l.price) / l.price * 100.0;
        const LevelState now_state = price >= l.price ? LevelState::Above : LevelState::Below;
        const bool nearNow = std::fabs(distance) <= nearPct;
        const std::string what = std::string(levelWord(l.kind)) + " " + formatPrice(l.price);
        char pct[32];
        std::snprintf(pct, sizeof pct, "%.2f", std::fabs(distance));
        if (l.state != LevelState::Unknown && now_state != l.state) {
            changes.push_back({now, market, "price",
                               (now_state == LevelState::Above ? "Price crossed above " : "Price crossed below ") + what
                                   + " (live " + formatPrice(price) + ")",
                               Source::Market});
            l.near = nearNow;
        } else if (nearNow && !l.near) {
            changes.push_back({now, market, "price",
                               "Price is " + std::string(pct) + " % " + (distance >= 0 ? "above " : "below ") + what
                                   + " (live " + formatPrice(price) + ")",
                               Source::Market});
            l.near = true;
        } else if (std::fabs(distance) > 2 * nearPct) {
            l.near = false;
        }
        l.state = now_state;
        l.distancePct = distance;
        l.hasDistance = true;
    }
    record(changes);
    return changes;
}

void DeskState::record(const std::vector<Change> &changes)
{
    if (changes.empty())
        return;
    for (const Change &c : changes)
        m_changes.push_back(c);
    if (m_changes.size() > kMaxChanges)
        m_changes.erase(m_changes.begin(), m_changes.begin() + long(m_changes.size() - kMaxChanges));
    ++m_revision;
}

void DeskState::restore(const std::string &current, std::map<std::string, Book> books, std::vector<Change> changes,
                        const std::string &summary)
{
    m_current = current;
    m_books = std::move(books);
    for (auto &[key, b] : m_books)
        b.market = key;
    m_changes = std::move(changes);
    if (m_changes.size() > kMaxChanges)
        m_changes.erase(m_changes.begin(), m_changes.begin() + long(m_changes.size() - kMaxChanges));
    m_summary = summary;
    ++m_revision;
}

void DeskState::clear()
{
    m_current.clear();
    m_books.clear();
    m_changes.clear();
    m_summary.clear();
    ++m_revision;
}

} // namespace Trading::Desk
