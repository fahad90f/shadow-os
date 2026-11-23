---
name: web-assessment
title: Assess a web target
mode: pentest
kinds: agents
summary: Enumerate an authorized web app (nikto, gobuster) and test for injection (sqlmap) with care.
triggers: web, web app, website, http, https, nikto, gobuster, directory brute, dirbust, sql injection, sqli, sqlmap, xss, web enumeration, crawl, fuzz web, owasp, login form
---
# Assess a web target (authorized)

Only against a site in the signed scope. Confirm the exact URL and that it is in scope first.

1. **Enumerate** with `web_enum` (nikto + gobuster): server and framework, headers, obvious misconfigurations, and hidden paths and files. Read what it finds before going further.
2. **Map the surface** by hand too: the forms, parameters, auth flow, cookies and API endpoints. Note anything that takes user input — that is where injection lives.
3. **Test for injection where input flows into a query.** `run_sqlmap` is an active, intrusive test: confirm the specific target URL/parameter with the user before running it, keep it to the agreed scope, and do not dump more data than needed to prove the finding.
4. **Other classes** (XSS, auth bypass, IDOR, SSRF) you probe with crafted requests via `run_shell_command` (curl); prove with a minimal, non-destructive payload, never a defacement or real data theft.
5. For each finding: the request that triggers it, the impact, and the fix (parameterized queries, output encoding, access checks, a header).

Stop and ask before anything that writes to the app, changes data, or could take it down.
