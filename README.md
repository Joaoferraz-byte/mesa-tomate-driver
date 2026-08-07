# MTM-1106 / T501 — Full-area Mode Activator for Linux and NixOS

This project provides a **USB mode activator**, not a new kernel driver. It was created for the tablet commercially identified as **MTM-1106**, which appears in Linux as `SZ PENG YI LTD. [T501]` with VID/PID `08f2:6811`. The investigated symptom is the simultaneous exposure of a large desktop region and a smaller Android/mobile region, with Linux using only the smaller area by default.

The solution sends `SET_REPORT` HID reports documented by the [`DIGImend/10moons-tools`](https://github.com/DIGImend/10moons-tools) tool. The default `digimend` profile sends a four-report sequence of eight bytes to HID interface `2`; the alternative `mx002` profile sends only the central report used by the [`marvinbelfort/mx002_linux_driver`](https://github.com/marvinbelfort/mx002_linux_driver) project. The program requires the expected VID/PID, requires HID interface number `2`, does not reset the tablet, and does not modify firmware or descriptors.

> **Validation Status:** The sequence has been audited against public implementations for the T501 family but still needs to be executed and validated on the user's physical MTM-1106. Do not treat a cursor change as proof of success; use the measurement criteria below.

## Windows Driver Reverse Engineering (`Tomate_Setup.exe`)

The Windows driver was extracted from the vendor installer (`Tomate_Setup.exe`, Inno Setup 5.5.7 unicode, app version `4.7.2.628`, internal service `TabletServiceV20`) and inspected statically. Its architecture explains why the tablet works on Windows and not on an unmodified Linux installation, and it is the basis for the `SET_REPORT` sequence above.

| Component | Identification | Role |
| --- | --- | --- |
| `TabletService.exe` | Delphi runtime (`TSETKEYST501`, `TTABLETSERVICEMAIN`); imports `CreateFileA`, `WriteFile`, `DeviceIoControl`, `RegisterDeviceNotificationA` | Tray/service that polls the HID device path (`\\.\GlobalRoot\Device\HID#...`) and listens for hotplug |
| `TabletCom_vc.dll` | Internal name `pencom_vc.dll`; exports `DetectDevice`, `ReadBuffer`, `WriteBuffer`, `EraseBlock` | The proprietary protocol layer: claims the tablet by VID/PID and sends/receives out-reports that switch the firmware into full work mode (pressure, buttons, full area) |
| `wintab32.dll`, `wisptis.exe`, `KWintab.dll` | Wacom WinTab API shim + Windows Ink wrapper | Exposes the activated tablet to drawing applications |
| `TSetting.ini` | Mapping `4095x4095` | Confirms a 12-bit internal digitizer resolution |

The key conclusion is that the tablet firmware boots in a **basic mode** (the small `205x137 mm` region, no pressure) and only `WriteBuffer`/`ReadBuffer` calls transition it into **work mode**. The exact payload bytes inside `pencom_vc.dll` are compiled by Delphi and are not readable as strings, so the payloads above are cross-validated against public implementations (`10moons-tools`, `mx002_linux_driver`, DIGImend issue [#722][6]) rather than extracted byte-for-byte. A further instrumented capture (usbmon under Windows, or Ghidra/radare on `pencom_vc.dll`) is the recommended next step to remove the last inference gap. The macOS driver (`Mac_tablet_driver.dmg`) could not be recovered: its APFS container lacks valid block headers (`NXXB` magic absent) and refuses to mount, which matches the vendor README note that "MAC OSX is not supported by current driver".

## Discoveries

The `libinput.txt` file provided by the user shows two tablet interfaces for the same USB ID: one with approximately `993x585 mm` and another with `205x137 mm`. The extracted Windows driver README separately mentions `Android Mode` and `Work mode`; the reverse engineering of that driver (section above) is what exposes the communication bytes. The DigiMend tool and public Linux drivers provide the necessary protocol evidence.

The complete sequence for the default profile is as follows:

| Order | Report Data | Operational Interpretation |
| --- | --- | --- |
| 1 | `08 04 1d 01 ff ff 06 2e` | Selection/discovery report for the T501 family |
| 2 | `08 03 00 ff f0 00 ff f0` | Report associated with full area by the mx002 driver |
| 3 | `08 06 01 00 00 00 00 00` | Intermediate step of the DigiMend sequence |
| 4 | `08 03 00 ff f0 00 ff f0` | Re-application of the full-area report |

All use `bmRequestType=0x21`, `bRequest=0x09`, `wValue=0x0308`, `wIndex=2`, and a 250 ms timeout. The semantic assignment of the bytes is partially inferred; what is confirmed is the sequence observed in public code and the association of the 8-byte report with a mode change.

## Safe Usage Before NixOS

The full capture roadmap and acceptance criteria are in [`TESTING.md`](./TESTING.md). Use it before enabling `autoStart`.

Compile and run local tests:

```bash
make check
```

The self-test does not open USB. To only view the bytes:

```bash
./mtm1106-mode --profile digimend --dry-run
./mtm1106-mode --profile mx002 --dry-run
```

Before opening the tablet, record the initial state. On a system with `libinput`, save at least:

```bash
lsusb -v -d 08f2:6811 > before-lsusb.txt
libinput debug-tablet > before-libinput.txt
udevadm info --export-db > before-udev.txt
```

The normal command must be run with the tablet connected and with sufficient privileges to claim the HID interface:

```bash
sudo ./mtm1106-mode --profile digimend
```

If the full sequence does not produce the expected result, disconnect and reconnect the tablet before any second experiment. Then compare the same diagnostics. The `mx002` profile is an alternative, not a calibration: do not run it repeatedly in sequence without reconnecting the device.

Minimum confirmation requires that the large area becomes the effective coordinate source again, approximately `993x585 mm`, and that the `205x137 mm` area stops being the only tablet interface used. Pressure, the two pen buttons, tablet keys, and reconnection must also be tested.

## NixOS Integration

The flake exports the package and the `nixosModules.default` module. In your `nix-conf`'s `flake.nix`, add the input:

```nix
inputs.mesa-tomate-driver.url = "github:Joaoferraz-byte/mesa-tomate-driver";
```

In the host that has the tablet, import the module:

```nix
self.nixosModules.mtm1106 = inputs.mesa-tomate-driver.nixosModules.default;
```

Then, include `self.nixosModules.mtm1106` in the `imports` list of `latitudeConfiguration` or the correct host and keep auto-activation disabled initially:

```nix
services."mtm1106-mode" = {
  enable = true;
  profile = "digimend";
  autoStart = false;
};
```

This installs `mtm1106-mode` on the system but requires manual execution. The first test after rebuild is:

```bash
sudo mtm1106-mode --profile digimend
```

Only after repeatedly confirming physical behavior should it be enabled:

```nix
services."mtm1106-mode" = {
  enable = true;
  profile = "digimend";
  autoStart = true;
};
```

With `autoStart`, a udev rule triggers a oneshot unit for `08f2:6811`. The helper refuses to act if it does not find HID interface `2` or if there is more than one compatible tablet without bus/address selection. The unit uses `NoNewPrivileges`, `PrivateTmp`, `ProtectHome`, and `ProtectSystem`; execution remains root only because the HID interface may be under the control of `hid-generic`.

## Rollback

To disable without changing the rest of the system, remove or set `enable = false` in `services."mtm1106-mode"`, remove the flake input if desired, and rebuild. The solution does not install a kernel module and does not write to firmware. If a graphical session is left without the tablet after an experiment, disconnect and reconnect the USB; the activator does not execute `reset` and should not persist a modification after power is removed.

## Limitations and False Positives

The VID/PID is shared by rebrands of the T501 chipset. Therefore, the project does not claim that every `08f2:6811` is an MTM-1106, although it additionally requires HID interface `2`. The sequence does not have a standardized read response that allows for automatic success declaration. The dimension shown by libinput may remain the same if the tablet is not re-enumerated, if the compositor is using an old interface, or if the command was sent to the wrong interface.

The project does not fix pressure, rotation, multi-monitor mapping, or event parser quality. These are later layers. First confirm the USB mode; only then adjust libinput, Niri, or a user driver. Do not add a scale transformation to hide a wrong area, as this would produce a visual false positive and lose physical resolution.

## References

[1]: https://docs.kernel.org/hid/hidintro.html "Linux Kernel: Introduction to HID report descriptors"
[2]: https://github.com/DIGImend/10moons-tools "DIGImend 10moons-tools"
[3]: https://github.com/marvinbelfort/mx002_linux_driver "mx002 Linux user-space driver"
[4]: https://github.com/f-caro/10moons-driver-vin1060plus "10moons/VINSA T501 reference driver"
[5]: https://bbs.archlinux.org/viewtopic.php?id=308509 "Arch Linux: Graphic tablet cuts its work area [SOLVED]"
[6]: https://github.com/DIGImend/digimend-kernel-drivers/issues/722 "DIGImend issue #722: MTM-1106 / T501"
[7]: https://github.com/Joaoferraz-byte/nix-conf "User NixOS configuration"
