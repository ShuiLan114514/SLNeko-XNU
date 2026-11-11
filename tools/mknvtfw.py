#!/usr/bin/env python3
"""Generate the Novatek touch firmware header for the xaga Novatek NT36672E.

xaga uses novatek_nt36672e_l16_fw01.bin (the image Android itself downloads on this panel);
fw02 has a matching FWINFO but its baseline stalls at a1 and it reports only 0x77 fill.

Generate the Novatek touch firmware blob that redwood_nvt.c programs into the controller.

Why this is needed at all: the vendor driver does not trust the image in the controller's own
flash. After probing it queues Boot_Update_Firmware, and 14 seconds later that calls
nvt_update_firmware(), which rewrites ILM, DLM, the header block and both overlay sections into
the controller's SRAM -- on every single boot. So the "bootloader reset" the vendor issues before
reading the chip id is not a recovery step; the chip genuinely comes up without a usable image,
and any host that wants touch has to hand it one.

Measured on xaga with this driver: the chip answers its trim id (0e 00 00 72 66 03, which selects
NT36675_memory_map and hw_crc 2), the event buffer at 0x22D00 reads back stable bytes that never
checksum, the fw handshake at 0x22D51 answers neither 0xA0 nor 0x00, and the reset-state byte at
0x22D60 stays 0x3A across fifty 10 ms polls after an eng reset and a software reset. 0x3A is
below RESET_STATE_INIT 0xA0, i.e. the firmware never started.

usage: mtk/tools/mknvtfw.py [-o OUTPUT] [BIN]
       default BIN is the Tianma part from the vendor partition dump;
       default OUTPUT is drivers/input/hakios.nvt_fw.h

The partition table is emitted as constants rather than parsed at run time: it is 139264 bytes of
firmware, its header never changes for a given part, and a parser that can be wrong is worse than
four constant rows that were checked once by this script (see --table to print them).
"""

import argparse
import os
import struct
import sys

DEFAULT_BIN = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "novatek_nt36672e_l16_fw01.bin")
# Which image, and why this one: novatek_nt36672e_l16_fw02.bin.
#
# Three earlier guesses were wrong and the boot log settled it. On a normal Android boot,
# nvt_ts_resume calls nvt_update_firmware, which reaches nvt_match_fw / nvt_get_panel_type and
# prints
#
#   [21.254] nvt_get_panel_type 1240: match panle type, fw is [novatek_nt36672e_l16_fw02.bin]
#   [21.254] update_firmware_request 321: filename is novatek_nt36672e_l16_fw02.bin
#   [21.339] nvt_update_firmware 1180: Update firmware success! <83891 us>
#   [21.339] nvt_get_fw_info 754: FW type is 0x01, fw_ver=22
#
# So Android does download an image -- fw_ver=22 is the version of the file it just wrote, not a
# factory image living in the chip. fw01 (what cfg_0 in the device tree names for a Tianma panel)
# boots its identity but never leaves reset state a1: no baseline, no interrupt edges. fw02 runs,
# and the panel is the CSOT one: the lockdown info this board reports starts 0x53 0x42, which is
# cfg_1's tp-vendor 0x53 and display-maker 0x42. fw02 also carries 17 partitions (ovly_sec_num 2)
# against novatek_ts_fw.bin's 15, so the coordinate tables differ between the two -- which is why
# a matching FWINFO identity never guaranteed the axis scale.
# The header lives with the other kernel files, not with the SoC: drivers/map is what tells
# xnu/apply.sh and xnu/sync-drivers.sh where a .h goes, and a drivers/*/conf can only name .c
# files. drivers/input/hakios.focaltech_fw.h set that precedent for a firmware blob.
DEFAULT_OUT = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "xnu", "files", "bsd", "dev", "redwood.nvt_fw.h")

ILM_DLM_NUM = 2


def word(data, at):
    return struct.unpack_from("<I", data, at)[0]


def parse(data):
    """Reproduce nvt_bin_header_parser(): returns the list of (name, sram, size, bin, crc).

    Sizes are the raw `size` field. The vendor treats it as a last index rather than a count:
    nvt_write_firmware() adds one before writing, while nvt_set_bld_hw_crc() hands the raw value
    to the bootloader's LENGTH register. Both numbers are therefore kept apart here, and the
    header says which is which.
    """
    end = word(data, 0)
    pos = 0x30
    info_sec_num = 0
    while pos < end:
        info_sec_num += 1
        pos += 0x10
    ovly_info = (data[0x28] & 0x10) >> 4
    ovly_sec_num = (data[0x28] & 0x0F) if ovly_info else 0
    partition = ILM_DLM_NUM + ovly_sec_num + info_sec_num

    rows = []
    for i in range(partition):
        if i < ILM_DLM_NUM:
            bin_addr = word(data, 0 + i * 12)
            sram = word(data, 4 + i * 12)
            size = word(data, 8 + i * 12)
            crc = word(data, 0x18 + i * 4)
            name = "ILM" if i == 0 else "DLM"
        elif i < ILM_DLM_NUM + info_sec_num:
            at = 0x30 + 0x10 * (i - ILM_DLM_NUM)
            sram = word(data, at)
            size = word(data, at + 4)
            bin_addr = word(data, at + 8)
            crc = word(data, at + 12)
            name = "Header" if (bin_addr == 0 and size != 0) else "Info-%d" % (i - ILM_DLM_NUM)
        else:
            # overlay sections are described by the DLM's own payload
            at = word(data, 12) + 0x10 * (i - ILM_DLM_NUM - info_sec_num)
            sram = word(data, at)
            size = word(data, at + 4)
            bin_addr = word(data, at + 8)
            crc = word(data, at + 12)
            name = "Overlay-%d" % (i - ILM_DLM_NUM - info_sec_num)
        if bin_addr + size > len(data):
            raise SystemExit("partition %s runs past the end of the file" % name)
        rows.append((name, sram, size, bin_addr, crc))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bin", nargs="?", default=DEFAULT_BIN)
    ap.add_argument("-o", "--output", default=DEFAULT_OUT)
    ap.add_argument("--table", action="store_true", help="print the partition table and stop")
    args = ap.parse_args()

    with open(args.bin, "rb") as f:
        data = f.read()
    rows = parse(data)

    if args.table:
        for name, sram, size, bin_addr, crc in rows:
            print("%-10s sram %#08x  size %#07x (+1)  bin %#08x  crc %#010x%s"
                  % (name, sram, size, bin_addr, crc, "" if size else "  skipped, size is zero"))
        return 0

    # The two partitions that nvt_set_bld_hw_crc() describes to the bootloader: ILM then DLM.
    ilm = rows[0]
    dlm = rows[1]

    lines = []
    w = lines.append
    w("/* Generated by mtk/tools/mknvtfw.py from %s. Do not edit. */"
      % os.path.basename(args.bin))
    w("#ifndef _HAKIOS_NVT_FW_H")
    w("#define _HAKIOS_NVT_FW_H")
    w("")
    w("/*")
    w(" * The image the vendor driver programs into the controller on every boot, over SPI, into")
    w(" * the controller's SRAM. It is not a flash upgrade: the chip comes up in its bootloader,")
    w(" * and this is what it is given to run.")
    w(" *")
    w(" * The partition table is the one the firmware carries in its own header, resolved here so")
    w(" * that the driver does not have to reproduce nvt_bin_header_parser(). `size` is the raw")
    w(" * field: nvt_write_firmware() writes size + 1 bytes, while nvt_set_bld_hw_crc() gives the")
    w(" * raw value to the bootloader's LENGTH register. The difference is the vendor's, and both")
    w(" * numbers matter, so the driver adds the one where it writes and not where it describes.")
    w(" *")
    w(" * %-10s sram %#08x  bin %#08x  size %#x (+1 written)  crc %#010x" % (ilm[0], ilm[1], ilm[3], ilm[2], ilm[4]))
    w(" * %-10s sram %#08x  bin %#08x  size %#x (+1 written)  crc %#010x" % (dlm[0], dlm[1], dlm[3], dlm[2], dlm[4]))
    for name, sram, size, bin_addr, crc in rows[2:]:
        if not size:
            continue
        w(" * %-10s sram %#08x  bin %#08x  size %#x (+1 written)  crc %#010x" % (name, sram, bin_addr, size, crc))
    w(" */")
    w("")
    w("#define NVT_FW_LENGTH %d" % len(data))
    w("")
    w("/* One row per partition that carries data; the zero-sized ones are dropped. */")
    w("struct nvt_fw_part {")
    w("\tuint32_t sram;")
    w("\tuint32_t bin;")
    w("\tuint32_t size;")
    w("\tuint32_t crc;")
    w("};")
    w("")
    w("#define NVT_FW_PARTS %d" % len(rows))
    w("static const struct nvt_fw_part nvt_fw_parts[NVT_FW_PARTS] = {")
    for name, sram, size, bin_addr, crc in rows:
        w("\t{ %#08xu, %#08xu, %#xu, %#010xu },%s/* %s */"
          % (sram, bin_addr, size, crc, "" if size else " ", name))
    w("};")
    w("")
    w("#define NVT_FW_ILM_SRAM  %#08xu" % ilm[1])
    w("#define NVT_FW_ILM_SIZE  %#xu" % ilm[2])
    w("#define NVT_FW_ILM_CRC   %#010xu" % ilm[4])
    w("#define NVT_FW_DLM_SRAM  %#08xu" % dlm[1])
    w("#define NVT_FW_DLM_SIZE  %#xu" % dlm[2])
    w("#define NVT_FW_DLM_CRC   %#010xu" % dlm[4])
    w("")
    w("static const unsigned char nvt_fw[NVT_FW_LENGTH] = {")
    for at in range(0, len(data), 16):
        chunk = data[at:at + 16]
        w("\t" + " ".join("0x%02x," % b for b in chunk))
    w("};")
    w("")
    w("#endif /* _HAKIOS_NVT_FW_H */")
    w("")

    text = "\n".join(lines)
    with open(args.output, "w") as f:
        f.write(text)
    print("mknvtfw: %s -> %s (%d bytes of firmware, %d partitions, %d lines)"
          % (os.path.basename(args.bin), args.output, len(data), len(rows), text.count("\n")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
