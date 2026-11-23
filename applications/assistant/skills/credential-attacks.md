---
name: credential-attacks
title: Test credentials
mode: pentest
kinds: agents
summary: Crack captured hashes (hashcat/john) and test login strength (hydra) only with explicit authorization.
triggers: password, hash, crack, hashcat, john, cracking, brute force, bruteforce, hydra, credentials, login brute, dictionary attack, wordlist, weak password
---
# Test credentials (authorized)

Credential testing is intrusive and sensitive. Do it only on hashes or accounts in the signed scope, and get the user's explicit go-ahead on the specific target before any online attempt.

1. **Offline first where possible.** Cracking a captured hash with `crack_hash` (hashcat/john) touches no live system and is the safer way to show that a password policy is weak. Identify the hash type, pick a wordlist and rules, and report how fast it fell — that is the finding.
2. **Online brute force is loud and risky.** `brute_service` (hydra) against a live login can lock accounts and trip alarms. Confirm scope and permission on that exact service, keep the username list and rate modest, and prefer a short, targeted guess list over a huge one.
3. **Never test credentials against a system outside the engagement**, and never exfiltrate or reuse real credentials beyond proving the finding.
4. **Report** the weak accounts or hashes, how they fell, and the fix: stronger policy, rate limiting and lockout, MFA, better hashing (bcrypt/argon2), and rotation of what was exposed.

If authorization for the specific target is not clear, stop and ask; do not guess.
