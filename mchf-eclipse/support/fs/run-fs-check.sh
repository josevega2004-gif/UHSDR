#!/bin/sh
# Judge drivers/fs/fat32.c by what other implementations make of its output.
#
# fat32.c agreeing with its own reader proves nothing beyond self-consistency, so this script builds a
# volume with dosfstools' mkfs.vfat, lets fat32.c write into it, and then asks (a) fsck.vfat whether the
# result is a sane filesystem and (b) - if this machine allows it - the kernel's own FAT driver, through
# a loop mount, to read the bytes back.  The kernel check is the one that matches what happens when the
# stick goes from the radio to a PC.  Where it cannot run, the script says so instead of pretending.
set -u
cd "$(dirname "$0")"
IMG=fs-check.img
REF=fs-check-ref.bin
BIN=fat32_test
rc=0

command -v mkfs.vfat >/dev/null 2>&1 || { echo "fs-check: mkfs.vfat is missing, cannot judge anything"; exit 2; }
command -v fsck.vfat >/dev/null 2>&1 || { echo "fs-check: fsck.vfat is missing, cannot judge anything"; exit 2; }

cc=${CC:-gcc}
if ! $cc -O1 -g -std=c99 -Wall -Wextra -Wno-unused-parameter -o $BIN fat32_test.c ../../drivers/fs/fat32.c -lm; then
    echo "fs-check: the harness did not build"
    exit 1
fi

run_case() {
    label="$1"; size_mb="$2"; spc="$3"; want="$4"   # want: ok or refuse
    rm -f "$IMG"
    dd if=/dev/zero of="$IMG" bs=1M count="$size_mb" conv=sparse status=none
    if [ "$want" = ok ]; then
        if ! mkfs.vfat -F 32 -s "$spc" "$IMG" >/dev/null 2>&1; then
            echo "fs-check [$label]: mkfs.vfat refused $size_mb MB at $spc sectors/cluster, skipping"
            return 0
        fi
        ./$BIN "$IMG" "$REF" || { echo "fs-check [$label]: harness reported failures"; rc=1; return 0; }
        if fsck.vfat -v "$IMG" >fsck.out 2>&1; then
            if grep -qiE "error|corrupt|fat mismatch|cluster out of range" fsck.out; then
                echo "fs-check [$label]: fsck.vfat exited 0 but reported: $(head -3 fsck.out)"
                rc=1
            else
                echo "fs-check [$label]: fsck.vfat clean on the volume fat32.c wrote into"
            fi
        else
            echo "fs-check [$label]: fsck.vfat REJECTED the volume. Its words:"
            sed -n '1,12p' fsck.out
            rc=1
        fi
        # The kernel's FAT driver, if a user is allowed to loop-mount here.
        LOOP=$(udisksctl loop-setup -f "$IMG" 2>/dev/null | sed -n 's/.* as \(\/dev\/loop[0-9]*\).*/\1/p')
        if [ -n "${LOOP:-}" ]; then
            MNT=$(udisksctl mount -b "$LOOP" 2>/dev/null | sed -n 's/.* at \(.*\)$/\1/p')
            if [ -n "${MNT:-}" ] && [ -f "$MNT/APT01234.PNG" ]; then
                if cmp -s "$MNT/APT01234.PNG" "$REF"; then
                    echo "fs-check [$label]: the kernel's vfat driver read back the same $(stat -c%s "$MNT/APT01234.PNG") bytes fat32.c wrote"
                else
                    echo "fs-check [$label]: THE KERNEL READ SOMETHING DIFFERENT from what fat32.c wrote"
                    rc=1
                fi
                if [ -f "$MNT/LOG00001.TXT" ]; then
                    echo "fs-check [$label]: second file visible to the kernel too"
                else
                    echo "fs-check [$label]: second file NOT visible to the kernel"
                    rc=1
                fi
            else
                echo "fs-check [$label]: could not loop-mount (no permission in this environment); kernel check skipped"
            fi
            [ -n "${MNT:-}" ] && udisksctl unmount -b "$LOOP" >/dev/null 2>&1
            udisksctl loop-delete -b "$LOOP" >/dev/null 2>&1
        else
            echo "fs-check [$label]: udisksctl could not attach a loop device; kernel check skipped"
        fi
    else
        mkfs.vfat -F 16 -s "$spc" "$IMG" >/dev/null 2>&1 || { echo "fs-check [$label]: mkfs -F 16 failed, skipping"; return 0; }
        if ./$BIN "$IMG" 2>/dev/null >/dev/null; then
            echo "fs-check [$label]: fat32.c ACCEPTED a FAT16 volume - it must refuse"
            rc=1
        else
            echo "fs-check [$label]: fat32.c refuses the FAT16 volume, with a reason"
        fi
    fi
}

run_case "FAT32 1-sector clusters" 64  1 ok
run_case "FAT32 8-sector clusters"  512 8 ok
run_case "FAT16 refusal"            32  4 refuse

rm -f "$IMG" "$REF" fsck.out
if [ $rc -eq 0 ]; then
    echo "fs-check: passed"
else
    echo "fs-check: FAILED"
fi
exit $rc
