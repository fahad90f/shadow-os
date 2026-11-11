// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QString>

// Incremental splitter for a packet capture arriving as a byte stream, in
// either of the two standard file formats:
//
//   pcap     24-byte file header, then per packet a 16-byte record header and
//            the packet bytes (what `tcpdump -w -` writes)
//   pcapng   a sequence of length-prefixed blocks: section header, interface
//            descriptions, packets, statistics (what `dumpcap -w -` writes)
//
// The stream is cut into self-contained units so the caller can count packets
// and start a new file at any packet boundary: a new file is valid if it
// begins with all preamble units seen so far (the pcap file header, or the
// pcapng section header plus the interface description blocks).
class CaptureStream
{
public:
    enum class Format { Unknown, Pcap, PcapNg };
    enum class Kind { Preamble, Packet, Other };

    struct Unit
    {
        Kind kind = Kind::Other;
        qsizetype offset = 0;       // into the internal buffer; valid until compact()/feed()
        qsizetype length = 0;
        bool sectionStart = false;  // first preamble unit of a new file/section
    };

    void reset();
    void feed(const QByteArray &data);
    // The next complete unit, or false if more data is needed or the stream
    // is malformed (then error() is set and no further units are returned).
    bool next(Unit *unit);
    const char *bytes(const Unit &unit) const { return m_buffer.constData() + unit.offset; }
    // Drop the bytes of all units returned so far.
    void compact();

    Format format() const { return m_format; }
    QString error() const { return m_error; }
    // Bytes received that do not form a complete unit yet.
    qsizetype pendingBytes() const { return m_buffer.size() - m_position; }

    // 24-byte pcap file header for the given LINKTYPE_ value, used to let
    // tcpdump compile a filter without opening a device.
    static QByteArray pcapFileHeader(quint32 linkType, quint32 snapLength = 262144);

private:
    enum { MaxUnitSize = 64 * 1024 * 1024 };

    bool nextPcap(Unit *unit);
    bool nextPcapNg(Unit *unit);
    quint32 read32(qsizetype offset) const;
    bool fail(const QString &message);

    QByteArray m_buffer;
    qsizetype m_position = 0;
    Format m_format = Format::Unknown;
    bool m_bigEndian = false;
    bool m_headerSeen = false;
    QString m_error;
};
