---
name: traffic-capture
title: Capture traffic
mode: pentest
kinds: agents
summary: Capture and read network traffic (tcpdump) on an interface you are authorized to monitor.
triggers: capture, traffic, tcpdump, sniff, packets, pcap, wireshark, network traffic, listen on, monitor traffic, intercept, analyze packets
---
# Capture traffic (authorized)

Capture only on a network and interface you own or were authorized to monitor. Other people's traffic is private; do not capture it.

1. **Scope the capture.** Pick the right interface and a filter that limits it to the authorized target (a host or port), so you are not recording bystanders. `start_capture` (tcpdump) wraps this; pass a tight filter.
2. **Capture briefly, to a file**, then read it rather than staring at a live stream: open the pcap, follow the streams that matter, and look for what the engagement is about — cleartext credentials, tokens in URLs, unencrypted protocols, odd destinations.
3. **Handle what you find as sensitive.** Anything captured may contain real personal data; keep it local, show only what proves the finding, and delete the capture when done.
4. **Report** the exposure (what was in the clear, between whom) and the fix: TLS everywhere, no secrets in URLs, segment the network, disable the legacy protocol.

Do not leave a capture running unattended, and do not capture on a network you are only a guest on.
