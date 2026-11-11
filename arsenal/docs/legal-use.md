# Legal and responsible use

Shadow OS is made for security professionals, students and developers who do
**authorised** testing, research, defence and forensics. The tools SHADOW
Arsenal installs are ordinary free software from the Debian archive. Many of
them are dual-use: the scanner that shows an administrator what is exposed
shows the same thing to anyone else. What makes their use legitimate is
authorisation, not the tool.

This page is not legal advice. If an engagement matters, ask a lawyer who knows
the law of the countries involved.

## The rule

Use these tools only on systems, networks, applications and devices that

1. **you own**, or
2. **you have explicit, written permission to test**, given by someone who is
   entitled to give it.

"Entitled" matters. An employee usually cannot authorise a test of their
employer's network; a customer cannot authorise a test of the cloud platform or
hosting provider their service runs on; a tenant cannot authorise a test of the
building's network. Shared and third-party infrastructure has its own owner and
often its own testing policy — read it and follow it.

## Before you start

Put the following in writing and have it signed or confirmed by the system
owner before the first packet is sent:

* **Scope**: the exact addresses, host names, applications and accounts that
  may be tested, and what is explicitly out of scope.
* **Methods**: what is allowed (for example scanning and exploitation of
  listed hosts) and what is not (for example denial of service, social
  engineering, physical access, tests against production data).
* **Time window** and the time zone it is in.
* **Contacts**: who to call when something breaks, on both sides, at any hour
  of the window.
* **Data handling**: what you may collect, where it is stored, who may see it,
  and when it is destroyed.
* **Reporting**: who receives the findings and how they are transmitted.

Keep that document with you during the work. Stop and ask when you reach
something that is not clearly covered by it — a host outside the listed range,
another organisation's data, a system that turns out to be safety-relevant.

## Law

The details differ between countries; the outline does not. Almost everywhere
it is a criminal offence to

* access a computer system or data without authorisation, or beyond the
  authorisation you have;
* interfere with a system or its data (alter, delete, disrupt, overload);
* intercept communications that are not addressed to you;
* obtain, use or pass on other people's credentials.

Some jurisdictions also regulate making, possessing or supplying tools
"primarily" intended for such offences, the export of some security and
cryptographic software, and transmitting on radio frequencies (which covers
several wireless and software-defined-radio tools). The law of the place where
the target is, the place where you are, and the places the traffic passes
through can all apply. You are responsible for knowing which rules apply to
you; "I was only learning" and "the port was open" are not defences.

Bug-bounty and vulnerability-disclosure programmes are a form of written
authorisation: they apply only to the scope and methods the programme
publishes, and only as long as you follow its rules.

## While you work

* Prefer the least intrusive technique that answers the question.
* Do not use more privilege, and do not look at more data, than the task needs.
* Keep notes of what you did and when; they protect you and help the owner.
* Personal data you come across is still protected by privacy law. Collect the
  minimum, store it encrypted, and delete it when the engagement ends.
* Report weaknesses to the owner. Do not publish them before the owner had a
  reasonable chance to fix them.
* If you cause damage or an outage, say so immediately to the agreed contact.

## Learning and research

Practise on an environment you built for the purpose: virtual machines on an
isolated network, with targets that are meant to be attacked. See
[lab-setup.md](lab-setup.md). Malware samples and other hazardous material are
not part of Shadow OS and should only ever be handled in such an environment.

## What Shadow OS does and does not do

* No tool is installed unless you ask for a profile or a tool.
* Every package comes from Debian `main` and is installed by the package
  manager, with signature verification. SHADOW Arsenal contains no exploit code,
  payloads, malware samples, credentials or word lists.
* Tools that are not in Debian `main` are only *listed*, with a link to their
  vendor; SHADOW Arsenal never downloads or repackages them.
* SHADOW Arsenal asks every user once to acknowledge these conditions. The
  acknowledgement is stored in `~/.config/shadow/arsenal.conf`. It records that
  you were told; it does not authorise anything.

## No warranty

The tools and Shadow OS are distributed in the hope that they are useful, but
**without any warranty**, to the extent permitted by law — see the licence of
each package under `/usr/share/doc/<package>/copyright`. Several tools can
interrupt services or destroy data when used carelessly. The Shadow OS
developers and the authors of the tools are not liable for what you do with
them.
