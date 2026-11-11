#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Minimal FTP server for the network-location tests of SHADOW Files.

    ftpd.py <root> <port> [user password]

Serves <root> on 127.0.0.1:<port> in passive mode. Without credentials every
login is accepted; with them only that user. Test fixture only.
"""
import os
import socket
import socketserver
import sys
import time

ROOT = os.path.realpath(sys.argv[1])
PORT = int(sys.argv[2])
USER, PASSWORD = (sys.argv[3], sys.argv[4]) if len(sys.argv) > 4 else (None, None)


class Handler(socketserver.StreamRequestHandler):
    def reply(self, text):
        self.wfile.write((text + "\r\n").encode())

    def real(self, arg):
        path = arg if arg.startswith("/") else os.path.join(self.cwd, arg)
        path = os.path.normpath(path)
        full = os.path.realpath(ROOT + path)
        if full != ROOT and not full.startswith(ROOT + os.sep):
            raise PermissionError(arg)
        return full, path

    def data(self):
        conn, _ = self.passive.accept()
        self.passive.close()
        self.passive = None
        return conn

    def handle(self):
        self.cwd = "/"
        self.passive = None
        self.user = None
        self.authed = False
        self.rename_from = None
        self.rest = 0
        self.reply("220 SHADOW Files test server")
        while True:
            line = self.rfile.readline()
            if not line:
                return
            line = line.decode("utf-8", "replace").rstrip("\r\n")
            cmd, _, arg = line.partition(" ")
            cmd = cmd.upper()
            print(cmd, "***" if cmd == "PASS" else arg, flush=True)
            try:
                if cmd == "USER":
                    self.user = arg
                    self.reply("331 Password required")
                elif cmd == "PASS":
                    if USER is None or (self.user == USER and arg == PASSWORD):
                        self.authed = True
                        self.reply("230 Logged in")
                    else:
                        time.sleep(0.3)
                        self.reply("530 Login incorrect")
                elif cmd == "QUIT":
                    self.reply("221 Bye")
                    return
                elif cmd == "FEAT":
                    self.reply("211-Features\r\n SIZE\r\n MDTM\r\n REST STREAM\r\n UTF8\r\n211 End")
                elif cmd == "OPTS":
                    self.reply("200 OK")
                elif not self.authed:
                    self.reply("530 Please log in")
                elif cmd == "SYST":
                    self.reply("215 UNIX Type: L8")
                elif cmd == "PWD":
                    self.reply('257 "%s"' % self.cwd)
                elif cmd == "TYPE" or cmd == "MODE" or cmd == "STRU" or cmd == "NOOP":
                    self.reply("200 OK")
                elif cmd == "CWD":
                    full, path = self.real(arg)
                    if os.path.isdir(full):
                        self.cwd = path
                        self.reply("250 OK")
                    else:
                        self.reply("550 Not a directory")
                elif cmd == "CDUP":
                    self.cwd = os.path.dirname(self.cwd) or "/"
                    self.reply("250 OK")
                elif cmd in ("PASV", "EPSV"):
                    if self.passive:
                        self.passive.close()
                    self.passive = socket.socket()
                    self.passive.bind(("127.0.0.1", 0))
                    self.passive.listen(1)
                    port = self.passive.getsockname()[1]
                    if cmd == "PASV":
                        self.reply("227 Entering Passive Mode (127,0,0,1,%d,%d)" % (port >> 8, port & 255))
                    else:
                        self.reply("229 Entering Extended Passive Mode (|||%d|)" % port)
                elif cmd == "LIST" or cmd == "NLST":
                    args = [a for a in arg.split() if not a.startswith("-")]
                    full, _ = self.real(args[0] if args else ".")
                    names = sorted(os.listdir(full)) if os.path.isdir(full) else [os.path.basename(full)]
                    base = full if os.path.isdir(full) else os.path.dirname(full)
                    self.reply("150 Here it comes")
                    conn = self.data()
                    out = []
                    for name in names:
                        st = os.lstat(os.path.join(base, name))
                        kind = "d" if os.path.isdir(os.path.join(base, name)) else "-"
                        perms = "".join(c if st.st_mode & (1 << (8 - i)) else "-" for i, c in enumerate("rwxrwxrwx"))
                        stamp = time.strftime("%b %d %H:%M", time.localtime(st.st_mtime))
                        out.append("%s%s 1 ftp ftp %d %s %s" % (kind, perms, st.st_size, stamp, name) if cmd == "LIST" else name)
                    conn.sendall(("\r\n".join(out) + ("\r\n" if out else "")).encode())
                    conn.close()
                    self.reply("226 Done")
                elif cmd == "SIZE":
                    full, _ = self.real(arg)
                    self.reply("213 %d" % os.path.getsize(full) if os.path.isfile(full) else "550 Not a file")
                elif cmd == "MDTM":
                    full, _ = self.real(arg)
                    self.reply("213 " + time.strftime("%Y%m%d%H%M%S", time.gmtime(os.path.getmtime(full))))
                elif cmd == "REST":
                    self.rest = int(arg)
                    self.reply("350 OK")
                elif cmd == "RETR":
                    full, _ = self.real(arg)
                    if not os.path.isfile(full):
                        self.reply("550 Not a file")
                        continue
                    self.reply("150 Sending")
                    conn = self.data()
                    with open(full, "rb") as f:
                        f.seek(self.rest)
                        self.rest = 0
                        while chunk := f.read(65536):
                            conn.sendall(chunk)
                    conn.close()
                    self.reply("226 Done")
                elif cmd in ("STOR", "APPE"):
                    full, _ = self.real(arg)
                    self.reply("150 Receiving")
                    conn = self.data()
                    with open(full, "ab" if cmd == "APPE" else "wb") as f:
                        while chunk := conn.recv(65536):
                            f.write(chunk)
                    conn.close()
                    self.reply("226 Done")
                elif cmd == "MKD":
                    full, path = self.real(arg)
                    os.mkdir(full)
                    self.reply('257 "%s" created' % path)
                elif cmd == "RMD":
                    os.rmdir(self.real(arg)[0])
                    self.reply("250 OK")
                elif cmd == "DELE":
                    os.remove(self.real(arg)[0])
                    self.reply("250 OK")
                elif cmd == "RNFR":
                    full, _ = self.real(arg)
                    if os.path.lexists(full):
                        self.rename_from = full
                        self.reply("350 Ready")
                    else:
                        self.reply("550 No such file")
                elif cmd == "RNTO":
                    os.rename(self.rename_from, self.real(arg)[0])
                    self.reply("250 OK")
                elif cmd == "SITE" and arg.upper().startswith("CHMOD "):
                    _, mode, name = arg.split(" ", 2)
                    os.chmod(self.real(name)[0], int(mode, 8))
                    self.reply("200 OK")
                else:
                    self.reply("502 Not implemented")
            except (OSError, ValueError, IndexError) as error:
                self.reply("550 %s" % error)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


Server(("127.0.0.1", PORT), Handler).serve_forever()
