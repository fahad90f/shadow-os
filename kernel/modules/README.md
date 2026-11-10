# Out-of-tree kernel modules

NEXUS OS ships **no** out-of-tree kernel modules.

Hardware that needs a driver outside the mainline kernel is handled the Debian
way: a `*-dkms` package from the Debian archive builds the module against the
installed kernel headers (`linux-headers-*-nexus-*` are published next to the
NEXUS kernel image for exactly this purpose).

Two consequences are worth knowing:

* **Secure Boot.** With Secure Boot enabled the kernel is in lockdown and only
  loads signed modules. In-tree modules are signed at build time; DKMS modules
  are not, unless you sign them with a key you enrol as a Machine Owner Key
  (`mokutil --import`). This is the same behaviour as on Debian.
* **The build-time module key is discarded.** The private key that signs the
  in-tree modules exists only for the duration of the kernel build, so nobody —
  including the NEXUS project — can sign additional modules that the kernel
  would accept as "built with the kernel".

If a module ever has to be carried here, it goes in a sub-directory with its
own DKMS configuration and a justification in `docs/kernel/patches.md`.
