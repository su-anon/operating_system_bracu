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
    uint32_t size;
    uint32_t type;      
    uint32_t block_no;  
    uint32_t checksum;  
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

//read block function
static void rd_blck(int fd, uint32_t blck_idx, void *buf) {
    off_t offset = (off_t)blck_idx * BLOCK_SIZE;
    ssize_t n = pread(fd, buf, BLOCK_SIZE, offset);
    if (n != (ssize_t)BLOCK_SIZE) {
        die("pread block");
    }
}

//read journal in byte function
static void rd_jrnl_byte(int fd, uint32_t offset, uint32_t size, void *buf) {
    off_t jrnl_start = (off_t)JOURNAL_BLOCK_IDX * BLOCK_SIZE;
    off_t abs_offset = jrnl_start + offset;

    ssize_t n = pread(fd, buf, size, abs_offset);
    if (n != (ssize_t)size) {
        die("pread journal");
    }
}

//write block function
static void wr_blk(int fd, uint32_t blck_idx, const void *buf) {
    off_t offset = (off_t)blck_idx * BLOCK_SIZE;
    ssize_t n = pwrite(fd, buf, BLOCK_SIZE, offset);
    if (n != (ssize_t)BLOCK_SIZE) {
        die("pwrite block");
    }
}

//write journal in byte function
static void wr_jrnl_byte(int fd, uint32_t offset, uint32_t size, const void *buf) {
    off_t jrnl_start = (off_t)JOURNAL_BLOCK_IDX * BLOCK_SIZE;
    off_t abs_offset = jrnl_start + offset;

    ssize_t n = pwrite(fd, buf, size, abs_offset);
    if (n != (ssize_t)size) {
        die("pwrite journal");
    }
}


//journal block validation check
static void chk_jrnl_header(int fd, struct journal_header *jh) {
    rd_blck(fd, JOURNAL_BLOCK_IDX, jh);
    if (jh->magic != JOURNAL_MAGIC) {
        // Initialize
        memset(jh, 0, sizeof(*jh));
        jh->magic = JOURNAL_MAGIC;
        jh->nbytes_used = 0; 
    }
}



//append each journal create
static void rd_updated_blk(int fd, uint32_t block_idx, void *buf) {
    rd_blck(fd, block_idx, buf);

    struct journal_header jh;
    rd_blck(fd, JOURNAL_BLOCK_IDX, &jh);
    
    if (jh.magic != JOURNAL_MAGIC) {
        return;
    }

    uint32_t journal_pointer = BLOCK_SIZE; 
    uint32_t end_offset = BLOCK_SIZE + jh.nbytes_used;

    while (journal_pointer < end_offset) {
        struct rec_header rh;
        rd_jrnl_byte(fd, journal_pointer, sizeof(rh), &rh);

        if (rh.type == REC_DATA) {
            if (rh.block_no == block_idx) {
                rd_jrnl_byte(fd, journal_pointer + sizeof(rh), BLOCK_SIZE, buf);
            }
            journal_pointer += sizeof(rh) + BLOCK_SIZE;
        } else if (rh.type == REC_COMMIT) {
            journal_pointer += sizeof(rh);
        } else {
            break;
        }
    }
}

//search for free inodes through bitmap
static int free_inode(int fd, struct superblock *sb, uint32_t *inode_idx) {
    uint8_t bitmap[BLOCK_SIZE];
    rd_updated_blk(fd, sb->inode_bitmap, bitmap);
    
    for (uint32_t i = 0; i < sb->inode_count; i++) {
        uint32_t byte_idx = i / 8; 
        uint32_t bit_pos = i % 8;
        uint8_t mask = (1 << bit_pos); 

        if ((bitmap[byte_idx] & mask) == 0) { 
            *inode_idx = i; 
            return 1; 
        }
    } 
    return 0;
}


//searches for free blocks for new file entry
static int free_dirent(int fd, struct superblock *sb, const char *name, uint32_t *out_blk_idx, uint32_t *out_offset) {    
    uint8_t inode_table[BLOCK_SIZE];
    rd_updated_blk(fd, sb->inode_start, inode_table);
    struct inode *root = (struct inode *)inode_table;
    
    //scan for direct pointers
    for (uint32_t i = 0; i < DIRECT_POINTERS; i++) {
        uint32_t blk = root->direct[i];
        if (blk == 0) continue;
        
        uint8_t data_blk[BLOCK_SIZE];
        rd_updated_blk(fd, blk, data_blk);
        
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


//Create
static void handle_create(int fd, const char *filename) {
    struct superblock sb;
    rd_updated_blk(fd, 0, &sb); //read the superblock
    
    struct journal_header jh;
    chk_jrnl_header(fd, &jh); //journal validator
    
    uint32_t free_inode_idx;
    if (!free_inode(fd, &sb, &free_inode_idx)) {
        fprintf(stderr, "Error: No free inodes.\n"); //searches available inodes
        return;
    }
    
    uint32_t dir_blk, dir_off;
    if (!free_dirent(fd, &sb, filename, &dir_blk, &dir_off)) {
         fprintf(stderr, "Error: Root directory full.\n"); //finds empty slot in root dir
         return;
    }
    
    //update inode bitmap
    uint8_t inode_bitmap[BLOCK_SIZE];
    rd_updated_blk(fd, sb.inode_bitmap, inode_bitmap);
    //calculates the byte and bit position to find free inode
    uint32_t change_byte = free_inode_idx / 8;
    uint32_t set_bit = free_inode_idx % 8;
    uint8_t bit_mask = (1 << set_bit);
    inode_bitmap[change_byte] |= bit_mask; //bitwise or operation
        
    //update inode table
    //calculates inode block location
    uint32_t inode_blk_offset = (free_inode_idx * sizeof(struct inode));
    uint32_t inode_blk_idx = sb.inode_start + (inode_blk_offset / BLOCK_SIZE);
    uint32_t inode_within_blk = inode_blk_offset % BLOCK_SIZE;
    
    //initializes new inode
    uint8_t inode_table_blk[BLOCK_SIZE]; //temp buffer array to store metadata
    rd_updated_blk(fd, inode_blk_idx, inode_table_blk);
    struct inode *new_inode = (struct inode *)(inode_table_blk + inode_within_blk);
    
    memset(new_inode, 0, sizeof(*new_inode));
    new_inode->type = 1; 
    new_inode->links = 1; 
    new_inode->size = 0;
    new_inode->ctime = time(NULL);
    new_inode->mtime = time(NULL);
    
    //root dir size updates
    if (inode_blk_idx == sb.inode_start) {
        struct inode *root_inode = (struct inode *)(inode_table_blk);
        uint32_t req_size = dir_off + sizeof(struct dirent);
        if (req_size > root_inode->size) {
            root_inode->size = req_size;
        }
    } else {
        fprintf(stderr, "Warning: Root inode not in same block as new inode. Size update skipped.\n");
    }
    
    //updates dir entry, handles filename (in RAM)
    uint8_t dir_data_blk[BLOCK_SIZE];
    rd_updated_blk(fd, dir_blk, dir_data_blk);
    struct dirent *de = (struct dirent *)(dir_data_blk + dir_off);
    de->inode = free_inode_idx;
    strncpy(de->name, filename, sizeof(de->name) - 1);
    

    //journal capacity calculation    
    uint32_t needed = 3 * (sizeof(struct rec_header) + BLOCK_SIZE) + sizeof(struct rec_header);
    uint32_t capacity = (JOURNAL_BLOCKS - 1) * BLOCK_SIZE;
    
    if (jh.nbytes_used + needed > capacity) {
        fprintf(stderr, "Error: Journal full.\n");
        return;
    }
    
    uint32_t current_offset = jh.nbytes_used;
    uint32_t base_offset = BLOCK_SIZE + jh.nbytes_used;
    uint32_t pos = base_offset;
    
    struct rec_header rh; //record header
    
    //inode bitmap logging of the new file
    rh.size = BLOCK_SIZE;
    rh.type = REC_DATA;
    rh.block_no = sb.inode_bitmap;
    rh.checksum = 0;
    wr_jrnl_byte(fd, pos, sizeof(rh), &rh);
    pos += sizeof(rh);
    wr_jrnl_byte(fd, pos, BLOCK_SIZE, inode_bitmap);
    pos += BLOCK_SIZE;
    
    //logs inode table block
    rh.block_no = inode_blk_idx;
    wr_jrnl_byte(fd, pos, sizeof(rh), &rh);
    pos += sizeof(rh);
    wr_jrnl_byte(fd, pos, BLOCK_SIZE, inode_table_blk);
    pos += BLOCK_SIZE;
    
    //logs dir entry data block
    rh.block_no = dir_blk;
    wr_jrnl_byte(fd, pos, sizeof(rh), &rh);
    pos += sizeof(rh);
    wr_jrnl_byte(fd, pos, BLOCK_SIZE, dir_data_blk);
    pos += BLOCK_SIZE;
    
    //journal commit
    rh.size = 0;
    rh.type = REC_COMMIT;
    rh.block_no = 0; 
    rh.checksum = 0;
    wr_jrnl_byte(fd, pos, sizeof(rh), &rh);
    pos += sizeof(rh);
    
    //updates journal header
    jh.nbytes_used += needed;
    wr_jrnl_byte(fd, 0, sizeof(jh), &jh);
    
    //fsync to ensure it hits disk
    fsync(fd);
    
    printf("Create transaction for '%s' file was successful!.\n", filename);
}


//Install
#define MAX_QUEUE 64
struct prep_write {
    uint32_t block_no;
    uint8_t data[BLOCK_SIZE];
};

static void handle_install(int fd) {
    struct journal_header jh;
    chk_jrnl_header(fd, &jh);
    
    if (jh.nbytes_used == 0) {
        printf("Journal empty, nothing to install.\n");
        return;
    }
    
    //read journal blocks
    uint32_t journal_pointer = BLOCK_SIZE;
    uint32_t end_offset = BLOCK_SIZE + jh.nbytes_used;
    
    struct prep_write q[MAX_QUEUE];
    int q_count = 0;
    int transaction_count = 1;
    while (journal_pointer < end_offset) {
        struct rec_header rh;
        rd_jrnl_byte(fd, journal_pointer, sizeof(rh), &rh);
        
        if (rh.type == REC_DATA) {

            printf("Reading journal record at offset %u... found DATA for block %u\n", journal_pointer, rh.block_no);
            //read data block
            if (q_count >= MAX_QUEUE) {
                fprintf(stderr, "Error: Too many queued writes.\n");
                return; 
            }
            q[q_count].block_no = rh.block_no;
            rd_jrnl_byte(fd, journal_pointer + sizeof(rh), BLOCK_SIZE, q[q_count].data);
            q_count++;
            
            journal_pointer += sizeof(rh) + BLOCK_SIZE;
        } else if (rh.type == REC_COMMIT) {
            //apply transaction
            printf("Reading journal record at offset %u... found COMMIT.\n", journal_pointer);
            printf("--> Transaction %d verified. Applying %d blocks.\n", transaction_count, q_count);
            for (int i = 0; i < q_count; i++) {
                wr_blk(fd, q[i].block_no, q[i].data);
            }
            q_count = 0;
            transaction_count++;
            journal_pointer += sizeof(rh);
        } else {
            fprintf(stderr, "Warning: Unknown record type %u at %u. Aborting scan.\n", rh.type, journal_pointer);
            break;
        }
    }
    
    //reset journal
    jh.nbytes_used = 0;
    jh.magic = JOURNAL_MAGIC;
    wr_jrnl_byte(fd, 0, sizeof(jh), &jh);
    
    uint32_t zero = 0;
    wr_jrnl_byte(fd, 0, 4, &zero);
    
    fsync(fd);
    printf("Journal replayed and cleared.\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <create|install> [filename]\n", argv[0]);
        return 1;
    }
    
    char *cmd = argv[1];
    char *img_path = "vsfs.img";
    char *filename = NULL;
    
    if (strcmp(cmd, "create") == 0) {
        if (argc == 3) {
            filename = argv[2];
        } else if (argc == 4) {
            img_path = argv[2];
            filename = argv[3];
        } else {
            fprintf(stderr, "Usage: %s create <filename>\n", argv[0]);
            return 1;
        }
        
        int fd = open(img_path, O_RDWR);
        if (fd < 0) die("open");
        handle_create(fd, filename);
        close(fd);

    } else if (strcmp(cmd, "install") == 0) {
        if (argc == 3) {
            img_path = argv[2];
        } else if (argc > 3) {
             fprintf(stderr, "Usage: %s install\n", argv[0]);
             return 1;
        }
        
        int fd = open(img_path, O_RDWR);
        if (fd < 0) die("open");
        handle_install(fd);
        close(fd);

    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        return 1;
    }
    
    return 0;
}