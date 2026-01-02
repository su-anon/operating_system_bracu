#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

//VSFS
#define BLOCK_SIZE        4096U
#define JOURNAL_BLOCK_IDX    1U
#define JOURNAL_BLOCKS      16U
#define INODE_BLOCKS         2U
#define DATA_BLOCKS         64U
#define INODE_BMAP_IDX     (JOURNAL_BLOCK_IDX + JOURNAL_BLOCKS)
#define DATA_BMAP_IDX      (INODE_BMAP_IDX + 1U)
#define INODE_START_IDX    (DATA_BMAP_IDX + 1U)
#define DATA_START_IDX     (INODE_START_IDX + INODE_BLOCKS)
#define TOTAL_BLOCKS       (DATA_START_IDX + DATA_BLOCKS)
#define DIRECT_POINTERS     8U
#define JOURNAL_MAGIC 0x4A524E4C
#define REC_DATA 1
#define REC_COMMIT 2

//Superblock struct
struct superblock {
    uint32_t magic;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t inode_count;

    uint32_t journal_block;
    uint32_t inode_bitmap;
    uint32_t data_bitmap;
    uint32_t inode_start;
    uint32_t data_start;

    uint8_t  _pad[128 - 9 * 4];
};

//Inode struct
struct inode {
    uint16_t type;
    uint16_t links;
    uint32_t size;

    uint32_t direct[DIRECT_POINTERS];

    uint32_t ctime;
    uint32_t mtime;

    uint8_t _pad[128 - (2 + 2 + 4 + DIRECT_POINTERS * 4 + 4 + 4)];
};

//Directory entry struct
struct dirent {
    uint32_t inode;
    char name[28];
};

//Journal header struct
struct journal_header {
    uint32_t magic;
    uint32_t nbytes_used;
    uint8_t _pad[BLOCK_SIZE - 8];
};

//Data record header
struct rec_header {
    uint32_t size;      // 0 if empty/invalid
    uint32_t type;      // REC_DATA or REC_COMMIT
    uint32_t block_no;  // Target block on main disk
    uint32_t checksum;  // Optional, can be 0 for now
};

_Static_assert(sizeof(struct superblock) == 128, "superblock must be 128 bytes");
_Static_assert(sizeof(struct inode) == 128, "inode must be 128 bytes");
_Static_assert(sizeof(struct dirent) == 32, "dirent must be 32 bytes");
_Static_assert(sizeof(struct journal_header) == BLOCK_SIZE, "journal header must be one block");
_Static_assert(sizeof(struct rec_header) == 16, "rec_header must be 16 bytes");

//Helper functions

static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static void read_blk(int fd, uint32_t block_idx, void *buf) {
    off_t offset = (off_t)block_idx * BLOCK_SIZE;
    ssize_t n = pread(fd, buf, BLOCK_SIZE, offset);
    if (n != (ssize_t)BLOCK_SIZE) {
        die("pread block");
    }
}

static void write_blk(int fd, uint32_t block_idx, const void *buf) {
    off_t offset = (off_t)block_idx * BLOCK_SIZE;
    ssize_t n = pwrite(fd, buf, BLOCK_SIZE, offset);
    if (n != (ssize_t)BLOCK_SIZE) {
        die("pwrite block");
    }
}

static void read_j_bytes(int fd, uint32_t offset, uint32_t size, void *buf) {
    off_t journal_start = (off_t)JOURNAL_BLOCK_IDX * BLOCK_SIZE;
    off_t abs_offset = journal_start + offset;
    
    // Safety check
    if (offset + size > JOURNAL_BLOCKS * BLOCK_SIZE) {
        fprintf(stderr, "Error: Journal read out of bounds.\n");
        exit(EXIT_FAILURE);
    }

    ssize_t n = pread(fd, buf, size, abs_offset);
    if (n != (ssize_t)size) {
        die("pread journal");
    }
}

static void write_j_bytes(int fd, uint32_t offset, uint32_t size, const void *buf) {
    off_t journal_start = (off_t)JOURNAL_BLOCK_IDX * BLOCK_SIZE;
    off_t abs_offset = journal_start + offset;

    if (offset + size > JOURNAL_BLOCKS * BLOCK_SIZE) {
        fprintf(stderr, "Error: Journal write out of bounds.\n");
        exit(EXIT_FAILURE);
    }

    ssize_t n = pwrite(fd, buf, size, abs_offset);
    if (n != (ssize_t)size) {
        die("pwrite journal");
    }
}

static void get_journal_header(int fd, struct journal_header *jh) {
    read_blk(fd, JOURNAL_BLOCK_IDX, jh);
    if (jh->magic != JOURNAL_MAGIC) {
        memset(jh, 0, sizeof(*jh));
        jh->magic = JOURNAL_MAGIC;
        jh->nbytes_used = 0; 
    }
}


// Reads a block from disk, then overlays any changes found in the journal.
static void read_latest_blk(int fd, uint32_t block_idx, void *buf) {
    // 1. Read baseline from disk
    read_blk(fd, block_idx, buf);

    // 2. Replay journal overrides
    struct journal_header jh;
    // We read the header directly to avoid circular dependency.
    read_blk(fd, JOURNAL_BLOCK_IDX, &jh);
    
    if (jh.magic != JOURNAL_MAGIC) {
        return;
    }

    uint32_t cursor = BLOCK_SIZE; // Data area starts after header
    uint32_t end_offset = BLOCK_SIZE + jh.nbytes_used;
    // We used to start at BLOCK_SIZE (End of header).

    while (cursor < end_offset) {
        struct rec_header rh;
        read_j_bytes(fd, cursor, sizeof(rh), &rh);

        if (rh.type == REC_DATA) {
            if (rh.block_no == block_idx) {
                read_j_bytes(fd, cursor + sizeof(rh), BLOCK_SIZE, buf);
            }
            cursor += sizeof(rh) + BLOCK_SIZE;
        } else if (rh.type == REC_COMMIT) {
            cursor += sizeof(rh);
        } else {
            break;
        }
    }
}

static int find_free_inode(int fd, struct superblock *sb, uint32_t *inode_idx) {
    uint8_t bitmap[BLOCK_SIZE];
    read_latest_blk(fd, sb->inode_bitmap, bitmap);
    
    for (uint32_t i = 0; i < sb->inode_count; i++) {
        if (!((bitmap[i / 8] >> (i % 8)) & 1)) {
            *inode_idx = i;
            return 1; // Found
        }
    }
    return 0; // Full
}

static int find_free_dirent(int fd, struct superblock *sb, const char *name, uint32_t *out_blk_idx, uint32_t *out_offset) {
    uint8_t inode_table[BLOCK_SIZE];
    read_latest_blk(fd, sb->inode_start, inode_table);
    struct inode *root = (struct inode *)inode_table;
    
    for (uint32_t i = 0; i < DIRECT_POINTERS; i++) {
        uint32_t blk = root->direct[i];
        if (blk == 0) continue; 
        
        uint8_t data_blk[BLOCK_SIZE];
        read_latest_blk(fd, blk, data_blk);
        
        struct dirent *entries = (struct dirent *)data_blk;
        uint32_t entry_count = BLOCK_SIZE / sizeof(struct dirent);
        
        for (uint32_t j = 0; j < entry_count; j++) {
             if (entries[j].inode == 0 && entries[j].name[0] == '\0') {
                 *out_blk_idx = blk;
                 *out_offset = j * sizeof(struct dirent);
                 return 1;
             }
        }
    }
    return 0; 
}

static void handle_create(int fd, const char *filename) {
    struct superblock sb;
    read_blk(fd, 0, &sb);
    
    struct journal_header jh;
    get_journal_header(fd, &jh);
    
    uint32_t free_inode_idx;
    if (!find_free_inode(fd, &sb, &free_inode_idx)) {
        fprintf(stderr, "Error: No free inodes.\n");
        return;
    }
    
    uint32_t dir_blk, dir_off;
    if (!find_free_dirent(fd, &sb, filename, &dir_blk, &dir_off)) {
         fprintf(stderr, "Error: Root directory full.\n");
         return;
    }
    
    // --- Prepare Metadata Buffers ---
    
    // 1. Inode Bitmap
    uint8_t inode_bitmap[BLOCK_SIZE];
    read_latest_blk(fd, sb.inode_bitmap, inode_bitmap);
    inode_bitmap[free_inode_idx / 8] |= (1 << (free_inode_idx % 8));
    
    // 2. Inode Table (New Inode & Root Inode Update)
    uint32_t inode_blk_offset = (free_inode_idx * sizeof(struct inode));
    uint32_t inode_blk_idx = sb.inode_start + (inode_blk_offset / BLOCK_SIZE);
    uint32_t inode_within_blk = inode_blk_offset % BLOCK_SIZE;
    
    uint8_t inode_table_blk[BLOCK_SIZE];
    read_latest_blk(fd, inode_blk_idx, inode_table_blk);
    struct inode *new_ino = (struct inode *)(inode_table_blk + inode_within_blk);
    
    memset(new_ino, 0, sizeof(*new_ino));
    new_ino->type = 1; // File
    new_ino->links = 1; 
    new_ino->size = 0;
    new_ino->ctime = time(NULL);
    new_ino->mtime = time(NULL);
    
    // Update Root Inode Size
    if (inode_blk_idx == sb.inode_start) {
        struct inode *root_ino = (struct inode *)(inode_table_blk + 0); 
        uint32_t required_size = dir_off + sizeof(struct dirent);
        if (required_size > root_ino->size) {
            root_ino->size = required_size;
        }
    } else {
        fprintf(stderr, "Warning: Root inode not in same block as new inode.\n");
    }
    
    // 3. Directory Entry
    uint8_t dir_data_blk[BLOCK_SIZE];
    read_latest_blk(fd, dir_blk, dir_data_blk);
    struct dirent *de = (struct dirent *)(dir_data_blk + dir_off);
    de->inode = free_inode_idx;
    strncpy(de->name, filename, sizeof(de->name) - 1);
    
    // --- Journal Transaction ---
    uint32_t needed = 3 * (sizeof(struct rec_header) + BLOCK_SIZE) + sizeof(struct rec_header);
    uint32_t capacity = (JOURNAL_BLOCKS - 1) * BLOCK_SIZE; 
    
    if (jh.nbytes_used + needed > capacity) {
        fprintf(stderr, "Error: Journal full.\n");
        return;
    }
    
    struct rec_header rh;
    uint32_t pos = BLOCK_SIZE + jh.nbytes_used;
    
    // Record 1: Inode Bitmap
    rh.size = BLOCK_SIZE;
    rh.type = REC_DATA;
    rh.block_no = sb.inode_bitmap;
    rh.checksum = 0;
    write_j_bytes(fd, pos, sizeof(rh), &rh);
    pos += sizeof(rh);
    write_j_bytes(fd, pos, BLOCK_SIZE, inode_bitmap);
    pos += BLOCK_SIZE;
    
    // Record 2: Inode Table Block
    rh.block_no = inode_blk_idx;
    write_j_bytes(fd, pos, sizeof(rh), &rh);
    pos += sizeof(rh);
    write_j_bytes(fd, pos, BLOCK_SIZE, inode_table_blk);
    pos += BLOCK_SIZE;
    
    // Record 3: Directory Data Block
    rh.block_no = dir_blk;
    write_j_bytes(fd, pos, sizeof(rh), &rh);
    pos += sizeof(rh);
    write_j_bytes(fd, pos, BLOCK_SIZE, dir_data_blk);
    pos += BLOCK_SIZE;
    
    // Record 4: Commit
    rh.size = 0;
    rh.type = REC_COMMIT;
    rh.block_no = 0; 
    rh.checksum = 0;
    write_j_bytes(fd, pos, sizeof(rh), &rh);
    pos += sizeof(rh);
    
    // Update Header
    jh.nbytes_used += needed;
    write_j_bytes(fd, 0, sizeof(jh), &jh);
    fsync(fd);
    
    printf("Create transaction for '%s' file was successful!.\n", filename);
}


// --- Core Mechanic 3: Install Workflow ---

struct pending_write {
    uint32_t block_no;
    uint8_t data[BLOCK_SIZE];
};

#define MAX_PENDING 64

static void handle_install(int fd) {
    struct journal_header jh;
    get_journal_header(fd, &jh);
    
    if (jh.nbytes_used == 0) {
        printf("Journal empty, nothing to install.\n");
        return;
    }
    
    uint32_t cursor = BLOCK_SIZE; 
    uint32_t end_offset = BLOCK_SIZE + jh.nbytes_used;
    
    struct pending_write pending[MAX_PENDING];
    int pending_count = 0;
    int transaction_count = 1;
    
    while (cursor < end_offset) {
        struct rec_header rh;
        read_j_bytes(fd, cursor, sizeof(rh), &rh);
        
        if (rh.type == REC_DATA) {
            printf("Reading journal record at offset %u... found DATA for block %u\n", cursor, rh.block_no);
            if (pending_count >= MAX_PENDING) {
                fprintf(stderr, "Error: Too many pending writes.\n");
                return; 
            }
            pending[pending_count].block_no = rh.block_no;
            read_j_bytes(fd, cursor + sizeof(rh), BLOCK_SIZE, pending[pending_count].data);
            pending_count++;
            
            cursor += sizeof(rh) + BLOCK_SIZE;
        } else if (rh.type == REC_COMMIT) {
            printf("Reading journal record at offset %u... found COMMIT.\n", cursor);
            printf("--> Transaction %d verified. Applying %d blocks.\n", transaction_count, pending_count);
            for (int i = 0; i < pending_count; i++) {
                write_blk(fd, pending[i].block_no, pending[i].data);
            }
            pending_count = 0;
            transaction_count++;
            cursor += sizeof(rh);
        } else {
            fprintf(stderr, "Warning: Unknown record type %u at %u. Aborting scan.\n", rh.type, cursor);
            break;
        }
    }
    
    // Checkpointing & Cleanup
    jh.nbytes_used = 0;
    jh.magic = JOURNAL_MAGIC; 
    write_j_bytes(fd, 0, sizeof(jh), &jh);
    
    uint32_t zero = 0;
    write_j_bytes(fd, 0, 4, &zero);
    
    fsync(fd);
    printf("Journal replayed and cleared.\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <create|install> [image_file] [filename]\n", argv[0]);
        return 1;
    }
    
    char *cmd = argv[1];
    char *image_path = "vsfs.img";
    char *filename = NULL;
    
    if (strcmp(cmd, "create") == 0) {
        // [create] [f1] -> argc 3. image="vsfs.img", filename="f1"
        // [create] [img] [f1] -> argc 4. image="img", filename="f1"
        if (argc == 3) {
            filename = argv[2];
        } else if (argc == 4) {
            image_path = argv[2];
            filename = argv[3];
        } else {
            fprintf(stderr, "Usage: %s create [image_file] <filename>\n", argv[0]);
            return 1;
        }
        
        int fd = open(image_path, O_RDWR);
        if (fd < 0) die("open");
        handle_create(fd, filename);
        close(fd);

    } else if (strcmp(cmd, "install") == 0) {
        // [install] -> argc 2. image="vsfs.img"
        // [install] [img] -> argc 3. image="img"
        if (argc == 3) {
            image_path = argv[2];
        } else if (argc > 3) {
             fprintf(stderr, "Usage: %s install [image_file]\n", argv[0]);
             return 1;
        }
        
        int fd = open(image_path, O_RDWR);
        if (fd < 0) die("open");
        handle_install(fd);
        close(fd);

    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        return 1;
    }
    
    return 0;
}