#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A tiny DNS server for the systemd-resolved lab test (resolved-lab-test.sh).

    dns_responder.py <address> <log file> [--port N] [--udp-only] [--tls <cert> <key>]

Answers every A query with 203.0.113.7 (RFC 5737), DS and DNSKEY queries with
an unsigned record, and every other query with NODATA, on UDP and TCP port 53
(or --port N) of <address>; --udp-only: no TCP (like Tor's DNSPort); with
--tls also on port 853 (DNS-over-TLS). EDNS0 is answered like a resolver without
DNSSEC does (an OPT record without the DO bit): systemd-resolved then keeps
EDNS0 and TLS, and with DNSSEC=allow-downgrade stops validating (this server
cannot produce signatures or proofs of unsigned zones). Every query is
appended to <log file> as "<udp|tcp|tls> <name>". Runs until killed.
Standard library only.
"""
import socket
import ssl
import struct
import sys
import threading


def qname(packet):
    labels, pos = [], 12
    while pos < len(packet) and packet[pos]:
        length = packet[pos]
        labels.append(packet[pos + 1:pos + 1 + length].decode("ascii", "replace"))
        pos += 1 + length
    qtype = struct.unpack(">H", packet[pos + 1:pos + 3])[0] if pos + 3 <= len(packet) else 0
    return ".".join(labels).lower(), pos + 5, qtype


def edns_do(query, end):
    """None without an OPT record in the query, else whether its DO bit is set."""
    arcount = struct.unpack(">H", query[10:12])[0]
    pos = end
    for _ in range(arcount):
        if pos + 11 > len(query) or query[pos] != 0:      # OPT: owner is the root
            return None
        rtype, _cls, ttl, rdlen = struct.unpack(">HHIH", query[pos + 1:pos + 11])
        if rtype == 41:
            return bool(ttl & 0x8000)
        pos += 11 + rdlen
    return None


SOA_RDATA = (b"\x02ns\x07example\x00" b"\x0ahostmaster\x07example\x00"
             + struct.pack(">IIIII", 1, 3600, 600, 86400, 60))


# What a forwarder without DNSSEC support returns for DS and DNSKEY queries:
# the records, but no signatures (resolved's allow-downgrade detects exactly
# this and stops validating).
RECORDS = {
    1: bytes([203, 0, 113, 7]),                                               # A
    43: struct.pack(">HBB", 12345, 8, 2) + bytes(32),                         # DS
    48: struct.pack(">HBB", 257, 3, 8) + bytes(64),                           # DNSKEY
}


def answer(query):
    name, end, qtype = qname(query)
    do = edns_do(query, end)
    nodata = qtype not in RECORDS
    header = query[:2] + struct.pack(">HHHHH", 0x8180 | (query[2] & 0x01) << 8, 1, 0 if nodata else 1,
                                     1 if nodata else 0, 0 if do is None else 1)
    body = query[12:end]
    if not nodata:
        rdata = RECORDS[qtype]
        body += b"\xc0\x0c" + struct.pack(">HHIH", qtype, 1, 60, len(rdata)) + rdata
    else:
        # NODATA, as an unsigned zone answers: the SOA in the authority section.
        body += b"\xc0\x0c" + struct.pack(">HHIH", 6, 1, 60, len(SOA_RDATA)) + SOA_RDATA
    if do is not None:
        body += b"\x00" + struct.pack(">HHIH", 41, 1232, 0, 0)      # EDNS0 yes, DO (DNSSEC) no
    return name, header + body


class Server:
    def __init__(self, address, log):
        self.address, self.log, self.lock = address, log, threading.Lock()

    def record(self, proto, name):
        with self.lock, open(self.log, "a") as fh:
            fh.write(f"{proto} {name}\n")

    def udp(self, port=53):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # As Tor's DNSPort does (and most DNS servers): on a port any user
        # may bind, another socket with SO_REUSEADDR can bind it too.
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind((self.address, port))
        while True:
            data, peer = sock.recvfrom(4096)
            name, reply = answer(data)
            self.record("udp", name)
            sock.sendto(reply, peer)

    def stream(self, conn, proto):
        try:
            while True:
                head = conn.recv(2)
                if len(head) < 2:
                    return
                length = struct.unpack(">H", head)[0]
                data = b""
                while len(data) < length:
                    chunk = conn.recv(length - len(data))
                    if not chunk:
                        return
                    data += chunk
                name, reply = answer(data)
                self.record(proto, name)
                conn.sendall(struct.pack(">H", len(reply)) + reply)
        except (OSError, ssl.SSLError):
            return
        finally:
            conn.close()

    def tcp(self, port, context=None):
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind((self.address, port))
        sock.listen(16)
        while True:
            conn, _ = sock.accept()
            if context is not None:
                try:
                    conn = context.wrap_socket(conn, server_side=True)
                except (OSError, ssl.SSLError):
                    conn.close()
                    continue
            threading.Thread(target=self.stream, args=(conn, "tls" if context else "tcp"), daemon=True).start()


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: dns_responder.py <address> <log file> [--port N] [--udp-only] [--tls <cert> <key>]")
    address, log, rest = sys.argv[1], sys.argv[2], sys.argv[3:]
    port, udp_only, tls = 53, False, None
    while rest:
        opt = rest.pop(0)
        if opt == "--port":
            port = int(rest.pop(0))
        elif opt == "--udp-only":
            udp_only = True
        elif opt == "--tls":
            tls = (rest.pop(0), rest.pop(0))
        else:
            sys.exit(f"dns_responder.py: unknown option {opt}")
    server = Server(address, log)
    threads = [threading.Thread(target=server.udp, args=(port,), daemon=True)]
    if not udp_only:
        threads.append(threading.Thread(target=server.tcp, args=(port,), daemon=True))
    if tls:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(*tls)
        threads.append(threading.Thread(target=server.tcp, args=(853, context), daemon=True))
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()


if __name__ == "__main__":
    main()
