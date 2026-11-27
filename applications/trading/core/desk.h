// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <map>
#include <string>
#include <vector>

// The desk's understanding of a conversation about the markets, in plain
// C++17 (no Qt) so that every rule is unit-tested with only a compiler.
//
// Two parts:
//
// * read(): what one message says, found at once and without a model: the
//   markets it names, the price levels, the indicator and chart observations,
//   which way it leans, the trade being weighed, and whether it asks
//   something. It is deliberately cautious: a number becomes a price level
//   only when the words around it say so.
//
// * DeskState: the analysis the conversation builds up, one Book per market,
//   updated field by field as messages, a model's reading of them, the live
//   market and the analysis team add to it. Every entry says who it came from
//   (Source), so what the person said is never presented as what the market
//   or a model measured.
namespace Trading::Desk {

enum class Side { None, Bullish, Bearish, Neutral };
const char *sideName(Side side); // "" | "bullish" | "bearish" | "neutral"
Side sideFromName(const std::string &name);

// Who put an entry into the analysis.
enum class Source { You, Model, Market, Team };
const char *sourceName(Source source); // "you" | "model" | "market" | "team"
Source sourceFromName(const std::string &name);

// ---- reading one message ---------------------------------------------------------

struct Mention
{
    std::string base;  // "BTC", "ETH", "XAU", "EUR", …
    std::string quote; // "USD", "USDT", … or "" when the message did not say
    std::string text;  // as written
};

struct LevelNote
{
    double price = 0;
    std::string kind;     // "support" | "resistance" | "entry" | "stop" | "target" | "level"
    std::string relation; // "approaching" | "above" | "below" | "break" | "reject" | "at" | ""
};

struct Observation
{
    std::string topic; // "rsi" | "macd" | "moving averages" | "bollinger" | "volume" | "divergence"
                       // | "structure" | "pattern" | "trend" | "macro" | "funding" | "sentiment"
    std::string text;  // short: "RSI bearish divergence"
    Side side = Side::None;
};

struct Reading
{
    std::vector<Mention> markets;
    std::vector<LevelNote> levels;
    std::vector<Observation> observations;
    Side lean = Side::None;   // which way the message's own claims lean (not the trade it asks about)
    std::string tradeSide;    // "long" | "short": a trade the person is weighing
    bool question = false;    // asks or requests something: wants an answer
    bool tradeDecision = false; // asks whether to enter, exit or hold
    // Live facts asked for and nothing more ("what is the price of BTC?"):
    // "price" | "rsi" | "macd" | "trend" | "signal" | "position" | "levels".
    // Empty when the question wants an opinion.
    std::vector<std::string> facts;
};

Reading read(const std::string &text);

// "95,000" "95k" "1.0850" "$94.5K" -> the number; false when the text is not one.
bool parsePrice(const std::string &token, double *value);

// ---- the analysis ------------------------------------------------------------------

struct Entry
{
    std::string topic;
    std::string text;
    Side side = Side::None;
    Source source = Source::You;
    std::string by;        // the model's name for a Model entry, else ""
    long long message = 0; // the message it came from, 0 for none
    long long time = 0;    // ms since the epoch
};

enum class LevelState { Unknown, Above, Below }; // where the live price is

struct Level
{
    double price = 0;
    std::string kind;
    std::string relation;
    Source source = Source::You;
    long long message = 0;
    long long time = 0;
    LevelState state = LevelState::Unknown;
    bool near = false;        // the live price is close to it now
    double distancePct = 0;   // (price - level) / level * 100, at the last price
    bool hasDistance = false;
    long long crossedAt = 0;  // when a crossing was last reported
};

struct Plan
{
    std::string side;  // "long" | "short" | ""
    double entry = 0;  // 0 = not given
    double stop = 0;
    double target = 0;
    std::string note;
    Source source = Source::You;
    std::string by;
    long long time = 0;
};
// Reward per unit of risk of the plan, 0 when side, entry, stop and target do
// not make a valid trade (a stop on the winning side, a target behind the entry).
double planRewardRisk(const Plan &plan);
// Why planRewardRisk() is 0: "" when it is not.
std::string planProblem(const Plan &plan);

struct Bias
{
    Side side = Side::None;
    Source source = Source::You;
    std::string by;   // model name, or ""
    std::string why;
    long long time = 0;
};

struct Change
{
    long long time = 0;
    std::string market; // "" when it is about the conversation as a whole
    std::string field;  // "market" | "bias" | "observation" | "level" | "plan" | "conclusion" | "price" | "signal"
    std::string text;
    Source source = Source::You;
};

// Everything known about one market.
struct Book
{
    std::string market;          // the market's symbol ("BTCUSDT"), "" for "not named yet"
    std::string name;            // "BTC/USDT"
    std::vector<Entry> entries;  // observations, newest last
    std::vector<Level> levels;   // by price
    Plan plan;
    Bias bias;                   // the assessed bias (a model's or the team's), when there is one
    std::vector<Entry> conclusions; // newest last
    long long updated = 0;
};

// The conversation's lean from what was said about a market: bullish minus
// bearish entries the person (or a model reading their messages) gave.
struct Lean
{
    Side side = Side::None;
    int bullish = 0;
    int bearish = 0;
};
Lean leanOf(const Book &book);

class DeskState
{
public:
    static constexpr size_t kMaxEntries = 30;
    static constexpr size_t kMaxLevels = 12;
    static constexpr size_t kMaxConclusions = 8;
    static constexpr size_t kMaxChanges = 60;

    // The market the conversation is about now ("" until one is named).
    const std::string &current() const { return m_current; }
    // Switch to a market; a change is recorded when it is another one.
    std::vector<Change> focus(const std::string &market, const std::string &name, long long now);

    Book &book(const std::string &market);
    const Book *find(const std::string &market) const;
    const std::map<std::string, Book> &books() const { return m_books; }
    Book &currentBook() { return book(m_current); }

    // What one message adds to the current market's book. Same entries again
    // (same topic and text, or a level within 0.05 %) refresh the old ones.
    std::vector<Change> apply(const Reading &reading, long long message, long long now);

    // Single updates, for a model's reading, the market and the team.
    bool addEntry(const std::string &market, Entry entry, std::vector<Change> *changes);
    bool addLevel(const std::string &market, Level level, std::vector<Change> *changes);
    void setPlan(const std::string &market, const Plan &plan, std::vector<Change> *changes);
    void setBias(const std::string &market, const Bias &bias, std::vector<Change> *changes);
    void addConclusion(const std::string &market, Entry conclusion, std::vector<Change> *changes);

    // The live price of a market against its levels. A change is recorded
    // when the price comes within 'nearPct' of a level (once, until it has
    // moved away twice as far) and when it crosses one: only once it is
    // clearly past it (by 'nearPct' / 2), and at most once a minute per level,
    // so a price hovering on a level is not reported again and again.
    std::vector<Change> priceUpdate(const std::string &market, double price, double nearPct, long long now);

    // A running summary of the conversation, kept by a model.
    const std::string &summary() const { return m_summary; }
    void setSummary(const std::string &summary) { m_summary = summary; }

    void record(const std::vector<Change> &changes);
    const std::vector<Change> &changes() const { return m_changes; }
    long long revision() const { return m_revision; }

    // For loading saved state.
    void restore(const std::string &current, std::map<std::string, Book> books, std::vector<Change> changes,
                 const std::string &summary);
    void clear();

private:
    std::string m_current;
    std::map<std::string, Book> m_books;
    std::vector<Change> m_changes;
    std::string m_summary;
    long long m_revision = 0;
};

// "95000" -> "95,000", "1.0982" -> "1.0982": a price as people write it.
std::string formatPrice(double price);

} // namespace Trading::Desk
