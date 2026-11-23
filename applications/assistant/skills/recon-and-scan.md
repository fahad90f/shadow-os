---
name: recon-and-scan
title: Scan a target
mode: pentest
kinds: agents
summary: Map in-scope hosts, ports and service versions with nmap (pentest_scan), least-noisy first.
triggers: scan, nmap, port scan, recon, reconnaissance, discover hosts, find open ports, service version, what's running on, enumerate ports, host discovery, network map
---
# Scan a target (authorized)

Confirm the target is in scope before sending a single packet. Scan what you were authorized to scan, nothing adjacent.

1. **Discover** what is live before scanning everything — a host-discovery pass over the authorized range, then scan only the hosts that answered.
2. **Start light.** Use `pentest_scan` with the quick profile first (top ports), read it, then go deeper (service/version detection, then full-port) only where it is warranted. A full aggressive scan of a wide range is noisy and rarely the right first move.
3. **Record service versions** — they drive the next phase (exploit-research, web-assessment). Note the OS guess, the open ports, and anything unusual (an admin port, an old version banner).
4. **Mind the blast radius.** Fragile or production devices can fall over under aggressive timing; keep timing reasonable and say so if the target looks delicate.
5. For anything `pentest_scan` does not cover, `run_shell_command` the tool directly (nmap scripts, masscan), still inside scope.

Summarize: live hosts, their open ports and versions, and the two or three things most worth looking at next.
