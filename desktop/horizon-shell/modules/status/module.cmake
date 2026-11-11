# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build additions of the "status" shell module (status indicators, quick
# settings, on-screen display, calendar). Included by the shell's
# CMakeLists.txt after the horizon-shell target exists.
#
#   KF6::PulseAudioQt   volume and microphone through the PulseAudio protocol
#                       (pipewire-pulse on SHADOW)
#   KF6::BluezQt        Bluetooth adapters and connected devices (BlueZ)
#   KF6::ConfigCore     switching KWin's Night Light on and off (kwinrc, with
#                       the change notification KWin listens to)
#
# NetworkManager, UPower, power-profiles-daemon, logind and MPRIS are reached
# with plain QtDBus (already linked through Shadow::App).
find_package(KF6PulseAudioQt REQUIRED)
find_package(KF6BluezQt REQUIRED)
find_package(KF6Config REQUIRED)

target_link_libraries(horizon-shell PRIVATE
    KF6::PulseAudioQt
    KF6::BluezQt
    KF6::ConfigCore
)
