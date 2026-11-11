// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.shadow.ui
import "Fmt.js" as Fmt

// Section "secureboot" plus the running kernel from section "hardening".
SectionPage {
    id: page

    signal openPage(string key)

    readonly property var sb: ready ? section : ({})
    readonly property bool uefi: sb.firmware === "uefi"
    readonly property var hardening: Fmt.has(center.report.hardening) && center.report.hardening.available !== false
        ? center.report.hardening : null

    sectionKey: "secureboot"
    title: qsTr("Secure Boot and boot chain")
    sectionTitle: qsTr("The Secure Boot state")
    subtitle: qsTr("Whether the firmware verifies the boot loader and kernel, and how far the running kernel is locked down.")

    NxBanner {
        visible: page.ready && Fmt.has(page.sb.boot_loader_refresh_failed) && page.sb.boot_loader_refresh_failed !== ""
        kind: "danger"
        title: qsTr("The boot loader could not be updated")
        message: qsTr("A GRUB or shim update was installed but could not be copied to the boot partition (since %1). In a terminal: sudo shadow-grub-install --verbose")
            .arg(page.sb.boot_loader_refresh_failed || "")
    }

    NxSection {
        visible: page.ready
        title: qsTr("Firmware")
        NxRow {
            icon: "cpu"
            title: qsTr("Firmware mode")
            subtitle: page.uefi ? qsTr("/sys/firmware/efi exists.") : qsTr("/sys/firmware/efi does not exist.")
            value: page.uefi ? qsTr("UEFI") : page.sb.firmware === "bios" ? qsTr("Legacy BIOS") : (page.sb.firmware || qsTr("not reported"))
        }
        NxRow {
            icon: "key"
            iconColor: page.sb.enabled === true ? Theme.success : Theme.textSecondary
            title: qsTr("Secure Boot")
            subtitle: !page.uefi ? (page.sb.reason || "")
                : page.sb.supported === false ? qsTr("The firmware does not expose the SecureBoot variable, so the state cannot be read.")
                : qsTr("EFI variable SecureBoot.")
            NxBadge {
                text: !page.uefi ? qsTr("Not supported")
                    : Fmt.tri(page.sb.enabled, qsTr("Enabled"), qsTr("Disabled"), qsTr("Not determined"))
                kind: !page.uefi ? "neutral" : Fmt.tri(page.sb.enabled, "success", "warning", "neutral")
            }
        }
        NxRow {
            visible: page.uefi
            title: qsTr("Setup mode")
            subtitle: qsTr("In setup mode the firmware accepts new Secure Boot keys without authentication.")
            NxBadge {
                text: Fmt.tri(page.sb.setup_mode, qsTr("In setup mode"), qsTr("Off"), qsTr("Not determined"))
                kind: Fmt.tri(page.sb.setup_mode, "warning", "success", "neutral")
            }
        }
    }

    NxSection {
        visible: page.ready && page.uefi
        title: qsTr("Boot chain")
        NxRow {
            title: qsTr("shim boot loader")
            subtitle: qsTr("The Microsoft-signed first stage, looked for in /boot/efi/EFI/*/shim*.efi.")
            value: page.sb.shim_installed === true ? qsTr("Installed") : qsTr("Not found")
        }
        NxRow {
            title: qsTr("Machine owner keys (MOK)")
            subtitle: qsTr("Keys enrolled on this machine that may sign boot loaders, kernels and modules.")
            value: Fmt.has(page.sb.mok_keys) ? String(page.sb.mok_keys) : qsTr("not determined — mokutil could not list them")
        }
    }

    NxSection {
        visible: page.ready
        title: qsTr("Running kernel")
        NxRow {
            icon: "terminal"
            title: qsTr("Kernel release")
            subtitle: page.hardening ? "" : qsTr("Reported with the hardening section, which is not available.")
            value: page.hardening ? (page.hardening.kernel || qsTr("not reported")) : qsTr("not reported")
            monoValue: page.hardening !== null
        }
        NxRow {
            title: qsTr("Kernel lockdown")
            subtitle: !page.uefi ? qsTr("The status tool reads the lockdown mode only on UEFI systems.")
                : qsTr("Restricts what even root may do to the running kernel (/sys/kernel/security/lockdown).")
            NxBadge {
                visible: page.uefi
                text: Fmt.has(page.sb.kernel_lockdown) ? page.sb.kernel_lockdown : qsTr("Not determined")
                kind: page.sb.kernel_lockdown === "integrity" || page.sb.kernel_lockdown === "confidentiality" ? "success" : "neutral"
            }
            Text {
                visible: !page.uefi
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("not reported")
                color: Theme.textSecondary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
            }
        }
        NxRow {
            visible: page.hardening !== null
            title: qsTr("Kernel taint flags")
            subtitle: page.hardening && page.hardening.tainted > 0
                ? qsTr("Non-zero: an out-of-tree, unsigned or proprietary module was loaded, or the kernel reported a fault (/proc/sys/kernel/tainted).")
                : qsTr("/proc/sys/kernel/tainted")
            value: page.hardening && Fmt.has(page.hardening.tainted) ? String(page.hardening.tainted) : qsTr("not determined")
            monoValue: true
        }
        NxRow {
            title: qsTr("Kernel image signature")
            subtitle: Fmt.has(page.sb.kernel_signed)
                ? (page.sb.kernel_image || "")
                : qsTr("Whether the running kernel image is signed is not part of the status report. In a terminal: sbverify --list /boot/vmlinuz-$(uname -r)")
            value: Fmt.has(page.sb.kernel_signed) ? (page.sb.kernel_signed ? qsTr("Signed") : qsTr("Not signed")) : qsTr("not reported")
        }
        NxRow {
            title: qsTr("Kernel hardening settings")
            subtitle: qsTr("Boot parameters, sysctl values and CPU mitigations of this kernel.")
            chevron: true
            onClicked: page.openPage("hardening")
        }
    }
}
