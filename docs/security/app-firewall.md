# Per-application firewall

Take the network away from one application. A blocked application is started
without any network access: it cannot reach the internet, the local network,
or services on this computer.

It is off for every application until you block one.

## Using it

**Control Center → Firewall → Applications.** Every installed application has
a switch. Switch one off and it is started without network access from then
on. Changing a switch asks for an administrator password.

A change applies **the next time the application is started**. An instance
that is already running keeps what it had: close and reopen it.

From a terminal:

```bash
shadow-appfw list                          # the applications and their mode
```

```bash
sudo shadow-appfw set firefox-esr deny
```

```bash
sudo shadow-appfw set firefox-esr allow
```

```bash
shadow-appfw status                        # what is blocked, loaded and running
```

The name is the application's launcher entry without `.desktop`
(`shadow-appfw list` shows them). To start any command without network
access, block its application and start it through the wrapper:

```bash
shadow-appfw-run firefox-esr -- firefox-esr --private-window
```

## What a blocked application sees

Connections are refused at once (TCP is reset, everything else gets an ICMP
error); nothing waits for a timeout. Loopback is blocked too. A local proxy or
resolver would otherwise carry the application's traffic out: Tor's SOCKS
port, and the resolver at 127.0.0.53, both listen on loopback. A program that
talks to itself over TCP on localhost therefore does not work while blocked.
Unix sockets are not network traffic: the display, sound and D-Bus keep
working.

If the application cannot be confined (the rule does not load, polkit refuses,
systemd does not answer) it is **not started**, and a notification says so. A
blocked application never comes up with the network because something went
wrong.

## What it does not do

* **No rules by destination, no prompt per connection.** An application has
  the network or it does not.
* **Only applications started through their launcher entry**: the launcher,
  the dock, "open with", a file association. A program started by its command
  name in a terminal is not blocked (use `shadow-appfw-run`). An entry of your
  own in `~/.local/share/applications` takes precedence over the firewall's
  and is used as it is.
* **Not a sandbox.** It keeps an ordinary application off the network. It does
  not contain a hostile one: a blocked program can still ask another program
  that is not blocked to fetch something for it (the session's service
  manager, a helper reached over D-Bus, a scheduled job). What it cannot do is
  open a connection itself, or move itself or its children out of the
  confinement.
* **A socket handed over by another program** keeps the access of the program
  that opened it.

## How it works

| Piece | What it does |
|---|---|
| `/etc/shadow-os/appfw.json` | which applications are blocked (world-readable; written by `shadow-appfw set`) |
| `/usr/local/share/applications/<name>.desktop` | for each blocked application, a copy of its launcher entry whose `Exec` lines start it through `shadow-appfw-run`. `/usr/local/share` comes before `/usr/share`, so every launcher uses the copy. Marked `X-Shadow-AppFw=deny`; an entry there without the mark is somebody else's and is never touched (the application is then reported as "not in force") |
| `shadow-appfw-run` | asks to be confined, then *becomes* the application (`exec`), so not one instruction of the application runs outside the confinement |
| `shadow-appfw-enter` (through pkexec) | the only request an ordinary user may make without a password. It can only take network access away from the caller's own starting process |
| `shadowblock.slice` | a system slice. The starting process is adopted into a scope in it (`shadowblock-<name>-<pid>.scope`), bound to the user's session so that it ends at logout. The slice belongs to root: a process cannot leave it |
| table `inet shadow_appfw` | one set and two rules in the `output` hook at priority -40: reject whatever is sent from a socket that was created in the slice (`socket cgroupv2 level 1 @blocked`) |

The rule is loaded when something is blocked: at boot
(`shadow-appfw.service`), when an application is blocked, and again each time
a blocked application starts, which is also when the slice (it exists only
while something runs in it) is put into the rule's set. The watchdog that
guards the kill switch (`shadow-killswitch-watch.timer`) puts the table back
within 20 seconds if something removes it while a blocked application runs.

When a package installs, updates or removes an application, the copies in
`/usr/local/share/applications` are written again (a dpkg trigger of
`shadow-hardening`), and at every boot.

Next to the other firewall pieces: the host firewall (`inet shadow_filter`)
filters what comes in; the kill switch (`inet shadow_egress`, priority -50)
decides what may leave the computer at all; this table only ever adds a
refusal for the blocked applications. Hide Me and VPNs are not affected.

## Switching it off for one start

Add `shadow.appfw=off` to the kernel command line (GRUB: `e`, add it to the
`linux` line). The rule is not loaded and blocked applications are started
with the network until the next restart without it. The Firewall page says so
while it is in effect.

## Removing the package

Removing `shadow-hardening` deletes the firewall's launcher entries (the
applications start normally again) and the table. `/etc/shadow-os/appfw.json`
is kept until the package is purged.

## Tests

| Test | What it covers | Run |
|---|---|---|
| `tests/security/test_appfw_unit.py` | the logic with the kernel and systemd kept in memory: what is written for a blocked application and what is never touched, when the rule is loaded, who may confine which process, that every failure keeps a blocked application from starting; `shadow-appfw-run` with a stand-in pkexec; the polkit actions, unit and packaging lists | any user |
| `tests/network/test_appfw_netns.py` | the real nft, real control groups and real packets in a private network namespace: a confined process reaches nothing over TCP, UDP or ICMP, to another machine or to loopback, and neither do its children; a process outside is not affected; an unprivileged process cannot leave the slice; the rule follows a slice that came back; the watchdog restores a deleted table | root |

Beyond these, the whole path was run once by hand with the image's own
systemd, D-Bus, polkit and nft booted as the init of private namespaces (the
tests above replace systemd by a stand-in). What still needs a graphical
login on a booted system is in [limitations.md](../limitations.md).
