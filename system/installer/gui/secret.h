// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QString>

#include <cstring>

// Overwrite a buffer in a way the optimiser may not remove.
inline void wipeMemory(void *data, size_t size)
{
    if (data && size > 0)
        explicit_bzero(data, size);
}

// Wipes the bytes of a byte array that is not shared with another owner.
inline void wipeBytes(QByteArray &bytes)
{
    if (!bytes.isEmpty())
        wipeMemory(bytes.data(), size_t(bytes.capacity() > bytes.size() ? bytes.capacity() : bytes.size()));
    bytes.clear();
}

// A password or passphrase held by the installer between the moment the user
// typed it and the moment it is written to the engine's standard input.
//
// The bytes (UTF-8) live in one unshared buffer that is overwritten when the
// secret is replaced, cleared or destroyed. It is never copied into a
// property, a log line, a file or a command line. What this class cannot
// reach are the copies Qt itself makes while the text travels from the
// QML text field to here (see README.md, "Secrets").
class Secret
{
public:
    Secret() = default;
    ~Secret() { clear(); }
    Secret(const Secret &) = delete;
    Secret &operator=(const Secret &) = delete;

    void set(const QString &text)
    {
        clear();
        m_bytes = text.toUtf8();
        m_codePoints = codePoints(text);
    }
    // Counted in place: no second copy of the text is made.
    static int codePoints(const QString &text)
    {
        int count = 0;
        for (qsizetype i = 0; i < text.size(); ++i) {
            if (text.at(i).isHighSurrogate() && i + 1 < text.size() && text.at(i + 1).isLowSurrogate())
                ++i;
            ++count;
        }
        return count;
    }
    void clear()
    {
        wipeBytes(m_bytes);
        m_codePoints = 0;
    }
    bool isEmpty() const { return m_bytes.isEmpty(); }
    // Number of Unicode code points — what the engine's len() counts.
    int length() const { return m_codePoints; }
    bool hasLineBreak() const { return m_bytes.contains('\n') || m_bytes.contains('\r'); }
    // Only for writing into the engine's standard input document.
    const QByteArray &utf8() const { return m_bytes; }

private:
    QByteArray m_bytes;
    int m_codePoints = 0;
};
