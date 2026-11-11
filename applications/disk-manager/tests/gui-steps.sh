# SPDX-License-Identifier: GPL-3.0-or-later
#
# Interaction steps for gui-shots.sh (GUI_STEPS=/src/applications/disk-manager/tests/gui-steps.sh):
# select a partition, open the Format dialog, type the device name, confirm,
# then show free space and a disk that is protected because it is mounted at /boot.
# Coordinates are for the 1280x900 window that gui-shots.sh creates.
click 664 183
click 752 426
snap disks-dm-3-format-dialog.png
click 640 556 0.5
xdotool type --delay 40 "/dev/loop0p"
sleep 0.5
snap disks-dm-4-format-partial-name.png
xdotool type --delay 40 "2"
sleep 0.5
snap disks-dm-5-format-armed.png
xdotool key Return
sleep 4
snap disks-dm-6-formatted.png
echo "after GUI format: $(blkid -c /dev/null -o value -s TYPE ${A}p2) label=$(blkid -c /dev/null -o value -s LABEL ${A}p2)"
click 1043 183
snap disks-dm-7-free.png
mount "$B" /boot
click 120 128 2.5
snap disks-dm-8-protected.png
umount /boot
