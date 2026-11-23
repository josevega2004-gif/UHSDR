/*
 * fat32.h - minimal FAT32 volume writer for the UHSDR USB host stack.
 *
 * Scope, and why it is this narrow: the radio has never been able to put a file anywhere.  Nothing in
 * the tree writes a sector - the only FAT code that touches a thumb drive is in the bootloader, which
 * reads fw-mchf.bin and does not stay resident.  This is the piece that has to exist before the radio
 * can record anything at all (a weather-fax line, an audio capture, a log), so it is written and
 * tested on its own, against a volume that mkfs.vfat made and fsck.vfat judges.
 *
 * What it does: mount a FAT32 volume, find a free slot in the root directory, allocate clusters as
 * bytes are handed to it, and close the file.  It streams - it never holds more than one sector,
 * because the F407 has 192 KB of RAM total and a 600x800 fax picture is 480 KB, so any format worth
 * saving has to be written a line at a time.
 *
 * What it deliberately does not do, each with the reason the caller can see:
 *   - FAT16 or FAT12.  It is detected and refused with a reason, not mis-handled.  Sticks this size
 *     are FAT32, and the flashing instructions say FAT/FAT32, so FAT16 is a real possibility on
 *     someone's old drive and silently treating it as FAT32 would corrupt it.
 *   - Long file names.  8.3 only, so names look like APT00123.PNG.  LFN is another 150 lines of
 *     checksummed alias entries, and a name that fits the spec costs nothing to keep short.
 *   - Subdirectories, delete, rename, read-modify-rewrite of an existing file, the "." and ".."
 *     entries of anything, and volumes whose FSINFO lies (a stale free count is recovered by
 *     scanning, but the scan is where the mount gets slow).
 *   - Partitions.  It expects the LBA of the volume's first sector, so the caller supplies the
 *     offset after reading the MBR.  That is one more sector parser than a first cut should hide.
 *
 * Correctness is graded by fsck.vfat on the image afterwards, not by this module agreeing with
 * itself: a writer that reads back its own output proves only that it is self-consistent.
 */

#ifndef UHD_FAT32_H
#define UHD_FAT32_H

#include <stdint.h>
#include <stddef.h>

#define FAT_SECTOR_SIZE     512
#define FAT_MAX_ERR         56

/* The device is two callbacks over 512-byte sectors.  On the radio they land on the USB mass-storage
 * class; in the bench they land on a FILE.  Nothing else in here may know what a sector is stored
 * on, which is what makes the same code testable without hardware. */
typedef struct
{
    int (*read)(void *device, uint32_t lba, uint8_t *buffer);
    int (*write)(void *device, uint32_t lba, const uint8_t *buffer);
    void *device;
} FatDevice;

typedef struct
{
    const FatDevice *dev;

    uint32_t fat_begin;          // first sector of the first FAT copy
    uint32_t fat_sectors;        // sectors per FAT copy
    uint16_t fat_copies;
    uint32_t root_cluster;
    uint32_t fsinfo_sector;
    uint32_t backup_sector;
    uint32_t data_begin;         // first sector of cluster 2
    uint32_t cluster_count;      // usable clusters, 0 and 1 excluded
    uint32_t sectors_per_cluster;
    uint32_t hidden_sectors;     // kept for the error message: this is where a partition begins

    uint32_t scan_cursor;        // next cluster number to try when allocating

    // Free space, as the volume told us at mount and as our own allocations have changed it since.
    // Decrementing is what makes closing a file cheap; a recount means walking every FAT entry, which
    // on a 32 GB stick is millions of sector reads and has no business being in an interrupt-adjacent
    // path.  Fat32_FreeClusterCount() is the recount, for a caller that wants to calibrate.
    uint32_t free_clusters;
    int      free_clusters_known;

    // the file being written, as far as it has got
    uint32_t first_cluster;
    uint32_t cur_cluster;
    uint32_t cur_cluster_index;  // which cluster of this file cur_cluster is, 0 for the first
    uint8_t  dir_name[11];       // the 8.3 form, built by Fat32_Create, written by Fat32_Close
    uint8_t  dir_attr;

    // the file currently open, if any
    uint32_t write_size;         // bytes written so far
    uint32_t write_cluster;      // last cluster of the chain
    uint32_t write_dir_lba;      // sector holding the directory entry
    uint16_t write_dir_index;    // which 32-byte slot in that sector
    uint8_t  sector[FAT_SECTOR_SIZE];   // the one buffer this module will ever hold
    uint32_t sector_lba;         // which sector is in sector[]
    uint8_t  sector_dirty;

    uint16_t dos_time;           // filled by the caller from the RTC, or a fixed value in the bench
    uint16_t dos_date;
    uint16_t creat_time;
    uint16_t creat_date;

    int      mounted;
    int      open;
    char     error[FAT_MAX_ERR];
} Fat32;

/* Parse and validate the boot sector.  Returns 0 and a reason in fs->error if the volume is not
 * something this module is willing to write to.  A successful mount does not write anything. */
int Fat32_Mount(Fat32 *fs, const FatDevice *device);

/* Open a new file in the root directory.  name is "APT00123" "PNG" - it is converted to the 8.3
 * on-disk form and upper-cased; a name that cannot be expressed in 8.3 is refused, not truncated,
 * because a silently truncated name is a file the operator cannot find.  attr: 0x20 for archive. */
int Fat32_Create(Fat32 *fs, const char *name, const char *extension, uint8_t attr);

/* Append bytes.  Streams: each call may be any length, and only a sector is ever held. */
int Fat32_Write(Fat32 *fs, const uint8_t *data, uint32_t length);

/* Write the size and the end-of-chain, flush the directory entry and the FSINFO free count.
 * A file that was never closed is a chain the FAT describes but the directory does not size, which
 * fsck.vfat will report - so Close is part of the record, not a nicety. */
int Fat32_Close(Fat32 *fs);

/* Read bytes from a file given its first cluster, for tests and for a caller that wants to verify
 * rather than trust.  Returns the number of bytes read, or -1. */
int Fat32_ReadClusterChain(const Fat32 *fs, uint32_t first_cluster, uint32_t offset,
                           uint8_t *buffer, uint32_t length);

/* Cluster number of the next free slot, or 0 if the volume is full.  Exposed because "how full is
 * the stick" is the first question a recording feature has to answer on screen. */
uint32_t Fat32_FreeClusterCount(Fat32 *fs);

const char *Fat32_Error(const Fat32 *fs);

#endif // UHD_FAT32_H
