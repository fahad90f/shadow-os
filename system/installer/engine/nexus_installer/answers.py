# SPDX-License-Identifier: GPL-3.0-or-later
"""Validation of the installer answer file.

The answer file is the complete, declarative description of an installation.
It is produced by the graphical installer or written by hand for unattended
installs (docs/installation/unattended.md). Validation is strict: an unknown
key or an out-of-range value is an error, never silently ignored — a typo in
a storage setting must not turn into a different disk layout.
"""
from __future__ import annotations

import os
import re
from dataclasses import dataclass, field

from . import ANSWERS_VERSION
from .util import InstallError

USERNAME_RE = re.compile(r"^[a-z_][a-z0-9_-]{0,31}$")
HOSTNAME_RE = re.compile(r"^(?=.{1,63}$)[a-z0-9]([a-z0-9-]*[a-z0-9])?$")
LOCALE_RE = re.compile(r"^[a-z]{2,3}(_[A-Z]{2})?(\.UTF-8)?(@[a-z]+)?$")
XKB_RE = re.compile(r"^[A-Za-z0-9_-]{1,32}$")
TIMEZONE_RE = re.compile(r"^[A-Za-z0-9_+-]+(/[A-Za-z0-9_+-]+){0,2}$")
DEVICE_RE = re.compile(r"^/dev/[A-Za-z0-9/_.-]+$")
PROFILE_RE = re.compile(r"^[a-z0-9][a-z0-9-]{0,40}$")
CMDLINE_RE = re.compile(r"^[A-Za-z0-9_.,=:/@+ -]*$")

RESERVED_USERS = {
    "root", "daemon", "bin", "sys", "sync", "games", "man", "lp", "mail", "news", "uucp", "proxy",
    "www-data", "backup", "list", "irc", "nobody", "systemd-network", "systemd-timesync",
    "messagebus", "sddm", "polkitd", "avahi", "nexus-live", "admin", "adm", "sudo", "staff", "users",
}

MOUNTPOINT_CHOICES = ("/", "/boot", "/boot/efi", "/home", "/var", "/srv", "/opt", "/tmp")
FORMAT_FILESYSTEMS = ("ext4", "vfat")
KEEP_FILESYSTEMS = ("ext4", "ext3", "xfs", "btrfs", "vfat", "f2fs")


@dataclass
class ManualMount:
    device: str
    mountpoint: str | None          # None for swap
    use_as_swap: bool
    format: bool
    filesystem: str | None


@dataclass
class Storage:
    mode: str                       # erase-disk | free-space | manual
    disk: str | None = None
    encrypt: bool = False
    passphrase: str | None = None
    separate_home: bool = False
    root_size_gib: int | None = None
    swap: str = "file"              # file | partition | none
    swap_size_gib: int | None = None
    mounts: list[ManualMount] = field(default_factory=list)
    bootloader_device: str | None = None


@dataclass
class User:
    fullname: str
    username: str
    password: str | None
    password_hash: str | None
    autologin: bool


@dataclass
class Answers:
    locale: str
    keyboard_layout: str
    keyboard_variant: str
    keyboard_model: str
    timezone: str
    hostname: str
    user: User
    storage: Storage
    install_bootloader: bool
    bootloader_removable: str        # auto | yes | no
    os_prober: bool
    extra_kernel_cmdline: str
    profiles: list[str]
    install_profiles_now: bool


def _expect_keys(obj: dict, allowed: set[str], where: str):
    if not isinstance(obj, dict):
        raise InstallError(f"Answer file: '{where}' must be an object.")
    unknown = set(obj) - allowed
    if unknown:
        raise InstallError(f"Answer file: unknown key(s) in {where}: {', '.join(sorted(unknown))}.")


def _bool(obj: dict, key: str, default: bool, where: str) -> bool:
    value = obj.get(key, default)
    if not isinstance(value, bool):
        raise InstallError(f"Answer file: {where}.{key} must be true or false.")
    return value


def _str(obj: dict, key: str, default: str | None, where: str, *, required: bool = False) -> str | None:
    value = obj.get(key, default)
    if value is None:
        if required:
            raise InstallError(f"Answer file: {where}.{key} is required.")
        return None
    if not isinstance(value, str):
        raise InstallError(f"Answer file: {where}.{key} must be a string.")
    return value


def _int(obj: dict, key: str, where: str, lo: int, hi: int) -> int | None:
    value = obj.get(key)
    if value is None or value == "auto":
        return None
    if isinstance(value, bool) or not isinstance(value, int) or not lo <= value <= hi:
        raise InstallError(f"Answer file: {where}.{key} must be an integer between {lo} and {hi} (or \"auto\").")
    return value


def _device(value: str | None, where: str) -> str:
    if not value or not DEVICE_RE.fullmatch(value) or ".." in value:
        raise InstallError(f"Answer file: {where} must be a block device path such as /dev/nvme0n1.")
    return value


def parse(data: dict, *, zoneinfo_dir: str = "/usr/share/zoneinfo",
          supported_locales: str = "/usr/share/i18n/SUPPORTED") -> Answers:
    _expect_keys(data, {"version", "locale", "keyboard", "timezone", "hostname", "user", "storage",
                        "bootloader", "kernel", "packages"}, "the top level")
    if data.get("version") != ANSWERS_VERSION:
        raise InstallError(f"Answer file: unsupported version {data.get('version')!r} (expected {ANSWERS_VERSION}).")

    # ---- localisation ----------------------------------------------------
    locale = _str(data, "locale", "en_US.UTF-8", "top level")
    if not LOCALE_RE.fullmatch(locale):
        raise InstallError(f"Answer file: invalid locale '{locale}'.")
    if not locale.endswith(".UTF-8") and "@" not in locale:
        locale += ".UTF-8"
    if os.path.isfile(supported_locales):
        with open(supported_locales, encoding="utf-8", errors="replace") as fh:
            known = dict(line.split()[:2] for line in fh
                         if len(line.split()) >= 2 and not line.startswith("#"))
        # Some UTF-8 locales are listed without the ".UTF-8" suffix (hi_IN, vi_VN, ...).
        bare = locale[:-len(".UTF-8")] if locale.endswith(".UTF-8") else locale
        if locale not in known and known.get(bare) == "UTF-8":
            locale = bare
        if known.get(locale) != "UTF-8":
            raise InstallError(f"Answer file: locale '{locale}' is not available on this system.")

    kb = data.get("keyboard", {})
    _expect_keys(kb, {"layout", "variant", "model"}, "keyboard")
    layout = _str(kb, "layout", "us", "keyboard")
    variant = _str(kb, "variant", "", "keyboard") or ""
    model = _str(kb, "model", "pc105", "keyboard")
    for name, value in (("layout", layout), ("model", model)):
        if not XKB_RE.fullmatch(value):
            raise InstallError(f"Answer file: invalid keyboard {name} '{value}'.")
    if variant and not XKB_RE.fullmatch(variant):
        raise InstallError(f"Answer file: invalid keyboard variant '{variant}'.")

    timezone = _str(data, "timezone", "Etc/UTC", "top level")
    if not TIMEZONE_RE.fullmatch(timezone) or ".." in timezone:
        raise InstallError(f"Answer file: invalid time zone '{timezone}'.")
    if os.path.isdir(zoneinfo_dir) and not os.path.isfile(os.path.join(zoneinfo_dir, timezone)):
        raise InstallError(f"Answer file: unknown time zone '{timezone}'.")

    hostname = _str(data, "hostname", "nexus", "top level")
    if not HOSTNAME_RE.fullmatch(hostname):
        raise InstallError(f"Answer file: invalid host name '{hostname}' "
                           "(lower-case letters, digits and hyphens; at most 63 characters).")

    # ---- user --------------------------------------------------------------
    u = data.get("user")
    if u is None:
        raise InstallError("Answer file: 'user' is required.")
    _expect_keys(u, {"fullname", "username", "password", "password_hash", "autologin"}, "user")
    username = _str(u, "username", None, "user", required=True)
    if not USERNAME_RE.fullmatch(username) or username in RESERVED_USERS:
        raise InstallError(f"Answer file: '{username}' is not a valid user name "
                           "(start with a lower-case letter; letters, digits, '-' and '_'; not a system account).")
    fullname = _str(u, "fullname", "", "user") or ""
    if any(c in fullname for c in ":,=\n\r") or len(fullname) > 120:
        raise InstallError("Answer file: user.fullname must not contain ':', ',', '=' or line breaks.")
    password = _str(u, "password", None, "user")
    password_hash = _str(u, "password_hash", None, "user")
    if bool(password) == bool(password_hash):
        raise InstallError("Answer file: give exactly one of user.password and user.password_hash.")
    if password is not None and ("\n" in password or "\r" in password):
        raise InstallError("Answer file: user.password must not contain line breaks.")
    if password_hash is not None and not re.fullmatch(r"^\$(y|6|5|2[aby]|7|gy)\$[A-Za-z0-9./$=,+-]+$", password_hash):
        raise InstallError("Answer file: user.password_hash is not a crypt(5) hash (yescrypt or SHA-512 expected).")
    user = User(fullname, username, password, password_hash, _bool(u, "autologin", False, "user"))

    # ---- storage -----------------------------------------------------------
    s = data.get("storage")
    if s is None:
        raise InstallError("Answer file: 'storage' is required.")
    _expect_keys(s, {"mode", "disk", "encrypt", "passphrase", "separate_home", "root_size_gib", "swap",
                     "swap_size_gib", "mounts", "bootloader_device"}, "storage")
    mode = _str(s, "mode", None, "storage", required=True)
    if mode not in ("erase-disk", "free-space", "manual"):
        raise InstallError("Answer file: storage.mode must be 'erase-disk', 'free-space' or 'manual'.")
    storage = Storage(mode=mode)
    storage.encrypt = _bool(s, "encrypt", False, "storage")
    storage.passphrase = _str(s, "passphrase", None, "storage")
    storage.separate_home = _bool(s, "separate_home", False, "storage")
    storage.root_size_gib = _int(s, "root_size_gib", "storage", 16, 4096)
    storage.swap = _str(s, "swap", "file", "storage")
    if storage.swap not in ("file", "partition", "none"):
        raise InstallError("Answer file: storage.swap must be 'file', 'partition' or 'none'.")
    storage.swap_size_gib = _int(s, "swap_size_gib", "storage", 1, 256)

    if storage.encrypt:
        if not storage.passphrase or len(storage.passphrase) < 8:
            raise InstallError("Answer file: disk encryption needs storage.passphrase of at least 8 characters.")
        if "\n" in storage.passphrase or "\r" in storage.passphrase:
            raise InstallError("Answer file: storage.passphrase must not contain line breaks.")
    elif storage.passphrase:
        raise InstallError("Answer file: storage.passphrase is set but storage.encrypt is false.")

    if mode in ("erase-disk", "free-space"):
        storage.disk = _device(_str(s, "disk", None, "storage"), "storage.disk")
        if s.get("mounts") or s.get("bootloader_device"):
            raise InstallError(f"Answer file: storage.mounts/bootloader_device are only valid in manual mode.")
    else:
        if storage.encrypt:
            raise InstallError("Manual partitioning with encryption is not supported yet: use 'erase-disk' or "
                               "'free-space' for an encrypted installation.")
        if storage.separate_home or storage.root_size_gib or s.get("disk"):
            raise InstallError("Answer file: storage.disk/separate_home/root_size_gib do not apply to manual mode; "
                               "list the partitions under storage.mounts.")
        if storage.swap == "partition":
            raise InstallError("Answer file: in manual mode select a swap partition with "
                               "{\"device\": ..., \"use\": \"swap\"} under storage.mounts and set storage.swap to "
                               "'none' (or 'file' for a swap file).")
        raw = s.get("mounts")
        if not isinstance(raw, list) or not raw:
            raise InstallError("Answer file: manual mode needs a non-empty storage.mounts list.")
        seen_dev, seen_mp = set(), set()
        for i, m in enumerate(raw):
            where = f"storage.mounts[{i}]"
            _expect_keys(m, {"device", "mountpoint", "use", "format", "filesystem"}, where)
            dev = _device(_str(m, "device", None, where), f"{where}.device")
            use = _str(m, "use", "filesystem", where)
            if use not in ("filesystem", "swap"):
                raise InstallError(f"Answer file: {where}.use must be 'filesystem' or 'swap'.")
            fmt = _bool(m, "format", False, where)
            fs = _str(m, "filesystem", None, where)
            mp = _str(m, "mountpoint", None, where)
            if dev in seen_dev:
                raise InstallError(f"Answer file: {dev} is listed more than once in storage.mounts.")
            seen_dev.add(dev)
            if use == "swap":
                if mp:
                    raise InstallError(f"Answer file: {where} is swap and must not have a mountpoint.")
                storage.mounts.append(ManualMount(dev, None, True, True, "swap"))
                continue
            if mp not in MOUNTPOINT_CHOICES:
                raise InstallError(f"Answer file: {where}.mountpoint must be one of {', '.join(MOUNTPOINT_CHOICES)}.")
            if mp in seen_mp:
                raise InstallError(f"Answer file: mount point {mp} is assigned twice.")
            seen_mp.add(mp)
            if fmt:
                fs = fs or ("vfat" if mp == "/boot/efi" else "ext4")
                if fs not in FORMAT_FILESYSTEMS or (fs == "vfat") != (mp == "/boot/efi"):
                    raise InstallError(f"Answer file: {where}: {mp} can be formatted as "
                                       f"{'vfat' if mp == '/boot/efi' else 'ext4'} only.")
            elif fs is not None:
                raise InstallError(f"Answer file: {where}.filesystem is only meaningful with \"format\": true "
                                   "(an existing filesystem is detected, not declared).")
            storage.mounts.append(ManualMount(dev, mp, False, fmt, fs))
        if "/" not in seen_mp:
            raise InstallError("Answer file: manual mode needs a partition mounted at '/'.")
        root = next(m for m in storage.mounts if m.mountpoint == "/")
        if not root.format:
            raise InstallError("The root partition ('/') must be formatted: installing over an existing "
                               "system's files is not supported. Set \"format\": true for it.")
        bd = _str(s, "bootloader_device", None, "storage")
        storage.bootloader_device = _device(bd, "storage.bootloader_device") if bd else None

    # ---- boot loader / kernel / packages -----------------------------------
    b = data.get("bootloader", {})
    _expect_keys(b, {"install", "removable", "os_prober"}, "bootloader")
    removable = _str(b, "removable", "auto", "bootloader")
    if removable not in ("auto", "yes", "no"):
        raise InstallError("Answer file: bootloader.removable must be 'auto', 'yes' or 'no'.")

    k = data.get("kernel", {})
    _expect_keys(k, {"extra_cmdline"}, "kernel")
    extra = _str(k, "extra_cmdline", "", "kernel") or ""
    if not CMDLINE_RE.fullmatch(extra) or len(extra) > 512:
        raise InstallError("Answer file: kernel.extra_cmdline contains unsupported characters.")

    p = data.get("packages", {})
    _expect_keys(p, {"profiles", "install_now"}, "packages")
    profiles = p.get("profiles", [])
    if not isinstance(profiles, list) or not all(isinstance(x, str) and PROFILE_RE.fullmatch(x) for x in profiles):
        raise InstallError("Answer file: packages.profiles must be a list of profile names.")

    return Answers(
        locale=locale, keyboard_layout=layout, keyboard_variant=variant, keyboard_model=model,
        timezone=timezone, hostname=hostname, user=user, storage=storage,
        install_bootloader=_bool(b, "install", True, "bootloader"),
        bootloader_removable=removable, os_prober=_bool(b, "os_prober", True, "bootloader"),
        extra_kernel_cmdline=extra.strip(), profiles=list(dict.fromkeys(profiles)),
        install_profiles_now=_bool(p, "install_now", False, "packages"),
    )
