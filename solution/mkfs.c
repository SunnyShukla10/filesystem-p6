#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "wfs.h"

#define FAIL 1
#define SUCCESS 0

//rounds up to the nearest multiple of 32
size_t round_32(size_t value) {
    return ((value + (32-1)) / 32) * 32;
}

//rounds up to the nearest multiple of 512
size_t round_512(size_t value) {
    return ((value + (BLOCK_SIZE - 1)) / BLOCK_SIZE) * BLOCK_SIZE;
}

void initalize_disk(const char *disk_path, int disk_id, int raid_mode, int num_inodes, int num_data_blocks) {
    
    //open disk file, set user permissions
    int fd = open(disk_path, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

    if (fd < 0){
        exit(FAIL);
    }

    num_data_blocks = round_32(num_data_blocks);

    //layout offsets
    //bitmaps = track which indoes or data blocks are inuse or free

    //inode bit map
    off_t i_bitmap_ptr = sizeof(struct wfs_sb);

    //number of bytes
    size_t i_bitmap_size = num_inodes / 8;

    //data block bit map 
    off_t d_bitmap_ptr = i_bitmap_ptr + i_bitmap_size;

    //number of bytes
    size_t d_bitmap_size = num_data_blocks / 8;

    //where inode blocks begin (inode info stored starting here)
    off_t i_blocks_ptr = round_512(d_bitmap_ptr + d_bitmap_size);

    //where data blocks begin (actual file content or directory entries stored here)
    //each inode allocated fixed block size 
    off_t d_blocks_ptr = round_512(i_blocks_ptr + (num_inodes * BLOCK_SIZE));

    size_t total_size = d_blocks_ptr + (num_inodes * sizeof(struct wfs_inode)) + (num_data_blocks * BLOCK_SIZE);
    
    //get the disk file size
    struct stat disk_stat;

    if(fstat(fd, &disk_stat) == -1){
        close(fd);
        exit(FAIL);
    }

    //check if disk file size big enough
    if(disk_stat.st_size < total_size){
        close(fd);

        //return code based on directions
        exit(-1);
    }

    //update superblock
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

    //write to superblock
    //lseek --> where program will read or write within file
    if(lseek(fd, 0, SEEK_SET) == -1){
        close(fd);
        exit(FAIL);
    }

    //writes data to file
    //writes data (sb) to open file (fd) 
    if(write(fd, &sb, sizeof(struct wfs_sb)) != sizeof(struct wfs_sb)){
        close(fd);
        exit(FAIL);
    }

    //manually allocate memory for inode bitmap
    unsigned char i_bitmap[(i_bitmap_size)];

    //set all bits to 0
    memset(i_bitmap, 0, i_bitmap_size);

    //set root bit (inode 0) to allocated
    i_bitmap[0] |= 1;

    //where program should write i bitmap info in file on disk
    if(lseek(fd, i_bitmap_ptr, SEEK_SET) == -1){
        close(fd);
        exit(FAIL);
    }

    if(write(fd, i_bitmap, i_bitmap_size) != i_bitmap_size){
        close(fd);
        exit(FAIL);
    }

    // Initialize the root inode (inode 0)
    struct wfs_inode root_inode = {
        .num = 0,
        .mode = S_IFDIR | 0755,  // directory permissions rwxr-xr-x
        .uid = getuid(),
        .gid = getgid(),
        .size = 0,
        .nlinks = 2,  // current and parent directory (. and ..)
        .atim = time(NULL),
        .mtim = time(NULL),
        .ctim = time(NULL)
    };

    //intialize to 0
    memset(root_inode.blocks, 0, sizeof(root_inode.blocks));  

    // Write root inode to disk
    if (lseek(fd, i_blocks_ptr, SEEK_SET) == -1){ 
        close(fd);
        exit(FAIL);
    }

    if(write(fd, &root_inode, sizeof(struct wfs_inode)) != sizeof(struct wfs_inode)){
        close(fd);
        exit(FAIL);
    }

    close(fd);

}

int main(int argc, char*argv []) {
    int raid_mode = -1;
    char * disk_files[10];
    int num_inodes = 0;
    int num_data_blocks = 0;
    int num_disks = 0;

    // Tokenize the command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0) {
            if (i+1 > argc) {
                exit(FAIL);
            }

            if(strcmp(argv[i+1], "0") == 0){
                raid_mode = 0;
            }else if(strcmp(argv[i+1], "1") == 0){
                raid_mode = 1;
            }else if(strcmp(argv[i+1], "1v") == 0){
                raid_mode = 2;
            }else{
                exit(FAIL);
            }
            i++;
        }
        else if (strcmp(argv[i], "-d") == 0){
            if (i+1 > argc) {
                exit(FAIL);
            }
            disk_files[num_disks++] = argv[++i];
        }
        else if (strcmp(argv[i], "-i") == 0){
            if (i+1 > argc) {
                exit(FAIL);
            }
            num_inodes = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "-b") == 0){
            if (i+1 > argc) {
                exit(FAIL);
            }
            num_data_blocks = atoi(argv[++i]);
        }
        else {
            exit(FAIL);
        }
    }

    // Validate the arguments
    if (raid_mode < 0 || raid_mode > 3 || num_data_blocks <= 0 || num_disks == 0 || num_inodes <= 0) {
        exit(FAIL);
    }

    if(raid_mode == 1 && num_disks <= 1){
        exit(FAIL);
    }

    //should be multiple of nearest 32
    num_inodes = round_32(num_inodes);

    for (int i = 0; i < num_disks; i++) {
        initalize_disk(disk_files[i], i, raid_mode, num_inodes, num_data_blocks);
    }

    return SUCCESS;
}