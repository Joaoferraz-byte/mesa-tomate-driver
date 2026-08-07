# Physical Validation Protocol

This protocol must be executed on the machine with the MTM-1106 connected. The agent session did not have access to physical hardware; therefore, the results below are criteria and commands, not a confirmation of functioning on the user's device.

## 1. Initial State Capture

Connect the tablet without running `mtm1106-mode` and save USB, udev, and libinput data. Create a directory outside the repository to avoid committing local data:

```bash
mkdir -p ~/tablet-test/before
lsusb -v -d 08f2:6811 > ~/tablet-test/before/lsusb.txt
libinput list-devices > ~/tablet-test/before/devices.txt
udevadm info --export-db > ~/tablet-test/before/udev-db.txt
```

Also record `uname -a`, `nixos-version`, `ls -l /dev/hidraw*`, and the output of `journalctl -k -b | grep -Ei '08f2|6811|hid|usb'`. The diagnosis provided before implementation indicates that the expected intervals are approximately `993x585 mm` for the desktop region and `205x137 mm` for the mobile region.

## 2. Activation Test

Use the binary installed by the flake or the local build binary. Run only once:

```bash
sudo mtm1106-mode --profile digimend
```

The program should report four `SET_REPORT` sent, without resetting the tablet. If there is a permission error, disconnect the tablet, fix the installation/udev, and reconnect; do not repeat the sequence on a device whose state is unclear.

## 3. Acceptance Criteria

Consider the profile confirmed only if all of the following criteria are true:

| Verification | Required Result |
| --- | --- |
| Identification | The device remains `08f2:6811`/`T501`; no other tablet was changed. |
| Area | The large desktop region is the effective coordinate source, approximately `993x585 mm`; the `205x137 mm` region is not the only active area. |
| Resolution | No artificial scaling was applied in libinput, compositor, or `xinput` to simulate the result. |
| Events | Pressure, proximity, click, and the two pen buttons remain operational. |
| Keys | Physical buttons continue to generate expected events or, at a minimum, were not lost. |
| Stability | Disconnecting/reconnecting does not leave the kernel, libinput, or graphical session in a broken state. |

A cursor proportion change without confirmation of interval and events is a **false positive**.

## 4. Alternative Profile (mx002)

If the `digimend` profile fails, disconnect and reconnect the tablet, save a new `before-mx002` state, and run only once:

```bash
sudo mtm1106-mode --profile mx002
```

This profile sends only `08 03 00 ff f0 00 ff f0`, the minimum sequence used by the `mx002_linux_driver` Rust driver. It is not a calibration and should not be combined with the full profile without reconnecting the device.

## 5. Automatic Test on NixOS

Only after manual validation, change on the latitude host:

```nix
services."mtm1106-mode".autoStart = true;
```

Rebuild and observe the unit when reconnecting the tablet:

```bash
systemctl status mtm1106-mode.service
journalctl -u mtm1106-mode.service
```

If there is more than one `08f2:6811` tablet, do not enable automatic mode: the helper refuses to select between multiple devices without `--bus`/`--address`, and the udev unit should not be used in this scenario.

## 6. Rollback

To return to original behavior, set `autoStart = false` or `enable = false`, rebuild, and disconnect/reconnect the tablet. The project does not write firmware or change persistent descriptors. If behavior becomes inconsistent during the session, remove USB power before any new experiment.

## 7. Evidence to Send for Investigation

If the mode is not activated, send only the text files from the `before`, `after-digimend`, and, if applicable, `after-mx002` directories, as well as filtered `journalctl`. Do not send personal data or the full udev database if it contains other devices; prefer extracting lines from the `08f2:6811` USB path.
