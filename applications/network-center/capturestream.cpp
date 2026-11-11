// SPDX-License-Identifier: GPL-3.0-or-later
#include "capturestream.h"

#include <QObject>
#include <QtEndian>

namespace {

// pcapng block types (draft-ietf-opsawg-pcapng).
const quint32 kSectionHeader = 0x0A0D0D0A;
const quint32 kInterfaceDescription = 0x00000001;
const quint32 kObsoletePacket = 0x00000002;
const quint32 kSimplePacket = 0x00000003;
const quint32 kEnhancedPacket = 0x00000006;
const quint32 kByteOrderMagic = 0x1A2B3C4D;

} // namespace

void CaptureStream::reset()
{
    m_buffer.clear();
    m_position = 0;
    m_format = Format::Unknown;
    m_bigEndian = false;
    m_headerSeen = false;
    m_error.clear();
}

void CaptureStream::feed(const QByteArray &data)
{
    m_buffer.append(data);
}

void CaptureStream::compact()
{
    if (m_position == 0)
        return;
    m_buffer.remove(0, m_position);
    m_position = 0;
}

quint32 CaptureStream::read32(qsizetype offset) const
{
    const auto *p = reinterpret_cast<const uchar *>(m_buffer.constData() + offset);
    return m_bigEndian ? qFromBigEndian<quint32>(p) : qFromLittleEndian<quint32>(p);
}

bool CaptureStream::fail(const QString &message)
{
    m_error = message;
    return false;
}

bool CaptureStream::next(Unit *unit)
{
    if (!m_error.isEmpty())
        return false;
    if (m_format == Format::Unknown) {
        if (pendingBytes() < 4)
            return false;
        const auto *p = reinterpret_cast<const uchar *>(m_buffer.constData() + m_position);
        const quint32 magic = qFromBigEndian<quint32>(p);
        switch (magic) {
        case 0xA1B2C3D4:        // pcap, microseconds, big-endian writer
        case 0xA1B23C4D:        // pcap, nanoseconds, big-endian writer
            m_format = Format::Pcap;
            m_bigEndian = true;
            break;
        case 0xD4C3B2A1:        // the same two, written little-endian
        case 0x4D3CB2A1:
            m_format = Format::Pcap;
            m_bigEndian = false;
            break;
        case kSectionHeader:    // reads the same in both byte orders
            m_format = Format::PcapNg;
            break;
        default:
            return fail(QObject::tr("The capture tool did not produce a pcap or pcapng stream."));
        }
    }
    return m_format == Format::Pcap ? nextPcap(unit) : nextPcapNg(unit);
}

bool CaptureStream::nextPcap(Unit *unit)
{
    if (!m_headerSeen) {
        if (pendingBytes() < 24)
            return false;
        unit->kind = Kind::Preamble;
        unit->sectionStart = true;
        unit->offset = m_position;
        unit->length = 24;
        m_position += 24;
        m_headerSeen = true;
        return true;
    }
    if (pendingBytes() < 16)
        return false;
    // Record header: ts_sec, ts_frac, captured length, original length.
    const quint32 captured = read32(m_position + 8);
    if (captured > MaxUnitSize)
        return fail(QObject::tr("The capture stream is corrupt (a packet of %1 bytes).").arg(captured));
    const qsizetype total = 16 + qsizetype(captured);
    if (pendingBytes() < total)
        return false;
    unit->kind = Kind::Packet;
    unit->sectionStart = false;
    unit->offset = m_position;
    unit->length = total;
    m_position += total;
    return true;
}

bool CaptureStream::nextPcapNg(Unit *unit)
{
    if (pendingBytes() < 12)
        return false;
    const auto *p = reinterpret_cast<const uchar *>(m_buffer.constData() + m_position);
    const bool sectionHeader = qFromBigEndian<quint32>(p) == kSectionHeader;
    if (sectionHeader) {
        // The byte-order magic of the section tells how to read everything in it.
        const quint32 bomBig = qFromBigEndian<quint32>(p + 8);
        const quint32 bomLittle = qFromLittleEndian<quint32>(p + 8);
        if (bomBig == kByteOrderMagic)
            m_bigEndian = true;
        else if (bomLittle == kByteOrderMagic)
            m_bigEndian = false;
        else
            return fail(QObject::tr("The capture stream is corrupt (bad pcapng section header)."));
    } else if (!m_headerSeen) {
        return fail(QObject::tr("The capture stream is corrupt (no pcapng section header)."));
    }
    const quint32 type = sectionHeader ? kSectionHeader : read32(m_position);
    const quint32 length = read32(m_position + 4);
    if (length < 12 || length % 4 != 0 || length > MaxUnitSize)
        return fail(QObject::tr("The capture stream is corrupt (a block of %1 bytes).").arg(length));
    if (pendingBytes() < qsizetype(length))
        return false;

    unit->sectionStart = sectionHeader;
    if (sectionHeader || type == kInterfaceDescription)
        unit->kind = Kind::Preamble;
    else if (type == kEnhancedPacket || type == kSimplePacket || type == kObsoletePacket)
        unit->kind = Kind::Packet;
    else
        unit->kind = Kind::Other;
    unit->offset = m_position;
    unit->length = qsizetype(length);
    m_position += qsizetype(length);
    m_headerSeen = true;
    return true;
}

QByteArray CaptureStream::pcapFileHeader(quint32 linkType, quint32 snapLength)
{
    QByteArray header(24, '\0');
    auto *p = reinterpret_cast<uchar *>(header.data());
    qToLittleEndian<quint32>(0xA1B2C3D4, p);        // magic, microsecond timestamps
    qToLittleEndian<quint16>(2, p + 4);             // version 2.4
    qToLittleEndian<quint16>(4, p + 6);
    qToLittleEndian<quint32>(0, p + 8);             // thiszone
    qToLittleEndian<quint32>(0, p + 12);            // sigfigs
    qToLittleEndian<quint32>(snapLength, p + 16);
    qToLittleEndian<quint32>(linkType, p + 20);
    return header;
}
