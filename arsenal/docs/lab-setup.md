# Building an isolated practice lab

A lab is a small network of virtual machines that exists only to be tested,
that cannot reach the internet or your real network, and that you can throw
away and restore at will. Everything in the Arsenal profiles can be learned
there without touching a system that belongs to someone else.

The instructions use libvirt/QEMU, which are in the *Developer* profile
(`virt-manager`, `qemu-system-x86`) and work without any proprietary software.

## 1. Principles

* **Isolated by construction.** The lab network has no route to the outside.
  Isolation that depends on remembering not to type an address is not
  isolation.
* **Disposable.** Take a snapshot of every machine in its clean state; revert
  after each exercise.
* **Known targets from official sources.** Use systems that were published to
  be attacked, downloaded from the project that makes them, with the checksum
  verified.
* **Nothing of yours inside.** No personal accounts, keys, password stores or
  shared folders in lab machines — including the machine you attack from.

## 2. Host preparation

```
sudo apt install virt-manager qemu-system-x86 qemu-utils libvirt-daemon-system
sudo adduser "$USER" libvirt        # log out and in again afterwards
```

Check that hardware virtualisation is available: `ls /dev/kvm`.

## 3. A host-only (isolated) network

libvirt calls a network without any forwarding "isolated": guests can talk to
each other and to the host's address on that network, and nowhere else.

Save as `lab-net.xml`:

```xml
<network>
  <name>lab</name>
  <bridge name="virbr-lab" stp="on" delay="0"/>
  <!-- no <forward> element: the network is not routed or NATed anywhere -->
  <ip address="10.99.0.1" netmask="255.255.255.0">
    <dhcp>
      <range start="10.99.0.100" end="10.99.0.200"/>
    </dhcp>
  </ip>
</network>
```

```
virsh --connect qemu:///system net-define lab-net.xml
virsh --connect qemu:///system net-start lab
virsh --connect qemu:///system net-autostart lab      # optional
```

In virt-manager the same thing is *Edit → Connection Details → Virtual
Networks → +*, mode **Isolated**.

Choose an address range that is used nowhere else you connect to
(`10.99.0.0/24` above), so that a mistyped address can never be a real host.

For a network the host itself cannot reach either, omit the `<ip>` element and
give the guests static addresses; use this for malware analysis.

## 4. Machines

Attach **every** lab machine to the `lab` network only. A machine with a second
interface on the default (NAT) network is a bridge out of the lab.

* **Attacker machine**: a Shadow OS installation in a VM with the profiles you
  want. Install the profiles *before* moving its interface to the lab network
  (or keep a second, clean VM for downloads), and do not move it back and forth
  while targets are running.
* **Targets**: see the next section.

Give target machines no more than they need: no shared folders, no clipboard
sharing, no USB pass-through, no host devices.

## 5. Intentionally vulnerable targets — official sources

Download only from the project's own site or repository and verify the
published checksum or signature. Read each project's licence and notes: these
systems are insecure on purpose and must never be exposed to another network.

| Target | What it is for | Official source |
|---|---|---|
| OWASP Juice Shop | Modern web application with a broad set of flaws | <https://owasp.org/www-project-juice-shop/> |
| OWASP WebGoat | Guided lessons on web application security | <https://owasp.org/www-project-webgoat/> |
| DVWA | Classic PHP/MySQL training application | <https://github.com/digininja/DVWA> |
| OWASP crAPI | Deliberately vulnerable API | <https://owasp.org/www-project-crapi/> |
| Metasploitable 3 | Vulnerable Linux and Windows machines built from templates | <https://github.com/rapid7/metasploitable3> |
| OWASP Security Shepherd | Web and mobile training platform | <https://owasp.org/www-project-security-shepherd/> |
| Debian / Windows evaluation images | Realistic, *non*-vulnerable hosts for defensive exercises | <https://www.debian.org/distrib/>, <https://www.microsoft.com/evalcenter> |

Container-based targets (Juice Shop, WebGoat, crAPI) need their images
downloaded once. Do that in a build VM that has internet access, then shut it
down, switch its interface to `lab`, snapshot, and only then start the target.

For blue-team practice, add a monitoring machine with the *Blue Team* profile
on the same network and let it watch the others.

## 6. Snapshots

Take a snapshot when a machine is installed, configured and not yet touched:

```
virsh --connect qemu:///system snapshot-create-as TARGET clean "installed, before any exercise"
virsh --connect qemu:///system snapshot-list TARGET
virsh --connect qemu:///system snapshot-revert TARGET clean
```

Revert targets after every exercise — a compromised target is no longer a
known target — and revert the attacker machine whenever it has handled
untrusted files.

## 7. Keeping lab traffic off real networks

Check, do not assume:

* `virsh net-dumpxml lab` shows no `<forward>` element.
* Each guest has exactly one interface, on `lab`:
  `virsh domiflist NAME`.
* From a guest, `ping 1.1.1.1` and a DNS lookup of a public name both fail.
* On the host, watch the uplink while an exercise runs. Nothing from
  `10.99.0.0/24` may appear:
  `sudo tcpdump -ni <uplink-interface> net 10.99.0.0/24`.

Add a host firewall rule as a second barrier, so that a later configuration
mistake still cannot leak (nftables, in a table of its own):

```
sudo nft add table inet lab
sudo nft add chain inet lab forward '{ type filter hook forward priority -10; policy accept; }'
sudo nft add rule inet lab forward iifname "virbr-lab" oifname != "virbr-lab" counter drop
sudo nft add rule inet lab forward oifname "virbr-lab" iifname != "virbr-lab" counter drop
```

Further rules of thumb:

* Never bridge a lab machine to a physical interface, and never connect one to
  Wi-Fi.
* Wireless exercises need your own access point and your own client devices,
  with the transmit power low and — ideally — in a place where no other network
  is in range. Passing a USB radio through to a VM takes it out of the
  isolation described here: what it transmits is on the air.
* Do not run lab targets on a laptop that is connected to an employer's or a
  public network unless you have verified the points above on that day.
* Hazardous files (malware samples, captured payloads) stay inside a machine
  on a network without host address, are moved in on a read-only image, and
  never leave except as notes.
* When you are finished: shut the machines down, and `virsh net-destroy lab`.

## 8. When the lab is not enough

Public training platforms and bug-bounty programmes offer legal targets on the
internet. Their rules of engagement are the written permission described in
[legal-use.md](legal-use.md); they cover exactly what they say and nothing
more.
