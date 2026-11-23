/*
 * fat32.c - see fat32.h for what this is for and, more usefully, what it is not.
 *
 * Every number here comes from the Microsoft FAT specification (3.2 FAT32 BPB, 6.2.3 FSINFO, 6.4.1
 * directory entry), and the acceptance test is that dosfstools' fsck.vfat looks at the volume
 * afterwards and calls it clean.  An agreement between this writer and a reader by the same author
 * would be worth very little on its own, so the bench also compares the bytes that come back through
 * a second, unrelated path.
 */

#include "fat32.h"

#include <string.h>

#define FAT32_EOC_MIN     0x0FFFFFF8u
#define FAT32_FREE        0x00000000u
#define FAT32_BAD         0x0FFFFFF7u
#define DIR_SLOT_SIZE     32
#define DIR_ATTR_ARCHIVE  0x20
#define DIR_ATTR_VOLUME   0x08

static void set_error(Fat32 *fs, const char *reason)
{
    if (fs->error[0] == '\0')
    {
        strncpy(fs->error, reason, FAT_MAX_ERR - 1);
        fs->error[FAT_MAX_ERR - 1] = '\0';
    }
}

static uint16_t ld16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t ld32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void st16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void st32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* One sector of cache.  Loading flushes first, so the cache can never be silently dropped; there is
 * deliberately only one, because the radio's RAM is already spoken for and a file this module writes
 * is a sequence of sectors, not a working set. */
static int flush_sector(Fat32 *fs)
{
    if (fs->sector_dirty && fs->sector_lba != 0xFFFFFFFFu)
    {
        if (fs->dev->write(fs->dev->device, fs->sector_lba, fs->sector) != 0)
        {
            set_error(fs, "device refused a sector write");
            return -1;
        }
        fs->sector_dirty = 0;
    }
    return 0;
}

static int load_sector(Fat32 *fs, uint32_t lba)
{
    if (fs->sector_lba == lba)
    {
        return 0;
    }
    if (flush_sector(fs) != 0)
    {
        return -1;
    }
    if (fs->dev->read(fs->dev->device, lba, fs->sector) != 0)
    {
        set_error(fs, "device refused a sector read");
        return -1;
    }
    fs->sector_lba = lba;
    fs->sector_dirty = 0;
    return 0;
}

/* FAT entries are read and written straight through to the medium rather than left in the cache: the
 * cache is where file data sits, and letting a dirty FAT sector be evicted by a data write would
 * reorder the bookkeeping against the data it describes.  One sector read per cluster is not a cost
 * worth a second buffer. */
static uint32_t fat_get(Fat32 *fs, uint32_t cluster)
{
    uint32_t byte = cluster * 4u;
    if (load_sector(fs, fs->fat_begin + byte / FAT_SECTOR_SIZE) != 0)
    {
        return 0xFFFFFFFFu;
    }
    return ld32(&fs->sector[byte % FAT_SECTOR_SIZE]) & 0x0FFFFFFFu;
}

static int fat_put(Fat32 *fs, uint32_t cluster, uint32_t value)
{
    uint32_t copy;
    uint32_t byte = cluster * 4u;
    uint32_t lba = fs->fat_begin + byte / FAT_SECTOR_SIZE;

    if (flush_sector(fs) != 0)
    {
        return -1;
    }
    for (copy = 0; copy < fs->fat_copies; copy++)
    {
        if (fs->dev->read(fs->dev->device, lba + copy * fs->fat_sectors, fs->sector) != 0)
        {
            set_error(fs, "device refused a FAT read");
            return -1;
        }
        fs->sector_lba = 0xFFFFFFFFu;          // not in cache: do not let flush write it back
        {
            uint32_t keep = ld32(&fs->sector[byte % FAT_SECTOR_SIZE]);
            st32(&fs->sector[byte % FAT_SECTOR_SIZE], (keep & 0xF0000000u) | (value & 0x0FFFFFFFu));
        }
        if (fs->dev->write(fs->dev->device, lba + copy * fs->fat_sectors, fs->sector) != 0)
        {
            set_error(fs, "device refused a FAT write");
            return -1;
        }
    }
    return 0;
}

static int is_pow2(uint32_t v) { return v != 0 && (v & (v - 1u)) == 0; }

int Fat32_Mount(Fat32 *fs, const FatDevice *device)
{
    uint8_t bs[FAT_SECTOR_SIZE];
    uint32_t total_sectors, reserved, fat_sectors, root_sectors, data_sectors;

    memset(fs, 0, sizeof(*fs));
    fs->dev = device;
    fs->sector_lba = 0xFFFFFFFFu;

    if (device->read(device->device, 0, bs) != 0)
    {
        set_error(fs, "device refused the boot sector");
        return -1;
    }

    if (bs[510] != 0x55 || bs[511] != 0xAA)
    {
        set_error(fs, "no boot sector signature: is this the volume start, not the partition's MBR?");
        return -1;
    }
    if (memcmp(&bs[0x36], "FAT16   ", 8) == 0)
    {
        set_error(fs, "FAT16 volume: this module writes FAT32 only");
        return -1;
    }
    if (memcmp(&bs[0x52], "FAT32   ", 8) != 0)
    {
        set_error(fs, "boot sector does not describe itself as FAT32");
        return -1;
    }

    fs->sectors_per_cluster = bs[0x0D];
    reserved                = ld16(&bs[0x0E]);
    fs->fat_copies          = bs[0x10];
    fs->fsinfo_sector       = ld16(&bs[0x30]);
    fs->backup_sector       = ld16(&bs[0x32]);
    fs->root_cluster        = ld32(&bs[0x2C]);
    fs->hidden_sectors      = ld32(&bs[0x1C]);

    total_sectors = ld16(&bs[0x13]) != 0 ? ld16(&bs[0x13]) : ld32(&bs[0x20]);
    fat_sectors   = ld32(&bs[0x24]) != 0 ? ld32(&bs[0x24]) : ld32(&bs[0x28]);

    if (ld16(&bs[0x0B]) != FAT_SECTOR_SIZE)
    {
        set_error(fs, "sector size is not 512, which this module assumes everywhere");
        return -1;
    }
    if (!is_pow2(fs->sectors_per_cluster) || fs->sectors_per_cluster > 128)
    {
        set_error(fs, "implausible sectors-per-cluster");
        return -1;
    }
    if (fs->fat_copies == 0 || fs->fat_copies > 16 || fat_sectors == 0 || reserved == 0)
    {
        set_error(fs, "implausible FAT geometry");
        return -1;
    }
    if (fs->root_cluster < 2 || fs->fsinfo_sector >= reserved || fs->backup_sector >= reserved)
    {
        set_error(fs, "FAT32 fields point outside the reserved area");
        return -1;
    }

    fs->fat_begin    = reserved;
    fs->fat_sectors  = fat_sectors;      // not a redundancy: with this left at zero the two mirror
                                         // copies collapse onto the same sector, and only fsck notices
    root_sectors   = 0;                        // FAT32 keeps the root directory in the cluster chain
    data_sectors   = total_sectors > (uint32_t)(reserved + fs->fat_copies * fat_sectors + root_sectors)
                     ? total_sectors - (reserved + fs->fat_copies * fat_sectors + root_sectors) : 0;
    fs->data_begin = reserved + fs->fat_copies * fat_sectors + root_sectors;
    fs->cluster_count = data_sectors / fs->sectors_per_cluster;

    /* The FAT type is defined by cluster count, not by what the boot sector calls itself.  A volume
     * labelled FAT32 with fewer than 65472 clusters is a malformed or hand-made image; treating it
     * as FAT32 is exactly how a FAT16 stick gets destroyed. */
    if (fs->cluster_count < 65472)
    {
        set_error(fs, "cluster count says FAT16, though the boot sector says FAT32: refusing");
        return -1;
    }

    fs->scan_cursor = 2;

    // What the volume says about itself.  An unreadable or lying FSINFO is not a fault: it means the
    // count must be recomputed before anyone quotes a number, so the mount records that it is unknown
    // rather than guessing.
    fs->free_clusters_known = 0;
    if (fs->fsinfo_sector != 0)
    {
        uint8_t info[FAT_SECTOR_SIZE];
        if (device->read(device->device, fs->fsinfo_sector, info) == 0
            && ld32(&info[0x1E4]) == 0x61417272u
            && info[510] == 0x55 && info[511] == 0xAA)
        {
            uint32_t claim = ld32(&info[0x1E8]);
            if (claim != 0xFFFFFFFFu && claim <= fs->cluster_count)
            {
                fs->free_clusters = claim;
                fs->free_clusters_known = 1;
            }
            if (ld32(&info[0x1EC]) >= 2 && ld32(&info[0x1EC]) < fs->cluster_count + 2)
            {
                fs->scan_cursor = ld32(&info[0x1EC]);
            }
        }
    }

    fs->mounted = 1;
    return 0;
}

uint32_t Fat32_FreeClusterCount(Fat32 *fs)
{
    uint32_t cluster;
    uint32_t free_count = 0;

    if (!fs->mounted)
    {
        return 0;
    }
    // The FSINFO free count is allowed to be stale, and often is after a camera or a car radio.
    // Scanning is the only answer, so it is done once per mount and cached in the cursor's restart.
    for (cluster = 2; cluster < fs->cluster_count + 2; cluster++)
    {
        uint32_t v = fat_get(fs, cluster);
        if (v == FAT32_BAD)
        {
            return 0xFFFFFFFFu;
        }
        if (v == FAT32_FREE)
        {
            free_count++;
        }
    }
    return free_count;
}

static uint32_t alloc_cluster(Fat32 *fs)
{
    uint32_t tried, cluster, value;

    for (tried = 0; tried < fs->cluster_count; tried++)
    {
        cluster = fs->scan_cursor;
        if (cluster >= fs->cluster_count + 2)
        {
            cluster = 2;
        }
        fs->scan_cursor = cluster + 1;
        value = fat_get(fs, cluster);
        if (value == FAT32_FREE)
        {
            if (fs->free_clusters_known && fs->free_clusters > 0)
            {
                fs->free_clusters--;
            }
            return cluster;
        }
    }
    set_error(fs, "volume is full");
    return 0;
}

static int name_component_ok(char c)
{
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    return strchr("$%'-_@~`!(){}^#&", c) != NULL;
}

/* Build the 11-byte 8.3 name.  Refusals are deliberate and specific: a name that gets quietly
 * shortened is a file the operator cannot find on the stick afterwards. */
static int make_83(const char *name, const char *ext, uint8_t *out, const char **why)
{
    size_t i, n = strlen(name), e = ext ? strlen(ext) : 0;
    char c;

    memset(out, ' ', 11);
    if (n == 0 || n > 8)
    {
        *why = "base name longer than 8 characters";
        return -1;
    }
    if (e > 3)
    {
        *why = "extension longer than 3 characters";
        return -1;
    }
    for (i = 0; i < n; i++)
    {
        c = name[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c == ' ')
        {
            *why = "space in a base name: 8.3 cannot hold it";
            return -1;
        }
        if (!name_component_ok(c))
        {
            *why = "character not allowed in an 8.3 name";
            return -1;
        }
        out[i] = (uint8_t)c;
    }
    for (i = 0; i < e; i++)
    {
        c = ext[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (!name_component_ok(c))
        {
            *why = "character not allowed in an extension";
            return -1;
        }
        out[8 + i] = (uint8_t)c;
    }
    return 0;
}

int Fat32_Create(Fat32 *fs, const char *name, const char *extension, uint8_t attr)
{
    uint8_t n83[11];
    const char *why = "unusable name";
    uint32_t cluster, idx, total, max_entries;
    int slot_found = 0;

    if (!fs->mounted)
    {
        set_error(fs, "not mounted");
        return -1;
    }
    if (fs->open)
    {
        set_error(fs, "a file is already open");
        return -1;
    }
    if (make_83(name, extension, n83, &why) != 0)
    {
        set_error(fs, why);
        return -1;
    }

    /* Walk the whole root directory before committing, for two answers at once: the first free slot
     * (0x00 unused-or-end, 0xE5 deleted) and whether the name is already there.  Stopping at the
     * first free slot would let a duplicate be created further along, and two entries with one name
     * is the kind of damage that only shows up when the stick goes back in a PC. */
    cluster = fs->root_cluster;
    idx = 0;
    max_entries = fs->cluster_count * 16;
    for (total = 0; total < max_entries && cluster >= 2; total++)
    {
        uint32_t lba, byte;
        uint8_t first;

        if (idx == fs->sectors_per_cluster * 16)
        {
            cluster = fat_get(fs, cluster);
            idx = 0;
            if (cluster < 2)
            {
                break;
            }
        }
        lba = fs->data_begin + (cluster - 2) * fs->sectors_per_cluster + (idx * DIR_SLOT_SIZE) / FAT_SECTOR_SIZE;
        byte = (idx * DIR_SLOT_SIZE) % FAT_SECTOR_SIZE;
        if (load_sector(fs, lba) != 0)
        {
            return -1;
        }
        first = fs->sector[byte];
        if (first != 0x00 && first != 0xE5
            && (fs->sector[byte + 11] & DIR_ATTR_VOLUME) != DIR_ATTR_VOLUME      // not the volume label
            && memcmp(&fs->sector[byte], n83, 11) == 0)
        {
            set_error(fs, "that file already exists on the volume");
            return -1;
        }
        if (!slot_found && (first == 0x00 || first == 0xE5))
        {
            fs->write_dir_lba = lba;
            fs->write_dir_index = (uint16_t)(byte / DIR_SLOT_SIZE);
            slot_found = 1;
        }
        if (first == 0x00 && slot_found)
        {
            break;          // end of directory reached, nothing further can duplicate the name
        }
        idx++;
    }

    if (!slot_found)
    {
        set_error(fs, "root directory has no free slot (extending it is not implemented)");
        return -1;
    }

    memcpy(fs->dir_name, n83, 11);
    fs->dir_attr = attr;
    fs->write_size = 0;
    fs->first_cluster = 0;
    fs->cur_cluster = 0;
    fs->cur_cluster_index = 0;
    fs->open = 1;
    fs->error[0] = '\0';
    fs->creat_time = fs->dos_time;
    fs->creat_date = fs->dos_date;
    fs->sector_lba = 0xFFFFFFFFu;              // the directory sector we were reading is not ours to keep
    return 0;
}

int Fat32_Write(Fat32 *fs, const uint8_t *data, uint32_t length)
{
    uint32_t bytes_per_cluster = fs->sectors_per_cluster * FAT_SECTOR_SIZE;

    if (!fs->open)
    {
        set_error(fs, "no file open");
        return -1;
    }

    while (length > 0)
    {
        uint32_t cluster_index = fs->write_size / bytes_per_cluster;
        uint32_t in_cluster    = fs->write_size % bytes_per_cluster;
        uint32_t in_sector     = fs->write_size % FAT_SECTOR_SIZE;
        uint32_t chunk = FAT_SECTOR_SIZE - in_sector;
        uint32_t lba;

        if (chunk > length)
        {
            chunk = length;
        }

        if (fs->first_cluster == 0)
        {
            fs->cur_cluster = alloc_cluster(fs);
            if (fs->cur_cluster == 0)
            {
                return -1;
            }
            fs->first_cluster = fs->cur_cluster;
            fs->cur_cluster_index = 0;
            if (fat_put(fs, fs->cur_cluster, 0x0FFFFFFFu) != 0)   // end of chain until the next one appears
            {
                return -1;
            }
        }
        else if (cluster_index != fs->cur_cluster_index)
        {
            uint32_t next;
            if (cluster_index != fs->cur_cluster_index + 1)
            {
                set_error(fs, "internal: writes must be sequential");
                return -1;
            }
            next = alloc_cluster(fs);
            if (next == 0)
            {
                return -1;
            }
            if (fat_put(fs, fs->cur_cluster, next) != 0 || fat_put(fs, next, 0x0FFFFFFFu) != 0)
            {
                return -1;
            }
            fs->cur_cluster = next;
            fs->cur_cluster_index = cluster_index;
        }

        lba = fs->data_begin + (fs->cur_cluster - 2) * fs->sectors_per_cluster
              + in_cluster / FAT_SECTOR_SIZE;

        if (lba != fs->sector_lba)
        {
            if (flush_sector(fs) != 0)
            {
                return -1;
            }
            if (in_sector != 0)
            {
                // A partial sector must be read before it is extended, or its earlier bytes are lost.
                if (load_sector(fs, lba) != 0)
                {
                    return -1;
                }
            }
            else
            {
                memset(fs->sector, 0, FAT_SECTOR_SIZE);
                fs->sector_lba = lba;
                fs->sector_dirty = 0;         // a whole sector being replaced needs no read
            }
        }
        memcpy(&fs->sector[in_sector], data, chunk);
        fs->sector_dirty = 1;

        fs->write_size += chunk;
        data   += chunk;
        length -= chunk;
    }
    return 0;
}

int Fat32_Close(Fat32 *fs)
{
    uint8_t entry[DIR_SLOT_SIZE];

    if (!fs->open)
    {
        set_error(fs, "no file open");
        return -1;
    }

    if (flush_sector(fs) != 0)
    {
        return -1;
    }

    // The directory entry: built fresh, then written into the slot Fat32_Create found.
    memset(entry, 0, sizeof(entry));
    memcpy(entry, fs->dir_name, 11);
    entry[11] = fs->dir_attr;
    entry[12] = 0x00;                                   // NT flags: the name is upper case as stored
    st16(&entry[14], fs->creat_time);
    st16(&entry[16], fs->creat_date);
    st16(&entry[18], fs->dos_date);                     // last access
    st16(&entry[20], (uint16_t)(fs->first_cluster >> 16));
    st16(&entry[22], fs->dos_time);
    st16(&entry[24], fs->dos_date);
    st16(&entry[26], (uint16_t)(fs->first_cluster & 0xFFFF));
    st32(&entry[28], fs->write_size);

    if (load_sector(fs, fs->write_dir_lba) != 0)
    {
        return -1;
    }
    memcpy(&fs->sector[fs->write_dir_index * DIR_SLOT_SIZE], entry, DIR_SLOT_SIZE);
    fs->sector_dirty = 1;
    if (flush_sector(fs) != 0)
    {
        return -1;
    }

    // FSINFO: the running free count and the next-free hint.  This is the last thing written, and it
    // is done through a local sector rather than the cache, because the cache is where file data and
    // FAT sectors live and a scan that evicts the FSINFO from under an edit is exactly the bug this
    // replaced: the patch used to land on whichever FAT sector the count walk had last touched.
    if (fs->fsinfo_sector != 0)
    {
        uint8_t info[FAT_SECTOR_SIZE];
        if (fs->dev->read(fs->dev->device, fs->fsinfo_sector, info) == 0
            && ld32(&info[0x1E4]) == 0x61417272u
            && info[510] == 0x55 && info[511] == 0xAA)
        {
            if (fs->free_clusters_known)
            {
                st32(&info[0x1E8], fs->free_clusters);
            }
            st32(&info[0x1EC], fs->scan_cursor);
            if (fs->dev->write(fs->dev->device, fs->fsinfo_sector, info) != 0)
            {
                set_error(fs, "device refused the FSINFO write, so the free count is stale");
            }
        }
    }

    fs->open = 0;
    fs->write_size = 0;
    fs->write_cluster = 0;
    fs->first_cluster = 0;
    fs->cur_cluster = 0;
    fs->cur_cluster_index = 0;
    return 0;
}

int Fat32_ReadClusterChain(const Fat32 *fs, uint32_t first_cluster, uint32_t offset,
                           uint8_t *buffer, uint32_t length)
{
    uint32_t bytes_per_cluster = fs->sectors_per_cluster * FAT_SECTOR_SIZE;
    uint32_t done = 0;
    uint32_t cluster = first_cluster;
    uint32_t skip = offset;
    uint8_t *scratch;

    if (cluster < 2)
    {
        return -1;
    }
    while (skip >= bytes_per_cluster)
    {
        uint32_t next;
        skip -= bytes_per_cluster;
        next = fat_get((Fat32 *)fs, cluster);
        if (next < 2)
        {
            return -1;
        }
        cluster = next;
    }

    scratch = buffer;
    while (done < length)
    {
        uint32_t lba = fs->data_begin + (cluster - 2) * fs->sectors_per_cluster + skip / FAT_SECTOR_SIZE;
        uint32_t chunk = FAT_SECTOR_SIZE - (skip % FAT_SECTOR_SIZE);
        uint32_t remaining = length - done;

        if (chunk > remaining)
        {
            chunk = remaining;
        }
        {
            uint8_t sector[FAT_SECTOR_SIZE];
            if (fs->dev->read(fs->dev->device, lba, sector) != 0)
            {
                return -1;
            }
            memcpy(&scratch[done], &sector[skip % FAT_SECTOR_SIZE], chunk);
        }
        done += chunk;
        skip += chunk;
        if (skip >= bytes_per_cluster)
        {
            uint32_t next;
            skip = 0;
            next = fat_get((Fat32 *)fs, cluster);
            if (next < 2)
            {
                break;
            }
            cluster = next;
        }
    }
    return (int)done;
}

const char *Fat32_Error(const Fat32 *fs)
{
    return fs->error[0] != '\0' ? fs->error : "no error";
}
