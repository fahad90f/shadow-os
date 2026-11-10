# Live-session defaults consumed by live-config at boot.
mkdir -p /etc/live/config.conf.d
cat > /etc/live/config.conf.d/nexus.conf <<CONF
# NEXUS OS live session defaults (override on the kernel command line with
# live-config.username=..., live-config.hostname=..., etc.)
LIVE_USERNAME="nexus"
LIVE_USER_FULLNAME="NEXUS Live Session"
LIVE_HOSTNAME="nexus"
LIVE_USER_DEFAULT_GROUPS="audio cdrom dip floppy video plugdev netdev bluetooth lpadmin sudo wireshark kvm"
CONF

# The root account has no password; administration goes through sudo/polkit.
passwd -l root >/dev/null

# live-config only assigns LIVE_HOSTNAME when no hostname is configured, and
# the bootstrap tool copies the *build host's* name into the image — drop it.
# The installer writes the hostname the user chooses.
rm -f /etc/hostname
