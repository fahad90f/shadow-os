// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QStringList>

// Text for speech: turns the assistant's Markdown into plain sentences a TTS
// engine can read aloud, incrementally while a reply is still streaming in.
//
// Why this exists: the model answers in Markdown, and a speech engine reads
// whatever it is given — "asterisk asterisk", "hash hash", every character of
// a URL, a whole code listing. Replies are also spoken while they stream, so
// the text has to be cut into complete sentences as soon as each one is
// known, without cutting through a code span, a link or an abbreviation.
//
// Rules (shadow voice v2 spec §2.4):
//   * fenced code blocks          → "(code omitted)"
//   * inline code                 → kept, as words
//   * [text](url), ![alt](url)    → text / alt
//   * bare URLs, <autolinks>      → "a link"
//   * tables                      → skipped
//   * emphasis, heading, list and quote markers, HTML tags → removed
// Headings and list items become sentences of their own (a full stop is
// added when they have no closing punctuation, so the voice pauses).
namespace SpeechText {

// Inline-level cleanup of one line of Markdown (no block structure).
QString stripInline(const QString &line);

// The whole Markdown text as speakable sentences, joined by single spaces.
QString forSpeech(const QString &markdown);

} // namespace SpeechText

// Incremental sentence splitter for a streamed Markdown reply.
//
//   SentenceStream s;
//   for (chunk : deltas) speakAll(s.push(chunk));  // complete sentences only
//   speakAll(s.flush());                           // the rest, at segment end
//
// push() never returns half a sentence: text is held back until a sentence
// end is seen (or the paragraph/line ends, or the text grows too long to wait
// for one), so each returned string can be synthesised on its own.
class SentenceStream
{
public:
    // Feed the next streamed chunk; returns the sentences that are complete now.
    QStringList push(const QString &chunk);
    // The reply segment ended (or a tool call follows): everything still held
    // back, as sentences. Resets the block state (an unterminated code fence
    // does not swallow the next segment).
    QStringList flush();
    void reset();
    // True when nothing is held back.
    bool isEmpty() const { return m_line.isEmpty() && m_para.isEmpty(); }

private:
    void completeLine(const QString &line, QStringList &out);
    void earlySentences(QStringList &out);
    void flushParagraph(QStringList &out, bool finalPunctuation);
    void takeSentences(QStringList &out, bool endIsBoundary = false);
    void appendPara(const QString &text);

    QString m_line;              // raw text of the current, unfinished line
    bool m_lineContinues = false; // m_line continues a line whose start was already classified
    int m_lineKind = 0;          // block kind of that line (LineKind, see the .cpp)
    QString m_para;              // speakable text of the paragraph not yet returned
    bool m_inFence = false;      // inside a ``` / ~~~ code block
    QChar m_fenceChar;           // '`' or '~'
    int m_fenceLen = 0;          // length of the opening fence
};
