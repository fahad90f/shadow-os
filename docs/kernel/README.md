# The SHADOW kernel

## Status

| | |
|---|---|
| Kernel in the ISO images | Debian's signed `linux-image-amd64` (6.12 series), unmodified |
| SHADOW kernel configuration | implemented; `kernel/build.sh --configure-only` verified for amd64 |
| SHADOW kernel build | **not completed.** A full build was started and did not finish in the build environment (disk and time limits). No SHADOW kernel package has been produced or booted |
| SHADOW kernel in the image | **no** |
| Secure Boot | a SHADOW kernel is unsigned unless you supply a key; Debian's kernel is signed |
| arm64 | fragments and cross-compile support exist in the script; never configured, built or booted |

Everything below describes what `kernel/build.sh` and the files in `kernel/`
implement. Only the configuration step has been run successfully.

## What the SHADOW kernel is

Debian's kernel **source** for the suite's longterm series (package
`linux-source-6.12`, the source Debian's security team maintains), built
with Debian's **configuration** for the architecture plus the SHADOW
fragments, with the patches listed in `kernel/patches/series` applied (none).
kernel.org is not contacted. Hardware support is therefore Debian's.

## The pipeline (`kernel/build.sh`)

```
sudo kernel/build.sh [--arch amd64|arm64] [--configure-only] [--jobs N]
```

Must run as root on an amd64 host (arm64 would be cross-compiled).

| Environment variable | Values | Effect |
|---|---|---|
| `SHADOW_KERNEL_DEBUGINFO` | `btf` (default), `none` | `btf` keeps Debian's debug-info settings (BTF type information, needed by CO-RE eBPF tools); the script's header estimates about 25 GB of disk. `none` disables debug info and BTF; about 6 GB and much faster |
| `SHADOW_SB_KEY`, `SHADOW_SB_CERT` | paths to a PEM key and certificate | when both are set, the kernel image is signed with `sbsign` |

Steps:

1. **Build chroot.** A dedicated Debian trixie chroot at
   `build/work/chroot-kernel` (mmdebstrap, variant `buildd`) with the
   toolchain, `linux-source-6.12`, `linux-config-6.12`, `sbsigntool` and, for
   arm64, the cross compiler. Reused and updated on later runs.
2. **Source.** Unpacks `/usr/src/linux-source-6.12.tar.xz` once into
   `/build/kernel-<arch>` inside the chroot and applies every patch named in
   `kernel/patches/series` with `patch -p1`. The list of applied patches is
   written to `applied-patches`.
3. **Configuration.**
   * starts from Debian's `config.amd64_none_amd64.xz`
     (`config.arm64_none_arm64.xz` for arm64);
   * clears keys that exist only on Debian's build machines:
     `SYSTEM_TRUSTED_KEYS=""`, `SYSTEM_REVOCATION_KEYS=""`,
     `MODULE_SIG_KEY="certs/signing_key.pem"`; sets `LOCALVERSION=""` and
     disables `LOCALVERSION_AUTO`;
   * with `SHADOW_KERNEL_DEBUGINFO=none` switches debug info and BTF off;
   * merges `kernel/config/shadow-common.config` and the architecture
     fragment (`kernel/x86_64/shadow.config` or `kernel/aarch64/shadow.config`)
     with `scripts/kconfig/merge_config.sh -m`, then runs `make olddefconfig`.
4. **Verification.** Every line of `kernel/config/required.list` is checked
   against the final `.config` (see below). Any mismatch aborts the build.
   `scripts/diffconfig` then writes the difference to Debian's configuration.
   With `--configure-only` the script stops here.
5. **Build.** `make bindeb-pkg` with `LOCALVERSION=-shadow-<arch>` and package
   version `<kernel version>-1+shadow<version without dots>` (for example
   `…-1+shadow010`), producing `linux-image-…` and `linux-headers-…` packages.
6. **Optional Secure Boot signature.** If a key and certificate were given,
   the `vmlinuz` inside the image package is signed with `sbsign` and the
   package is rebuilt. The state (`signed`/`unsigned`) is recorded.
7. **Meta package.** `linux-image-shadow-<arch>`, depending on exactly the
   image just built.

Output:

| Path | Content |
|---|---|
| `build/out/kernel/config-<arch>` | the final configuration |
| `build/out/kernel/config-<arch>.diff-from-debian` | what differs from Debian's configuration |
| `build/out/packages/linux-*.deb` | image, headers and meta package (debug and `linux-libc-dev` packages are not copied) — only after a complete build |

Nothing adds these packages to an image: no package list or variant
references `linux-image-shadow-*`.

## Configuration changes and their reasons

From the comments in the fragments. "Pinned" means Debian already has the
value; it is listed so that a later rebase cannot lose it silently.

### All architectures (`kernel/config/shadow-common.config`)

| Options | Effect | Reason / cost |
|---|---|---|
| `DEFAULT_HOSTNAME="shadow"` | host name before one is configured | identity |
| `HZ_1000`, `HZ=1000` (not `HZ_250`) | 1000 Hz timer tick | lower input and audio latency on an interactive workstation; small throughput cost |
| `PREEMPT=y`, `PREEMPT_DYNAMIC=y` (not `PREEMPT_VOLUNTARY`) | full preemption by default | same; still switchable at boot with `preempt=voluntary` |
| `SCHED_AUTOGROUP=y` | per-session scheduling groups | desktop responsiveness |
| `INIT_ON_ALLOC_DEFAULT_ON`, `INIT_STACK_ALL_ZERO` | zero heap and stack memory on allocation | stops leaks and use of uninitialised memory |
| `SLAB_FREELIST_RANDOM`, `SLAB_FREELIST_HARDENED`, `SHUFFLE_PAGE_ALLOCATOR`, `RANDOM_KMALLOC_CACHES` | randomised and hardened allocators | makes heap exploitation less reliable |
| `HARDENED_USERCOPY`, `FORTIFY_SOURCE` | bounds checks on copies to/from user space and on string functions | memory-safety hardening |
| `RANDOMIZE_BASE`, `RANDOMIZE_KSTACK_OFFSET`, `RANDOMIZE_KSTACK_OFFSET_DEFAULT` | KASLR; per-syscall kernel stack offset | address-space randomisation |
| `STACKPROTECTOR`, `STACKPROTECTOR_STRONG`, `VMAP_STACK`, `SCHED_STACK_END_CHECK` | stack canaries, guard pages, overflow detection | stack-overflow protection |
| `STRICT_KERNEL_RWX`, `STRICT_MODULE_RWX` | no writable+executable kernel memory | code-injection protection |
| `ZERO_CALL_USED_REGS` | clear caller-saved registers on function return | fewer usable ROP gadgets |
| `LIST_HARDENED`, `BUG_ON_DATA_CORRUPTION` | detected corruption of core structures is fatal for the offending task | do not continue on corrupted state |
| `UBSAN`, `UBSAN_BOUNDS` (not `UBSAN_TRAP`, `_SHIFT`, `_BOOL`, `_ENUM`) | array-bounds checks compiled in | violations are reported, not fatal |
| `SECURITY_DMESG_RESTRICT` | kernel log readable by privileged users only | information leaks |
| `BPF_UNPRIV_DEFAULT_OFF`, `BPF_JIT_ALWAYS_ON` | no unprivileged eBPF; JIT instead of interpreter | attack surface reachable by unprivileged code |
| not `LEGACY_TIOCSTI`, not `LDISC_AUTOLOAD` | no keystroke injection into terminals; no automatic loading of line disciplines | same |
| `STRICT_DEVMEM`, `IO_STRICT_DEVMEM` | `/dev/mem` restricted | same |
| `SECURITY`, `SECURITY_APPARMOR`, `SECURITY_YAMA`, `SECURITY_LANDLOCK`, `SECURITY_LOCKDOWN_LSM`, `SECURITY_LOCKDOWN_LSM_EARLY`, `AUDIT`, `AUDITSYSCALL`, `SECCOMP`, `SECCOMP_FILTER` | security modules and auditing | pinned: the security baseline depends on them |
| `MODULE_SIG`, `MODULE_SIG_ALL`, `MODULE_SIG_SHA512`, `MODULE_SIG_HASH="sha512"`, not `MODULE_SIG_FORCE` | all in-tree modules signed at build time | see "Module signing" |
| `IOMMU_DEFAULT_DMA_STRICT` (not `IOMMU_DEFAULT_DMA_LAZY`) | strict IOMMU TLB invalidation | closes the window in which a device can still reach memory that was just unmapped; costs throughput on very fast storage/network devices; `iommu.strict=0` on the kernel command line reverts it |

### amd64 (`kernel/x86_64/shadow.config`)

| Options | Effect | Reason / cost |
|---|---|---|
| `LEGACY_VSYSCALL_NONE` (not `_XONLY`) | no legacy vsyscall page | only decades-old static binaries use it; those stop working |
| `X86_KERNEL_IBT`, `X86_USER_SHADOW_STACK` | hardware control-flow integrity | used when the CPU supports it |
| `MITIGATION_PAGE_TABLE_ISOLATION`, `MITIGATION_RETPOLINE`, `MITIGATION_RETHUNK`, `MITIGATION_SLS` | speculative-execution mitigations | pins Debian's defaults and adds the straight-line-speculation mitigation |
| `IA32_EMULATION=y`, not `COMPAT_VDSO` | 32-bit programs keep working (Wine, legacy tools); no fixed-address compat vDSO | compatibility without the legacy vDSO |
| `INTEL_IOMMU`, `AMD_IOMMU` | IOMMU support | DMA protection, device passthrough for lab VMs. Whether the Intel IOMMU is on by default is left as Debian has it (on, except for integrated graphics); `intel_iommu=on` forces it |

### arm64 (`kernel/aarch64/shadow.config`) — never configured or built

`ARM64_PTR_AUTH`, `ARM64_PTR_AUTH_KERNEL`, `ARM64_BTI`, `ARM64_BTI_KERNEL`
(pointer authentication and branch target identification), `ARM64_MTE`
(memory tagging for user space), `ARM64_SW_TTBR0_PAN`, `UNMAP_KERNEL_AT_EL0`,
`MITIGATE_SPECTRE_BRANCH_HISTORY`, `ARM_SMMU_V3`.

### What actually differs from Debian (amd64)

Many of the options above are already set by Debian. In the verified
`--configure-only` run, `scripts/diffconfig` reported about 30 differing
options. The file `build/out/kernel/config-amd64.diff-from-debian` is
generated by that run and is the authoritative list; in the run on record
the functional changes were:

* `HZ` 250 → 1000, `PREEMPT_VOLUNTARY` → `PREEMPT`
* `BPF_JIT_ALWAYS_ON`, `RANDOM_KMALLOC_CACHES`, `ZERO_CALL_USED_REGS`: off → on
* `UBSAN` with `UBSAN_BOUNDS`: off → on
* `LDISC_AUTOLOAD`: on → off
* `IOMMU_DEFAULT_DMA_LAZY` → `IOMMU_DEFAULT_DMA_STRICT`
* module signature hash SHA-256 → SHA-512, `MODULE_SIG_ALL` set with a
  build-time key, Debian's trusted-key file names cleared
* `DEFAULT_HOSTNAME` → `"shadow"`

Regenerate the file to see the current list; it changes with every Debian
kernel update.

## `required.list` verification

`kernel/config/required.list` holds options that **must** be present with
exactly the given value in the final `.config`:

```
CONFIG_NAME=value
CONFIG_NAME is not set
CONFIG_NAME=value [amd64]        # only checked for the named architecture(s)
```

`merge_config.sh` and `olddefconfig` silently drop an option whose
dependencies are not met. The check turns that into a build failure with the
required and the found value printed.

The list has two parts:

* the SHADOW hardening and responsiveness options from the fragments;
* options a distribution kernel must not lose — boot, live medium,
  encryption, containers, virtualisation: `MODULES`, `EFI`, `EFI_STUB`,
  `BLK_DEV_INITRD`, `SQUASHFS`, `OVERLAY_FS`, `DM_CRYPT`, `EXT4_FS`,
  `VFAT_FS`, `NF_TABLES`, `USER_NS`, `CGROUPS`, `KVM`, `DRM`, `SND`, `WLAN`,
  `BT`, `USB_STORAGE`, `NVME_CORE`, `VIRTIO_BLK`. These come from Debian's
  configuration and are listed so that a mistake in a fragment is caught at
  build time and not at boot.

## Patch policy

`kernel/patches/series` is intentionally empty. Debian's source already
carries the out-of-tree hardening SHADOW wants (for example the
`perf_event_open` restriction and the Secure Boot lockdown integration);
everything else SHADOW changes is configuration.

A patch is added only when all of these hold:

1. it fixes or hardens something that configuration cannot;
2. it is small enough to be re-reviewed at every stable update;
3. it has been submitted upstream, or is not upstreamable for a stated
   reason.

The series file is in quilt format: one patch file name per line, relative
to `kernel/patches/`, applied in order with `-p1`.

## Out-of-tree modules

Shadow OS ships none (`kernel/modules/README.md`). Hardware that needs a
driver outside the mainline kernel is handled the Debian way, with a
`*-dkms` package from the Debian archive; the headers package built next to
the SHADOW kernel image exists for that.

## Module signing

* With `MODULE_SIG_ALL`, every in-tree module is signed during the build
  with a key generated for that build (`certs/signing_key.pem`). The script
  does not keep the key: it stays in the build tree inside the chroot and is
  not copied to the output. Nobody, including the project, can later sign an
  additional module that the kernel would accept as built with it — provided
  the build tree is deleted afterwards.
* Signatures are **not enforced** by default (`MODULE_SIG_FORCE` is off),
  because enforcing would block DKMS drivers such as the proprietary NVIDIA
  module.
* Under Secure Boot the kernel is in lockdown and only loads signed modules.
  DKMS modules then need a signature from a key you enrol as a Machine Owner
  Key (`mokutil --import`), as on Debian.

## Secure Boot implications

| Kernel | Signed by | Boots under Secure Boot |
|---|---|---|
| Debian's `linux-image-amd64` (shipped) | Debian | yes, through shim's embedded Debian key |
| SHADOW kernel, no key supplied | nobody | no — unless you enrol the image's hash or sign it yourself |
| SHADOW kernel with `SHADOW_SB_KEY`/`SHADOW_SB_CERT` | your key | only after the certificate is enrolled as a MOK on the machine |

The project has no Secure Boot signing key and no shim review of its own.
This, together with the incomplete build, is why the image ships Debian's
kernel. With Debian's kernel, the options in the tables above that are not
already Debian defaults are **not in effect**; the hardening that applies to
the shipped image is what `shadow-hardening` sets at run time through sysctl
and the kernel command line ([../security/hardening.md](../security/hardening.md)).

## Kernel updates

The shipped kernel is updated by Debian through `trixie-security` and
`trixie-updates` like any Debian package; see
[../security/updates.md](../security/updates.md).
