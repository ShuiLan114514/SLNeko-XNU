# SLNeko-XNU — iOS 16.5 on the Redmi Note 11T Pro (xaga, MT6895)

This is the source tree for the **xaga** port of HakiOS: Apple's open-source Darwin/XNU
(``xnu-8796.121.2``, the iOS 16.5 kernel) booting on a Xiaomi Redmi Note 11T Pro / POCO X5 Pro
(MT6895).  The iOS 16.5 user space (20F66) runs unmodified: launchd → backboardd → SpringBoard.

The tree is derived from the HakiOS `mtk` branch.  The original upstream documentation is kept in
[`README.hakios.md`](README.hakios.md) and [`docs/`](docs/); this README only covers the xaga work.

## Status (11 November 2026)

| Area | State |
|---|---|
| Boot | XNU 8796.121.2 boots from `boot_a` through `mtkloader`; all-root/App launchd setup works. |
| Display | Sony/Novatek panel path works in graphics mode (`v_display=1`), SpringBoard draws the Hello/OOBE UI. |
| Touch | Novatek **NT36672E** on SPI2 (GPIO 109–112, IRQ GPIO135), firmware **`novatek_nt36672e_l16_fw01.bin`**; real multitouch reports work. Do not use fw02 on this panel: it downloads but its baseline stays at `a1` and reports only `0x77` fill. |
| OOBE | Hello → swipe → SpringBoard home screen; `bcatch` auto-swipe is disabled in the working root. |
| Logs | The loader writes an HLOG logbook to the last 8 MiB of the reserved tail of `iosroot` (a 200 MiB reserve). |
| Battery / charger | First implementation: MT6375 on I2C5 (GPIO33/34 bit-banged), VBAT + PWR_RDY + CHG_STAT, voltage-estimated SOC into `RedwoodBattery`. It is bring-up quality; the real MT6375 coulomb counter is not implemented yet. |
| Not a goal | Cellular, Bluetooth, camera capture, Apple online services/activation. |

## Hardware reference

- **SoC** MediaTek MT6895.
- **Touch** Novatek NT36672E, SPI2 `0x11012000`, MI/CS/MO/CLK = GPIO **109/110/111/112**,
  interrupt = GPIO **135**.  It is driven by bit-banging in `bsd/dev/redwood_nvt.c`.
- **Charger / gauge** MT6375 on **I2C5** (PMU address `0x34`, battery-monitor `0x1A`);
  SCL/SDA = GPIO **33/34**.
- **Display** 1080×2460, `v_display=1` graphics loader.

## Build

The kernel is built from an external Apple XNU tree plus `xnu/redwood.patch` and
`xnu/files/`:

```sh
xnu/apply.sh                 # fetch xnu-8796.121.2 and apply the port
xnu-deps/setup.sh            # host tools, MIG, tcsh, SDK shim
./xnu-build.sh -j8 -k        # -> xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837
userland/build.sh            # root.apfs / system image
```

The iOS system image is assembled from your own IPSW; Apple files and phone firmware are not
in this repository.  The NVT image header `xnu/files/bsd/dev/redwood.nvt_fw.h` is generated from
the vendor file pulled from *your own* phone:

```sh
# /vendor/firmware/novatek_nt36672e_l16_fw01.bin
python3 tools/mknvtfw.py --output xnu/files/bsd/dev/redwood.nvt_fw.h \
        novatek_nt36672e_l16_fw01.bin
```

`userland/build.sh` and the loader/flash helpers expect the Theos cctools/SDK at `$THEOS`.
See [`README.hakios.md`](README.hakios.md) and [`xnu/README.md`](xnu/README.md) for the full
toolchain notes.

## Flashing

The boot image is written to **`boot_a` only**.  Never flash or overwrite `boot_b`; it is the
Android fallback and must stay untouched.  `mtkloader` lives in `boot_a` ahead of the Android
kernel, and `iosroot` carries the iOS system image.

The logbook is read from Android with root:

```sh
fbdiag/pull-iosroot-logbook.sh
# or: dd if=/dev/block/by-name/iosroot of=logbook.bin bs=4096 \
#         skip=$(( (size/4096) - 2048 )) count=1025
```

## Credits

- **ShuiLan114514** — xaga port maintenance, NVT touch (fw01), graphics/OOBE bring-up,
  logbook workflow, MT6375 battery bring-up.
- **mytiantian001** — co-author / contributor.
- **HakiOS / oosexgang** — the upstream `hakios-xnu-kernel` `mtk` branch, loader, XNU
  compatibility work and documentation.
- **Apple** — XNU source and the iOS user space (not redistributed here).

## License

The XNU patch and files derived from XNU are under the Apple Public Source License 2.0.
Everything else is under MIT unless a file header says otherwise.  This is an independent
research project and is not affiliated with or endorsed by Apple, Xiaomi or MediaTek.
