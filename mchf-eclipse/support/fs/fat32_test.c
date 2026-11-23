/*
 * fat32_test.c - run drivers/fs/fat32.c against a real volume, on a host, without a radio.
 *
 * The point of this file is that fat32.c must be judged by something other than itself.  What is
 * checked here, in order of how much it is worth:
 *
 *   1. mkfs.vfat made the volume and fsck.vfat inspects it afterwards - an independent implementation
 *      of the same format, from the tools that format sticks.  It is what catches a wrong free-cluster
 *      count, a bad chain, or a directory entry that lies about its size.
 *   2. the kernel's own FAT driver, if this machine lets the harness loop-mount the image
 *      (`run-fs-check.sh` tries and reports whether it could).  That is the same code path a PC uses
 *      when the stick comes back out of the radio, and it is the only check of the *bytes* that does
 *      not go through fat32.c.
 *   3. this program's own read-back, which proves only that writer and reader agree, and is reported
 *      as such rather than as a pass.
 *
 * Sizes are chosen to be awkward rather than tidy: a length that is not a multiple of 512, and longer
 * than a cluster, so the tail of the last sector and the end of the chain both get exercised.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../drivers/fs/fat32.h"

typedef struct
{
    FILE *file;
    long sectors;
} FileDevice;

static int file_read(void *device, uint32_t lba, uint8_t *buffer)
{
    FileDevice *d = (FileDevice *)device;
    if (fseek(d->file, (long)lba * FAT_SECTOR_SIZE, SEEK_SET) != 0)
    {
        return -1;
    }
    return fread(buffer, 1, FAT_SECTOR_SIZE, d->file) == FAT_SECTOR_SIZE ? 0 : -1;
}

#ifdef LOG_WRITES
static int log_count;
#endif

static int file_write(void *device, uint32_t lba, const uint8_t *buffer)
{
    FileDevice *d = (FileDevice *)device;
#ifdef LOG_WRITES
    if (log_count++ < 400)
    {
        fprintf(stderr, "W %u\n", lba);
    }
#endif
    if (fseek(d->file, (long)lba * FAT_SECTOR_SIZE, SEEK_SET) != 0)
    {
        return -1;
    }
    return fwrite(buffer, 1, FAT_SECTOR_SIZE, d->file) == FAT_SECTOR_SIZE ? 0 : -1;
}

/* The pattern is a function of the absolute byte offset, so a cluster written in the wrong place or a
 * sector duplicated shows up as a mismatch rather than as plausible-looking noise. */
static void pattern(uint8_t *out, uint32_t offset, uint32_t length)
{
    uint32_t i;
    for (i = 0; i < length; i++)
    {
        uint32_t o = offset + i;
        out[i] = (uint8_t)((o * 1103515245u + 12345u) >> (o % 7u + 1u));
    }
}

static int failures;

static void check(int condition, const char *what)
{
    printf("%s  %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition)
    {
        failures++;
    }
}

/* DOS time for a fixed moment, so the entries are reproducible: 2026-11-23 12:00:00. */
static uint16_t dos_date_of(int y, int m, int d) { return (uint16_t)(((y - 1980) << 9) | (m << 5) | d); }
static uint16_t dos_time_of(int h, int mi, int s) { return (uint16_t)((h << 11) | (mi << 5) | (s / 2)); }

int main(int argc, char **argv)
{
    FileDevice device;
    FatDevice dev;
    Fat32 fs;
    uint8_t *buffer;
    uint32_t big = 1u * 1024u * 1024u + 3u * 512u + 77u;   // 1 MB + 1549 bytes: straddles clusters and sectors
    uint32_t small = 41u;
    const char *image = argc > 1 ? argv[1] : "volume.img";
    const char *expected_file = argc > 2 ? argv[2] : NULL;    // the reference bytes, for an outside reader
    uint32_t first_cluster = 0;

    device.file = fopen(image, "r+b");
    if (device.file == NULL)
    {
        printf("cannot open %s: make the volume first (mkfs.vfat)\n", image);
        return 2;
    }
    dev.read = file_read;
    dev.write = file_write;
    dev.device = &device;

    buffer = malloc(big);
    if (buffer == NULL)
    {
        return 2;
    }

    printf("volume: %s\n\n", image);

    check(Fat32_Mount(&fs, &dev) == 0, "mount parses the boot sector");
    if (!fs.mounted)
    {
        printf("    %s\n", Fat32_Error(&fs));
        return 1;
    }
    printf("    clusters %u, %u sectors/cluster, root cluster %u, FAT copies %u\n",
           fs.cluster_count, fs.sectors_per_cluster, fs.root_cluster, fs.fat_copies);
    check(fs.sectors_per_cluster >= 1, "geometry is not nonsense");

    fs.dos_time = dos_time_of(12, 0, 0);
    fs.dos_date = dos_date_of(2026, 11, 23);

    // A name that cannot be expressed in 8.3 has to be refused, not truncated.
    check(Fat32_Create(&fs, "TOO.LONG.NAME", "PNG", 0x20) != 0, "an unrepresentable name is refused");
    check(Fat32_Create(&fs, "APT1", "PNG", 0x20) == 0, "a normal 8.3 name is accepted");
    if (fs.open)
    {
        Fat32_Close(&fs);                     // empty file, closed: a zero-length chain is legal
    }

    // The real write.
    check(Fat32_Create(&fs, "APT01234", "PNG", 0x20) == 0, "create with an 8-character base");
    pattern(buffer, 0, big);
    /* Written in three pieces, none of them a multiple of the sector size: a recorder fed by a
     * fax line hands over one scanline at a time, and the widths of those lines are nobody's idea
     * of a round number. */
    check(Fat32_Write(&fs, buffer, 100000u) == 0, "write 100000 bytes");
    check(Fat32_Write(&fs, buffer + 100000u, 700001u) == 0, "write 700001 bytes");
    check(Fat32_Write(&fs, buffer + 800001u, big - 800001u) == 0, "write the odd tail");
    check(fs.write_size == big, "size tracked as written");
    first_cluster = fs.first_cluster;
    check(Fat32_Close(&fs) == 0, "close writes the directory entry");

    if (expected_file != NULL)
    {
        FILE *ef = fopen(expected_file, "wb");
        if (ef == NULL || fwrite(buffer, 1, big, ef) != big)
        {
            printf("cannot write the reference file\n");
            return 2;
        }
        fclose(ef);                            // buffer still holds the big pattern at this point
    }

    // The name just written cannot be created again; this is the check that the directory walk looks
    // past the free slot it chose instead of stopping at it.
    check(Fat32_Create(&fs, "APT01234", "PNG", 0x20) != 0, "a duplicate name is refused");
    if (!fs.open)
    {
        fs.error[0] = '\0';                    // the refusal above is expected, not a fault to carry
    }

    // A second file, so the directory is not a single entry and the allocator is not used once.
    check(Fat32_Create(&fs, "LOG00001", "TXT", 0x20) == 0, "create a second file");
    pattern(buffer, 0, small);
    check(Fat32_Write(&fs, buffer, small) == 0, "write 41 bytes");
    check(Fat32_Close(&fs) == 0, "close the short file");

    // Re-open the volume from scratch: nothing may depend on state left in memory.
    check(Fat32_Mount(&fs, &dev) == 0, "remount after writing");
    {
        uint8_t *read_back = malloc(big);
        uint8_t *expected  = malloc(big);
        int got;
        if (read_back == NULL || expected == NULL)
        {
            return 2;
        }
        got = Fat32_ReadClusterChain(&fs, first_cluster, 0, read_back, big);
        pattern(expected, 0, big);
        check(got == (int)big, "chain walk returns the whole length");
        check(got == (int)big && memcmp(read_back, expected, big) == 0,
              "bytes read back through the chain equal the bytes written");
        // A read starting in the middle of a cluster exercises the offset arithmetic separately.
        {
            uint32_t off = fs.sectors_per_cluster * FAT_SECTOR_SIZE + 300u;
            if (off + 1000u <= big)
            {
                uint8_t *part = malloc(1000u);
                int pgot = Fat32_ReadClusterChain(&fs, first_cluster, off, part, 1000u);
                check(pgot == 1000 && memcmp(part, expected + off, 1000u) == 0,
                      "a read starting mid-cluster lands on the right bytes");
                free(part);
            }
        }
        free(read_back);
        free(expected);
    }

    {
        /* The running count we maintained, against a recount that walks every FAT entry.  If these
         * disagree, fsck's "Free cluster summary wrong" is coming for us. */
        uint32_t recounted = Fat32_FreeClusterCount(&fs);
        printf("    free clusters: tracked %u, recounted %u%s\n",
               fs.free_clusters, recounted,
               fs.free_clusters_known && recounted == fs.free_clusters ? "  (agree)" : "  (DISAGREE)");
        check(!fs.free_clusters_known || recounted == fs.free_clusters, "the running free count matches a full recount");
    }

    printf("\n%s: %d check(s) failed\n", failures == 0 ? "self-consistency" : "SELF-CONSISTENCY ONLY", failures);
    printf("NOTE: the checks above are fat32.c agreeing with itself.  The verdict that matters is\n"
           "fsck.vfat and, if this machine allowed it, the kernel's own FAT driver - see run-fs-check.sh.\n");
    fclose(device.file);
    return failures == 0 ? 0 : 1;
}
