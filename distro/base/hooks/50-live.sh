# Image-level adjustments for the live medium (session defaults themselves
# come from the shadow-live package).

# The root account has no password; administration goes through sudo/polkit.
passwd -l root >/dev/null

# live-config only assigns LIVE_HOSTNAME when no hostname is configured, and
# the bootstrap tool copies the *build host's* name into the image — drop it.
# The installer writes the hostname the user chooses.
rm -f /etc/hostname
