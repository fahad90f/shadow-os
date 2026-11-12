#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Shadow OS — OS hardening setup hook.
# Runs during distro build (live-build hook) as root inside the chroot.
# Best-effort: failures are logged but do not abort the build.

set +e
set -u

LOGPFX="[70-hardening-setup]"

log()  { echo "${LOGPFX} $*"; }
warn() { echo "${LOGPFX} WARNING: $*" >&2; }

# ---------------------------------------------------------------------------
# 1. Install hardening packages
# ---------------------------------------------------------------------------
log "Installing hardening packages…"
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    auditd            \
    audispd-plugins   \
    apparmor          \
    apparmor-utils    \
    apparmor-profiles \
    libpam-pwquality  \
    fail2ban          \
    aide              \
    needrestart       \
    rkhunter          \
    || warn "Some packages failed to install."

# ---------------------------------------------------------------------------
# 2. Enable auditd at boot
# ---------------------------------------------------------------------------
log "Enabling auditd…"
systemctl enable auditd 2>/dev/null || warn "auditd enable failed."

# ---------------------------------------------------------------------------
# 3. Enable AppArmor at boot; enforce all profiles (best-effort)
# ---------------------------------------------------------------------------
log "Enabling AppArmor…"
systemctl enable apparmor 2>/dev/null || warn "apparmor enable failed."

if command -v aa-enforce >/dev/null 2>&1; then
    aa-enforce /etc/apparmor.d 2>/dev/null || warn "aa-enforce had errors (non-fatal)."
    log "AppArmor profiles set to enforce mode."
else
    warn "aa-enforce not found; AppArmor profiles left in complain mode."
fi

# ---------------------------------------------------------------------------
# 4. Apply extreme sysctl parameters immediately
# ---------------------------------------------------------------------------
if [ -f /etc/sysctl.d/91-shadow-extreme.conf ]; then
    log "Applying 91-shadow-extreme.conf sysctl params…"
    sysctl -p /etc/sysctl.d/91-shadow-extreme.conf 2>/dev/null \
        || warn "Some sysctl params not supported by build kernel (non-fatal)."
else
    warn "91-shadow-extreme.conf not found; sysctl step skipped."
fi

# ---------------------------------------------------------------------------
# 5. Configure PAM password quality (libpam-pwquality)
# ---------------------------------------------------------------------------
PWQCONF="/etc/security/pwquality.conf"
log "Configuring PAM password quality…"
if [ -f "${PWQCONF}" ]; then
    # Remove any existing lines for these keys, then append our values.
    sed -i '/^[ \t]*minlen\b/d'   "${PWQCONF}"
    sed -i '/^[ \t]*dcredit\b/d'  "${PWQCONF}"
    sed -i '/^[ \t]*ucredit\b/d'  "${PWQCONF}"
    sed -i '/^[ \t]*ocredit\b/d'  "${PWQCONF}"
    sed -i '/^[ \t]*lcredit\b/d'  "${PWQCONF}"
fi
cat >> "${PWQCONF}" <<'PWEOF'

# Shadow OS hardening — written by 70-hardening-setup.sh
minlen  = 12
dcredit = -1
ucredit = -1
ocredit = -1
lcredit = -1
PWEOF
log "Password quality policy applied (minlen=12, complexity required)."

# ---------------------------------------------------------------------------
# 6. Configure PAM faillock (account lockout after 5 failures)
# ---------------------------------------------------------------------------
FLCONF="/etc/security/faillock.conf"
log "Configuring faillock…"
mkdir -p "$(dirname "${FLCONF}")"
cat > "${FLCONF}" <<'FLEOF'
# Shadow OS — account lockout policy, written by 70-hardening-setup.sh.
# Lock account after 5 failures within 15 minutes; unlock after 15 minutes.
deny         = 5
fail_interval = 900
unlock_time  = 900
FLEOF
log "faillock configured (deny=5, unlock_time=900s)."

# ---------------------------------------------------------------------------
# 7. Verify module blacklist is in place
# ---------------------------------------------------------------------------
BLCONF="/etc/modprobe.d/shadow-blacklist.conf"
if [ -f "${BLCONF}" ]; then
    log "Module blacklist found at ${BLCONF}."
else
    warn "Module blacklist not found at ${BLCONF}."
fi

# ---------------------------------------------------------------------------
# 8. Regenerate initramfs so blacklist and module conf take effect at boot
# ---------------------------------------------------------------------------
log "Regenerating initramfs (this may take a while)…"
update-initramfs -u 2>/dev/null || warn "update-initramfs failed (non-fatal in chroot)."

# ---------------------------------------------------------------------------
# 9. Ensure shadow-harden is executable
# ---------------------------------------------------------------------------
if [ -f /usr/bin/shadow-harden ]; then
    chmod 755 /usr/bin/shadow-harden
    log "shadow-harden marked executable."
else
    warn "shadow-harden not found at /usr/bin/shadow-harden."
fi

# ---------------------------------------------------------------------------
# 10. Write polkit rule for shadow-harden
# ---------------------------------------------------------------------------
POLKIT_RULES="/etc/polkit-1/rules.d/50-shadow-harden.rules"
log "Writing polkit rule for shadow-harden…"
mkdir -p "$(dirname "${POLKIT_RULES}")"
cat > "${POLKIT_RULES}" <<'PKEOF'
// Shadow OS — allow members of the wheel/sudo group to run shadow-harden as root.
// Written by 70-hardening-setup.sh.
polkit.addRule(function(action, subject) {
    if (action.id === "org.freedesktop.policykit.exec" &&
        action.lookup("program") === "/usr/bin/shadow-harden" &&
        (subject.isInGroup("wheel") || subject.isInGroup("sudo"))) {
        return polkit.Result.YES;
    }
});
PKEOF
log "Polkit rule written to ${POLKIT_RULES}."

# ---------------------------------------------------------------------------
# 11. Configure rkhunter (rootkit scanner) for daily scan
# ---------------------------------------------------------------------------
if command -v rkhunter >/dev/null 2>&1; then
    log "Configuring rkhunter…"
    rkhunter --update 2>/dev/null || warn "rkhunter --update failed (non-fatal)."
    rkhunter --propupd 2>/dev/null || warn "rkhunter --propupd failed (non-fatal)."
    # Enable daily cron scan
    if [ -f /etc/default/rkhunter ]; then
        sed -i 's/^CRON_DAILY_RUN=.*/CRON_DAILY_RUN="true"/' /etc/default/rkhunter 2>/dev/null || true
    fi
    log "rkhunter initialised."
fi

# ---------------------------------------------------------------------------
# Verification
# ---------------------------------------------------------------------------
log "--- File verification ---"
for f in \
    /usr/bin/shadow-harden \
    /etc/sysctl.d/91-shadow-extreme.conf \
    /etc/modprobe.d/shadow-blacklist.conf \
    /etc/audit/rules.d/90-shadow-audit.rules \
    /etc/apparmor.d/usr.bin.shadow-anon \
    /etc/security/faillock.conf \
    "${POLKIT_RULES}"
do
    if [ -e "${f}" ]; then
        ls -la "${f}"
    else
        warn "MISSING: ${f}"
    fi
done

log "shadow-harden hook done."
exit 0
