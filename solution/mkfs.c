#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <wfs.h>

#define FAIL -1
#define SUCCESS 0

// Function to round up to the nearest multiple of 32
size_t round_up_32(size_t value) {
    return ((value + 31) / 32) * 32;
}

// Function to write the superblock
void write_superblock(int fd, struct wfs_sb *sb) {
    if (lseek(fd, 0, SEEK_SET) == -1) {
        perror("lseek failed");
        exit(FAIL);
    }
    if (write(fd, sb, sizeof(struct wfs_sb)) != sizeof(struct wfs_sb)) {
        perror("Failed to write superblock");
        exit(FAIL);
    }
}


void initalize_disk(const char *disk_path, int disk_id, int raid_mode, int num_inodes, int num_data_blocks) {
    int fd = open(disk_path, O_CREAT | O_RDWR | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (fd < 0) {
        printf("Failed to open disk file\n");
        exit(FAIL);
    }
    off_t i_bitmap_ptr = BLOCK_SIZE;    
    size_t i_bitmap_size = round_up_32((num_inodes + 7) / 8); // Turn into bytes

    off_t d_bitmap_ptr = i_bitmap_ptr + i_bitmap_size;
    size_t d_bitmap_size = round_up_32((num_data_blocks + 7) / 8); // Turn into bytes

    // Each inode is 512bytes --> which is why we do num_inodes * BLOCK_SIZE
    off_t i_blocks_ptr = d_bitmap_ptr + d_bitmap_size;
    off_t d_blocks_ptr = i_blocks_ptr + (num_inodes * BLOCK_SIZE);

        // Populate superblock
    struct wfs_sb sb = {
        .num_inodes = num_inodes,
        .num_data_blocks = num_data_blocks,
        .i_bitmap_ptr = i_bitmap_ptr,
        .d_bitmap_ptr = d_bitmap_ptr,
        .i_blocks_ptr = i_blocks_ptr,
        .d_blocks_ptr = d_blocks_ptr,
        .raid_mode = raid_mode,
        .disk_id = disk_id
    };

    // Write the superblock to disk
    write_superblock(fd, &sb);

}

int main(int argc, char*argv []) {
    int raid_mode = -1;
    char * disk_files[10];
    int num_inodes = 0;
    int num_data_blocks = 0;
    int num_disks = 0;

    // Tokenize the command line arguments
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0) {
            if (i+1 > argc) {
                printf("Missing value for -r\n");
                exit(FAIL);
            }
            raid_mode = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "-d") == 0){
            if (i+1 > argc) {
                printf("Missing value for -dr\n");
                exit(FAIL);
            }
            disk_files[num_disks++] = argv[++i];
        }
        else if (strcmp(argv[i], "-i") == 0){
            if (i+1 > argc) {
                printf("Missing value for -i\n");
                exit(FAIL);
            }
            num_inodes = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "-b") == 0){
            if (i+1 > argc) {
                printf("Missing value for -b\n");
                exit(FAIL);
            }
            num_data_blocks = atoi(argv[++i]);
        }
        else {
            printf("Unknown Flag: %s\n", argv[i]);
            exit(FAIL);
        }
    }

    // Validate the arguments
    if (num_inodes <= 0 || num_data_blocks <= 0 || raid_mode < 0) {
        printf("Invalid arguments: num_inodes, num_data_blocks, and raid_mode must be positive.\n");
        exit(FAIL);
    }   

    // Round num_blocks to nearest multiple of 32
    if (num_data_blocks % 32 != 0) {
        num_data_blocks = (num_data_blocks / 32 + 1) * 32;
    }

    for (int i = 0; i < num_disks; i++) {
        initalize_disk(disk_files[i], i, raid_mode, num_inodes, num_data_blocks);
    }
    printf("Success\n");
    return SUCCESS;
}