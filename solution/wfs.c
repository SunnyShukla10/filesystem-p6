#define FUSE_USE_VERSION 30
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <fuse.h>
#include "wfs.h"
#include <libgen.h>


// Global variables for memory-mapped regions and disk names
int num_disks;
int raid_mode;
int NUM_DENTRIES_PER_BLOCK = BLOCK_SIZE / sizeof(struct wfs_dentry);
void *disk_region[MAX_DISKS];
char *disk_names[MAX_DISKS];
size_t disk_sizes[MAX_DISKS]; // To munmap


/////////////////////////////////////////////////// HELPER FUNCTIONS ///////////////////////////////////////////////////////////

// Gets the superblock (get it from disk 0 since the superblock will be the same for all the disks)
struct wfs_sb *get_superblock() {
    return (struct wfs_sb *)DISK_MAP_PTR(0, 0);
}

// Getter functions for 
int get_disk(int i) {
    return i % num_disks;
}

// Gets the inode given an inode_num
struct wfs_inode *get_inode(int inode_num) {
    printf("get_inode: Accessing inode number %d\n", inode_num);
    struct wfs_sb *sb = get_superblock();
    off_t inode_table_offset = sb->i_blocks_ptr;

    return (struct wfs_inode *)(DISK_MAP_PTR(sb->disk_id, inode_table_offset) + inode_num * BLOCK_SIZE);
}

int find_disk(struct wfs_inode *dir_inode, int calling_function) {
    if (calling_function == MK_DIR_AND_NODE) {
   
        printf("find_disk for MK_DIR_AND_NODE: Checking available space for inode %d\n", dir_inode->num);

        // Iterate through existing blocks to check for free space
        for (int i = 0; i < D_BLOCK + 1; i++) {
            if (dir_inode->blocks[i] != 0) {
                char *data_block = DISK_MAP_PTR(0, dir_inode->blocks[i]);
                printf("find_disk: Inspecting block %d at address %p\n", i, data_block);

                for (int j = 0; j < NUM_DENTRIES_PER_BLOCK; j++) {
                    struct wfs_dentry *dentry = (struct wfs_dentry *)(data_block + j * sizeof(struct wfs_dentry));
                    if (dentry->name[0] == '\0' || dentry->num == 0) {
                        // Free space found in an existing block
                        int disk_to_use = get_disk(i);
                        printf("find_disk: Found space in block %d on disk %d\n", i, disk_to_use);
                        return disk_to_use;
                    }
                }
            }
        }

        // No space found in existing blocks, find next disk for new block
        int used_blks = 0;
        for (int i = 0; i < N_BLOCKS - 1; i++) {
            if (dir_inode->blocks[i] != 0) {
                used_blks++;
            }
        }
        int disk_to_use = get_disk(used_blks);
        printf("find_disk: No free space, allocating on disk %d for new block\n", disk_to_use);

        return disk_to_use;
    }

    printf("find_disk: calling type not defined\n");
    return -1;
}


int allocate_free_data_block(int disk_id) {
    printf("allocate_free_data_block: Searching for a free data block for raid %d\n", raid_mode);
   
    struct wfs_sb *sb = get_superblock();
    char *data_bitmap = DISK_MAP_PTR(disk_id, sb->d_bitmap_ptr);

    // Iterate through all the bytes and bits and mark the first free bit to used
    for (int i = 0; i < sb->num_data_blocks; i++) {
        if (!(data_bitmap[i / 8] & (1 << (i % 8)))) { // Check if the block is free
            printf("allocate_free_data_block: Found free block at index %d\n", i);
            data_bitmap[i / 8] |= (1 << (i % 8)); // Mark as used
            return sb->d_blocks_ptr + i * BLOCK_SIZE; // Return the block address
        }
    }
   
    printf("allocate_free_data_block: No free data blocks available\n");
    return 0; 
}

int allocate_free_inode() {
    printf("allocate_free_inode: Searching for a free inode\n");
    struct wfs_sb *sb = get_superblock();
    char *i_bitmap = DISK_MAP_PTR(sb->disk_id, sb->i_bitmap_ptr);

    // Iterate through all the bytes and bits and mark the first free bit to used
    for (int i = 0; i < sb->num_inodes; i++) {
        if (!(i_bitmap[i / 8] & (1 << (i % 8)))) { // Check if bit is free
            printf("allocate_free_inode: Allocated inode %d\n", i);
            i_bitmap[i / 8] |= (1 << (i % 8)); // Mark as used
            return i;
        }
    }
    printf("allocate_free_inode: No free inodes available\n");
    return -1;
}

void free_inode(int i_num) {
    printf("free_inode: Freeing inode number %d\n", i_num);
    struct wfs_sb *sb = get_superblock();
    char *i_bitmap = DISK_MAP_PTR(sb->disk_id, sb->i_bitmap_ptr);
    i_bitmap[i_num / 8] &= ~(1 << (i_num % 8)); // Mark as free
}


struct wfs_dentry *find_dentry_in_directory(struct wfs_inode *dir_inode, const char *name_to_add) {
    printf("find_dentry_in_directory: Searching for entry '%s' in directory inode %d\n", name_to_add, dir_inode->num);
    char *data_block;
    struct wfs_dentry *dentry;

    for (int i = 0; i < D_BLOCK + 1; i++) {
        if (dir_inode->blocks[i] == 0) {
            continue; // Skip empty blocks
        }
               
        // Iterate over all the disks to find the dentry
        if (raid_mode == 0){
            for (int disk = 0; disk < num_disks; disk++) {
                printf("find_dentry_in_directory: Checking block %d on disk %d with block address %ld\n", i, disk, dir_inode->blocks[i]);

                data_block = DISK_MAP_PTR(disk, dir_inode->blocks[i]);
                // Search over all dentries and see if we find the matching dentry
                for (int j = 0; j < NUM_DENTRIES_PER_BLOCK; j++) {
                    dentry = (struct wfs_dentry *)(data_block + j * sizeof(struct wfs_dentry));
                    if (strncmp(dentry->name, name_to_add, MAX_NAME) == 0) {
                        printf("find_dentry_in_directory: Found matching entry '%s', inode num: %d\n", dentry->name, dentry->num);
                        return dentry;
                    }
                }
            }
            printf("find_dentry_in_directory: Entry '%s' not found\n", name_to_add);
            return NULL;
        }
        // RAID 1
        else {
            printf("find_dentry_in_directory: Checking block %d on disk %d\n", i, 0);

            data_block = DISK_MAP_PTR(0, dir_inode->blocks[i]);
            printf("find_dentry_in_directory: Accessing data block at address %p\n", data_block);

            // Search over all dentries and see if we find the matching dentry
            for (int j = 0; j < NUM_DENTRIES_PER_BLOCK; j++) {
                dentry = (struct wfs_dentry *)(data_block + j * sizeof(struct wfs_dentry));
                if (strncmp(dentry->name, name_to_add, MAX_NAME) == 0) {
                    printf("find_dentry_in_directory: Found matching entry '%s', inode num: %d\n", dentry->name, dentry->num);
                    return dentry;
                }
            }
        }
    }

    printf("find_dentry_in_directory: Entry '%s' not found\n", name_to_add);
    return NULL;
}

struct wfs_inode *find_inode_by_path(const char *path) {
    printf("find_inode_by_path: Searching for path: %s\n", path);

    if (path[0] != '/') {
        printf("find_inode_by_path: Must start with '/'\n");
        return NULL;
    }

    // Get the root inode
    struct wfs_inode *current_inode = get_inode(0);
    if (!current_inode) {
        printf("find_inode_by_path: Failed to retrieve root inode\n");
        return NULL;
    }

    // If the path is only he root directory, return the root inode
    if (strcmp(path, "/") == 0) {
        printf("find_inode_by_path: Path is root directory\n");
        return current_inode;
    }

    // Otherwise, tokenize the path by "/" and get the corresponding dentry per token
    char *path_copy = strdup(path);
    if (!path_copy) {
        printf("find_inode_by_path: Failed to allocate memory for path copy");
        return NULL;
    }
    char *token = strtok(path_copy + 1, "/");

    while (token) {
        printf("find_inode_by_path: Current token: %s\n", token);

        // Make sure it is a directory
        if (!S_ISDIR(current_inode->mode)) {
            printf("find_inode_by_path: Path component '%s' is not a directory\n", token);
            free(path_copy);
            return NULL;
        }

        // Get the dentry given token 
        struct wfs_dentry *entry = find_dentry_in_directory(current_inode, token);
        if (!entry) {
            printf("find_inode_by_path: Path component '%s' not found\n", token);
            free(path_copy);
            return NULL;
        }

        // use the inode that the entry points to
        current_inode = get_inode(entry->num);
        if (!current_inode) {
            printf("find_inode_by_path: Failed to retrieve inode for '%s'\n", token);
            free(path_copy);
            return NULL;
        }

        token = strtok(NULL, "/"); // go to the next token
    }

    free(path_copy);
    printf("find_inode_by_path: Successfully found inode for path: %s\n", path);
    return current_inode; // Return the inode we found after iterating the path
}

int add_dentry_to_directory(struct wfs_inode *dir_inode, struct wfs_dentry *entry, const char *dir_name, int new_inode_num) {
    printf("add_dentry_to_directory: RAID mode: %d\n", raid_mode);
    
    char *data_block;
    struct wfs_dentry *dentry;
    int target_disk = 0;

    struct wfs_inode *inode;
    struct wfs_sb *sb = get_superblock();

    if (raid_mode == 0) {
        // Use find_disk to determine the target disk
        target_disk = find_disk(dir_inode, MK_DIR_AND_NODE);
        printf("add_dentry_to_directory: Target disk determined by find_disk is %d\n", target_disk);

        int inode_num = dir_inode->num;
        inode = (struct wfs_inode *)DISK_MAP_PTR(target_disk, sb->i_blocks_ptr + inode_num*BLOCK_SIZE);

        // For RAID-0, calculate which disk should hold the next block
        int blk_num = (new_inode_num - 1) / NUM_DENTRIES_PER_BLOCK;
        target_disk = get_disk(blk_num);   

        // Only allocate a new block if this is the first entry for this block on this disk
        if (new_inode_num % NUM_DENTRIES_PER_BLOCK == 1 && new_inode_num != 1) {
            off_t blk_addr = allocate_free_data_block(target_disk);
            if (blk_addr >= 0) {
                int blk_idx = blk_num;
                dir_inode->blocks[blk_idx] = blk_addr;
                printf("add_dentry_to_directory: Allocated new block %d on disk %d for entry %d\n",
                    blk_idx, target_disk, new_inode_num);
            }
        }
    } else {
        // Default target_disk logic for non-RAID 0 modes
        target_disk = 0; 
        inode = dir_inode;
    }

    printf("add_dentry_to_directory: Using target disk: %d\n", target_disk);
    
    // Look over each block in blocks array 
    for (int i = 0; i < D_BLOCK + 1; i++) {
        printf("Checking block %d\n", i);
        
        if (inode->blocks[i] == 0) {
            printf("Block %d is zero, attempting to allocate\n", i);
            inode->blocks[i] = allocate_free_data_block(target_disk);
            
            if (inode->blocks[i] == 0) {
                printf("Failed to allocate data block for disk %d\n", target_disk);
                return -1; 
            }
            printf("Allocated block %d on disk %d\n", i, target_disk);
        }
        
        data_block = DISK_MAP_PTR(target_disk, inode->blocks[i]);        

        // Loop over all the dentries and see if we can add to an empty slot
        for (int j = 0; j < NUM_DENTRIES_PER_BLOCK; j++) {
            dentry = (struct wfs_dentry *)(data_block + j * sizeof(struct wfs_dentry));
            if (dentry->name[0] == '\0') { // Empty slot
                *dentry = *entry; // Copy entry
                printf("Added dentry '%s' in block %d, slot %d\n", dir_name, i, j);
                return 0;
            }
        }
    }

    printf("No space left in directory inode %d\n", inode->num);
    return -1;
}

// -----------------------Helper functions to synchronize disks--------------------------------------
static void sync_disks_for_raid1(int s_disk) {
    struct wfs_sb *sb = get_superblock();  // Access superblock from disk 0
    size_t copy_size = sb->d_blocks_ptr + (sb->num_data_blocks * BLOCK_SIZE) - sb->i_bitmap_ptr;

    for (int disk = 0; disk < num_disks; disk++) {
        if (disk != s_disk) {
            // Copy everything after the superblock (inode bitmap, data bitmap, inodes, data blocks)
            memcpy(DISK_MAP_PTR(disk, sb->i_bitmap_ptr),   // destination
                   DISK_MAP_PTR(s_disk, sb->i_bitmap_ptr),  // source
                   copy_size);
        }
    }
    printf("sync_disks_for_raid1: Synchronized data from disk %d to other disks\n", s_disk);
}

static void sync_disks_for_raid0 (int s_disk) {
    struct wfs_sb *sb = get_superblock();
    size_t i_bitmap_size = sb->d_bitmap_ptr - sb->i_bitmap_ptr;
    size_t i_size = sb->d_blocks_ptr - sb->i_blocks_ptr;

    for (int disk = 0; disk < num_disks; disk++) {
        if (disk != s_disk) {
            memcpy(DISK_MAP_PTR(disk, sb->i_bitmap_ptr),   // destination
                   DISK_MAP_PTR(s_disk, sb->i_bitmap_ptr),  // source
                   i_bitmap_size);

            memcpy(DISK_MAP_PTR(disk, sb->i_blocks_ptr),
                    DISK_MAP_PTR(s_disk, sb->i_blocks_ptr),
                    i_size);
        }
    }
    printf("sync_disks_for_raid0: Synchronized metadata from disk %d to other disks\n", s_disk);
}
// -----------------------------------------------------------------------------------------------------


int remove_directory_helper(struct wfs_inode *parent_inode, struct wfs_inode *target_inode, const char *target_dir) {
    // Check if directory is empty
    int is_directory_empty = 1;
    for (int block_idx = 0; block_idx < D_BLOCK; block_idx++) {
        if (target_inode->blocks[block_idx] == 0) {
            continue;
        }
        // Check correct disk for the block in RAID 0
        int target_disk;
        if (raid_mode == 0) {
            target_disk = block_idx % num_disks;
        } else {
            target_disk = 0;
        }

        char *block_data = DISK_MAP_PTR(target_disk, target_inode->blocks[block_idx]);
        for (int entry_idx = 0; entry_idx < NUM_DENTRIES_PER_BLOCK; entry_idx++) {
            struct wfs_dentry *current_entry = (struct wfs_dentry *)(block_data + entry_idx * sizeof(struct wfs_dentry));
            // Skip '.' and '..'
            if (current_entry->name[0] != '\0' &&
                strcmp(current_entry->name, ".") != 0 &&
                strcmp(current_entry->name, "..") != 0) {
                is_directory_empty = 0;
                break;
            }
        }
        if (!is_directory_empty) {
            break;
        }
    }

    if (!is_directory_empty) {
        printf("remove_directory_helper: Directory is not empty\n");
        return FAIL;
    }

    struct wfs_sb *sb = get_superblock();
    char *parent_block_data;
    struct wfs_dentry *curr_dentry;
    int entry_found = 0;


    // Remove directory entry from parent
    for (int i = 0; i < D_BLOCK; i++) {
        if (parent_inode->blocks[i] == 0) {
            continue;
        }

        int target_disk;
        if (raid_mode == 0) {
            target_disk = get_disk(i);
        } else {
            target_disk = sb->disk_id;
        }
        parent_block_data = DISK_MAP_PTR(target_disk, parent_inode->blocks[i]);
        for (int j = 0; j < NUM_DENTRIES_PER_BLOCK; j++) {
            curr_dentry = (struct wfs_dentry *)(parent_block_data + j * sizeof(struct wfs_dentry));

            if (strncmp(curr_dentry->name, target_dir, MAX_NAME) == 0) {
                // Clear directory entry
                memset(curr_dentry, 0, sizeof(struct wfs_dentry));
                entry_found = 1;
                break;
            }
        }
        if (entry_found) {
            break;
        }
    }

    if (!entry_found) {
        return -ENOENT;
    }

    // Free directory blocks
    for (int i = 0; i < D_BLOCK; i++) {
        if (target_inode->blocks[i] != 0) {
            char *data_bitmap;
            int target_disk;

            if (raid_mode == 0) {
                target_disk = i % num_disks;
            } else {
                target_disk = sb->disk_id;
            }
            data_bitmap = DISK_MAP_PTR(target_disk, sb->d_bitmap_ptr);
    
            int blk_idx = (target_inode->blocks[i] - sb->d_blocks_ptr) / BLOCK_SIZE;
            int byte_index = blk_idx / 8;
            int bit_position = blk_idx % 8;
            data_bitmap[byte_index] &= ~(1 << bit_position);
        }
    }

    free_inode(target_inode->num);

    // Update parent metadata
    parent_inode->nlinks--;
    parent_inode->mtim = time(NULL);

    if (raid_mode == 1 && num_disks > 1) {
        sync_disks_for_raid1(0);
    } else if (raid_mode == 0 && num_disks > 1) {
        sync_disks_for_raid0(0);
    }

    return SUCCESS;
}


int unlink_file_helper(struct wfs_inode *parent_inode, struct wfs_inode *target_inode, const char *target_file) {
    struct wfs_sb *sb = get_superblock();
    struct wfs_dentry *curr_dentry;
    
    printf("unlink_file_helper: finding the dentry for %s\n", target_file);
    curr_dentry = find_dentry_in_directory(parent_inode, target_file);
    if (!curr_dentry) {
        return -ENOENT;
    }
    else {
        printf("unlink_file_helper: memsetting the dentry\n");
        memset(curr_dentry, 0, sizeof(struct wfs_dentry));
    }

    printf("unlink_file_helper: freeing the blocks and inodes now\n");
    // Free blocks and inodes
    for (int i = 0; i < D_BLOCK + 1; i++) {
        if (target_inode->blocks[i] != 0) {
            char *data_bitmap;
            int target_disk;

            if (raid_mode == 0) {
                // Find disk based on block index
                target_disk = i % num_disks;
                data_bitmap = DISK_MAP_PTR(target_disk, sb->d_bitmap_ptr);
            } else {
                // For raid 1/1v use default disk
                target_disk = sb->disk_id;
                data_bitmap = DISK_MAP_PTR(target_disk, sb->d_bitmap_ptr);
            }
            printf("unlink_file_helper: about to free the inode block %d from disk %d\n", i, target_disk);
            int blk_idx = (target_inode->blocks[i] - sb->d_blocks_ptr) / BLOCK_SIZE;
            int byte_index = blk_idx / 8;
            int bit_position = blk_idx % 8;
            data_bitmap[byte_index] &= ~(1 << bit_position);
        }
    }
    
    printf("unlink_file_helper: freed the indirect blocks, now to free the indirect blocks\n");

    // Free indirect blocks
    if (target_inode->blocks[IND_BLOCK] != 0) {
        printf("unlink_file_helper: block address of ind_block is %ld\n", target_inode->blocks[IND_BLOCK]);
        // Find correct disk
        int indirect_block_disk;
        if (raid_mode == 0) {
            indirect_block_disk = (IND_BLOCK) % num_disks;
        } else {
            indirect_block_disk = sb->disk_id;
        }

        printf("unlink_file_helper: disk we will use for indirect block is %d\n", indirect_block_disk);
        off_t *indirect_block = (off_t *)DISK_MAP_PTR(indirect_block_disk, target_inode->blocks[N_BLOCKS - 1]);
        char *bitmap = DISK_MAP_PTR(indirect_block_disk, sb->d_bitmap_ptr);

        printf("unlink_file_helper: we will look at each block of indirect block now\n");
        for (int i = 0; i < BLOCK_SIZE / sizeof(off_t); i++) {
            if (indirect_block[i] != 0) {
                fprintf(stderr,"unlink_file_helper: indirect block at index %d was allocated\n", i);
                // Find disk for each block
                int disk = (raid_mode == 0) ? (i % num_disks) : indirect_block_disk;
                int blk_idx = (indirect_block[i] - sb->d_blocks_ptr) / BLOCK_SIZE;
                char *disk_bitmap = DISK_MAP_PTR(disk, sb->d_bitmap_ptr);
                int byte_index = blk_idx / 8;
                int bit_position = blk_idx % 8;
                
                printf("indirect block address: %ld and d_blocks_ptr: %ld\n", indirect_block[i], sb->d_blocks_ptr);
                printf("disk: %d -- block index:%d -- byte idx: %d -- bit position: %d\n", disk, blk_idx, byte_index, bit_position);
                disk_bitmap[byte_index] &= ~(1 << bit_position);
            } else {
                printf("unlink_file_helper: indirect block at index %dwas unallocated\n", i);
            }
        }
        // Find indirect block
        int indirect_block_index = (target_inode->blocks[N_BLOCKS - 1] - sb->d_blocks_ptr) / BLOCK_SIZE;
        int byte_index = indirect_block_index / 8;
        int bit_position = indirect_block_index % 8;
        bitmap[byte_index] &= ~(1 << bit_position);
    }
    free_inode(target_inode->num);

    return SUCCESS;
}


int process_directory_blocks(struct wfs_inode *dir_inode, void *output_buffer, fuse_fill_dir_t entry_to_buffer) {
    // Go through all blocks in directory
    for (int block_index = 0; block_index < D_BLOCK; block_index++) {
        // Skip unallocated blocks
        if (dir_inode->blocks[block_index] == 0) {
            continue;
        }

        int disk = 0;
        // Find exact disk for raid 0
        if (raid_mode == 0) {
            disk = block_index % num_disks;
        }
        // Get data block with offset
        char *data_block_ptr = DISK_MAP_PTR(disk, dir_inode->blocks[block_index]);

        // Go through all entries in block
        for (int entry_index = 0; entry_index < BLOCK_SIZE / sizeof(struct wfs_dentry); entry_index++) {

            struct wfs_dentry *dentry = (struct wfs_dentry *)(entry_index * sizeof(struct wfs_dentry) + data_block_ptr);

            // Stop if we find an empty entry
            if (dentry->name[0] == '\0') {
                break;
            }

            // Get inode to check mode for file
            struct wfs_inode *entry_inode = get_inode(dentry->num);
            if (!entry_inode) {
                printf("wfs_readdir: Could not retrieve inode for entry: %s\n", dentry->name);
                continue;
            }

            // Prepare stat structure for the entry
            struct stat entry_stat;
            memset(&entry_stat, 0, sizeof(struct stat));
            entry_stat.st_mode = entry_inode->mode;

            // Add entry to buffer
            if (entry_to_buffer(output_buffer, dentry->name, &entry_stat, 0) != 0) {
                printf("wfs_readdir: Buffer full, stopping directory reading\n");
                return SUCCESS;
            }
        }
    }
    return SUCCESS;
}

void sort_disks_for_raid0(int num_disks) {
    struct wfs_sb *sb = get_superblock();

    // If not in RAID 0 mode, no sorting needed
    if (sb->raid_mode != 0) {
        return;
    }

    // // Print initial order
    // printf("Initial Disk Order:\n");
    // for (int i = 0; i < num_disks; i++) {
    //     printf("Disk %d: %s\n", i, disk_names[i]);
    // }

    // Create temp arrays to store sorted information
    char *sorted_disk_names[MAX_DISKS];
    size_t sorted_disk_sizes[MAX_DISKS];
    void *sorted_mmregion[MAX_DISKS];

    // Sort based on original disk order
    for (int target_id = 0; target_id < num_disks; target_id++) {
        int found = 0;
        // Find the disk with the matching original disk ID
        for (int j = 0; j < num_disks; j++) {
            // Open the current disk to read its superblock
            int fd = open(disk_names[j], O_RDONLY);
            if (fd == -1) {
                printf("Failed to open disk for ID check");
                continue;
            }

            // Read the superblock to get disk ID
            struct wfs_sb current_sb;
            if (read(fd, &current_sb, sizeof(struct wfs_sb)) != sizeof(struct wfs_sb)) {
                close(fd);
                printf("Failed to read superblock");
                continue;
            }
            close(fd);

            // // Print disk IDs
            // printf("Checking disk %s with disk_id %d, looking for target_id %d\n",
            //        disk_names[j], current_sb.disk_id, target_id);

            // Check if this disk matches the target ID
            if (current_sb.disk_id == target_id) {
                // Store in sorted position
                sorted_disk_names[target_id] = disk_names[j];
                sorted_disk_sizes[target_id] = disk_sizes[j];
                sorted_mmregion[target_id] = disk_region[j];
                found = 1;
                break;
            }
        }

        // DEBUG: Check if we found the disk
        if (!found) {
            printf("Could not find disk with target_id %d\n", target_id);
            return;
        }
    }

    // Copy back to original arrays
    for (int i = 0; i < num_disks; i++) {
        disk_names[i] = sorted_disk_names[i];
        disk_sizes[i] = sorted_disk_sizes[i];
        disk_region[i] = sorted_mmregion[i];
    }

    // // Print final order
    // printf("Final Disk Order:\n");
    // for (int i = 0; i < num_disks; i++) {
    //     printf("Disk %d: %s\n", i, disk_names[i]);
    // }
}


///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////// FUSE CALLBACK FUNCTIONS /////////////////////////////////////////////////////////////////////////////////////
int wfs_getattr(const char *path, struct stat *stbuf) {

    printf("wfs_getattr: Getting attributes for path: %s\n", path);
    memset(stbuf, 0, sizeof(struct stat)); // Clear the stat structure

    // Find the inode corresponding to the path
    struct wfs_inode *inode = find_inode_by_path(path);
    if (inode == NULL) {
        printf("wfs_getattr: File not found\n");
        return -ENOENT; 
    }

    // Populate the stat structure from the inode
    stbuf->st_uid = inode->uid;         
    stbuf->st_gid = inode->gid;         
    stbuf->st_atime = inode->atim;    
    stbuf->st_mtime = inode->mtim;       
    stbuf->st_mode = inode->mode;        
    stbuf->st_size = inode->size;        

    return SUCCESS;
}


int wfs_mkdir(const char *path, mode_t mode) {
    printf("wfs_mkdir: Attempting to create directory at path: %s\n", path);

    // Check the path is valid or not
    if (path == NULL || path[0] != '/') {
        printf("wfs_mkdir: Invalid path argument: %s\n", path ? path : "NULL");
        return FAIL; 
    }

    // If root direcotry, cannot create it since it already exists
    if (strcmp(path, "/") == 0) {
        printf("wfs_mkdir: Attempt to create root directory denied\n");
        return -EEXIST; 
    }

    
    char *cpy_path = strdup(path);
    char *cpy_path_2 = strdup(path);
    if (!cpy_path) {
        printf("wfs_mkdir: Memory allocation failed for cpy_path\n");
        return FAIL; 
    }
     if (!cpy_path_2) {
        printf("wfs_mkdir: Memory allocation failed for cpy_path_2\n");
        return FAIL; 
    }
    char *parent_path = dirname(cpy_path_2);   // Parent directory is everything before the last '/'

    // Find the last '/' character to separate the parent path and directory name
    char *last_slash = strrchr(cpy_path, '/');
 
    // Null-terminate the parent path at the last slash --> get the dir name from that
    *last_slash = '\0';  
    char *dir_name = last_slash + 1; 
    printf("wfs_mkdir: Parent path resolved: %s\n", parent_path);
    printf("wfs_mkdir: Directory name resolved: %s\n", dir_name);

    // Valudate length of name
    if (strlen(dir_name) >= MAX_NAME) {
        free(cpy_path);
        free(cpy_path_2);
        printf("wfs_mkdir: Name too long\n");
        return FAIL; // Name too long
    }

    struct wfs_inode *parent_inode = find_inode_by_path(parent_path);
    if (!parent_inode) {
        printf("wfs_mkdir: Parent directory not found: %s\n", parent_path);
        free(cpy_path);
        free(cpy_path_2);
        return -ENOENT; // Parent does not exist
    }

    // Check if parent is a directory
    if (!S_ISDIR(parent_inode->mode)) {
        printf("wfs_mkdir: Parent inode is not a directory\n");
        free(cpy_path);
        free(cpy_path_2);
        return FAIL; 
    }

    // Check if the directory already exists
    struct wfs_dentry *existing_entry = find_dentry_in_directory(parent_inode, dir_name);
    if (existing_entry) {
        printf("wfs_mkdir: Directory already exists with name: %s\n", dir_name);
        free(cpy_path);
        free(cpy_path_2);
        return -EEXIST; 
    }


    int new_inode_num = allocate_free_inode(); // Allocate from inode bitmap
    if (new_inode_num < 0) {
        printf("wfs_mkdir: No free inodes available\n");
        free(cpy_path);
        free(cpy_path_2);
        return -ENOSPC; 
    }
    printf("wfs_mkdir: Allocated new inode\n");

    struct wfs_inode *new_inode = get_inode(new_inode_num);
    if (!new_inode) {
        printf("wfs_mkdir: Failed to retrieve allocated inode\n");
        free(cpy_path);
        free(cpy_path_2);
        return FAIL; 
    }
    printf("wfs_mkdir: Retrieved new inode structure\n");

    // Initialize the inode
    memset(new_inode, 0, sizeof(struct wfs_inode));
    new_inode->num = new_inode_num;
    new_inode->mode = S_IFDIR | mode; 
    new_inode->uid = getuid();
    new_inode->gid = getgid();
    new_inode->size = 0; 
    new_inode->nlinks = 2; // Self (.) and parent (..)
    new_inode->atim = new_inode->mtim = new_inode->ctim = time(NULL);

    printf("wfs_mkdir: Initialized new inode: %d\n", new_inode_num);

    if (raid_mode == 1) {
        printf("\nRAID 1\n");
        // Adding a new entry in the parent directory
        struct wfs_dentry new_entry;
        strncpy(new_entry.name, dir_name, MAX_NAME);
        new_entry.num = new_inode_num;

        if (add_dentry_to_directory(parent_inode, &new_entry, dir_name, new_inode_num) < 0) {
            printf("wfs_mkdir: Failed to add new directory entry to parent\n");
            free_inode(new_inode_num);
            free(cpy_path);
            free(cpy_path_2);
            return -ENOSPC; // No space in directory entries
        }
        printf("wfs_mkdir: Added new directory entry to parent inode\n");

        // Update parent directory's metadata
        parent_inode->nlinks++;
        parent_inode->mtim = time(NULL);
        printf("wfs_mkdir: Updated parent inode metadata\n");

        if (num_disks > 1) {
            sync_disks_for_raid1(0);
        }
    }
    else { // RAID MODE 0
        if (num_disks > 1) {
            printf("\nRAID 0\n");

            sync_disks_for_raid0(0);
           
            struct wfs_dentry new_entry;
            strncpy(new_entry.name, dir_name, MAX_NAME);
            new_entry.num = new_inode_num;

            // Add the new dentry in the parent direcotry
            if (add_dentry_to_directory(parent_inode, &new_entry, dir_name, new_inode_num) < 0) {
                printf("wfs_mkdir: Failed to add new directory entry to parent\n");
                free_inode(new_inode_num);
                free(cpy_path);
                free(cpy_path_2);
                return -ENOSPC; 
            }
            printf("wfs_mkdir: Added new directory entry to parent inode\n");

            // Update parent directory's metadata
            parent_inode->nlinks++;
            parent_inode->mtim = time(NULL);
        }
    }

    free(cpy_path);
    free(cpy_path_2);
    printf("wfs_mkdir: Successfully created directory: %s\n", path);
    return SUCCESS; 
}


// FUSE callback function for mknod (creating special or regular files)
int wfs_mknod(const char *path, mode_t mode, dev_t rdev) {
    printf("wfs_mknod: Attempting to create file at path: %s\n", path);

    // Check path
    if (path == NULL || path[0] != '/') {
        printf("wfs_mknod: Invalid path argument: %s\n", path ? path : "NULL");
        return FAIL; 
    }

    // If root directory, cannot create it since it already exists
    if (strcmp(path, "/") == 0) {
        printf("wfs_mknod: Attempt to create root directory denied\n");
        return -EEXIST; 
    }

    char *path_copy = strdup(path);
    char *path_copy2 = strdup(path);
    if (!path_copy) {
        printf("wfs_mknod: Memory allocation failed for path_copy\n");
        return FAIL; 
    }
    char *parent_path = dirname(path_copy2);   // Parent directory is everything before the last '/'

    char *last_slash = strrchr(path_copy, '/');
    *last_slash = '\0';  // Null-terminate the parent path at the last slash
    char *file_name = last_slash + 1; // File name is everything after the last '/'

    printf("wfs_mknod: Parent path resolved: %s\n", parent_path);
    printf("wfs_mknod: File name resolved: %s\n", file_name);

    if (strlen(file_name) >= MAX_NAME) {
        printf("wfs_mknod: File name '%s' is too long (max: %d)\n", file_name, MAX_NAME);
        free(path_copy);
        free(path_copy2);
        return FAIL; 
    }

    struct wfs_inode *parent_inode = find_inode_by_path(parent_path);
    if (!parent_inode) {
        printf("wfs_mknod: Parent directory not found: %s\n", parent_path);
        free(path_copy);
        free(path_copy2);
        return -ENOENT; 
    }

    // Check if parent is a directory
    if (!S_ISDIR(parent_inode->mode)) {
        printf("wfs_mknod: Parent inode is not a directory\n");
        free(path_copy);
        free(path_copy2);
        return FAIL; 
    }

    // Check if the file already exists
    struct wfs_dentry *existing_entry = find_dentry_in_directory(parent_inode, file_name);
    if (existing_entry) {
        printf("wfs_mknod: File already exists with name: %s\n", file_name);
        free(path_copy);
        free(path_copy2);
        return -EEXIST; 
    }

    // Allocate a new inode for the file
    int new_inode_num = allocate_free_inode(); // Allocate from inode bitmap
    if (new_inode_num < 0) {
        printf("wfs_mknod: No free inodes available\n");
        free(path_copy);
        free(path_copy2);
        return -ENOSPC; 
    }
    printf("wfs_mknod: Allocated new inode");

    struct wfs_inode *new_inode = get_inode(new_inode_num);
    if (!new_inode) {
        printf("wfs_mknod: Failed to retrieve allocated inode\n");
        free(path_copy);
        free(path_copy2);
        return -FAIL; 
    }

    // Initialize the inode
    memset(new_inode, 0, sizeof(struct wfs_inode));
    new_inode->num = new_inode_num;
    new_inode->mode = S_IFREG | mode; 
    new_inode->uid = getuid();
    new_inode->gid = getgid();
    new_inode->size = 0; 
    new_inode->nlinks = 1; // Regular files start with 1 link
    new_inode->atim = new_inode->mtim = new_inode->ctim = time(NULL);

    printf("wfs_mknod: Initialized new inode: %d\n", new_inode_num);
    if (raid_mode == 1) {
        printf("\nwfs_mknod: RAID 1\n");

        // Add a new entry in the parent directory
        struct wfs_dentry new_entry;
        strncpy(new_entry.name, file_name, MAX_NAME);
        new_entry.num = new_inode_num;

        if (add_dentry_to_directory(parent_inode, &new_entry, file_name, new_inode_num) < 0) {
            printf("wfs_mknod: Failed to add new file entry to parent\n");
            free_inode(new_inode_num); 
            free(path_copy);
            free(path_copy2);
            return -ENOSPC; 
        }
        printf("wfs_mknod: Added new file entry to parent inode\n");

        // Update parent directory's metadata
        parent_inode->nlinks++;
        parent_inode->mtim = time(NULL);

        printf("wfs_mknod: Successfully created file: %s\n", path);
   
   
        if (num_disks > 1) {
            sync_disks_for_raid1(0);
            printf("Synchronized directory creation across all disks\n");
        }
    } else {
        if (num_disks > 1) {
            printf("\nwfs_mknod: RAID 0\n");
            sync_disks_for_raid0(0);

             // Add a new entry in the parent directory
            struct wfs_dentry new_entry;
            strncpy(new_entry.name, file_name, MAX_NAME);
            new_entry.num = new_inode_num;

            if (add_dentry_to_directory(parent_inode, &new_entry, file_name, new_inode_num) < 0) {
                printf("wfs_mknod: Failed to add new file entry to parent\n");
                free_inode(new_inode_num);
                free(path_copy);
                free(path_copy2);
                return -ENOSPC; 
            }
            printf("wfs_mknod: Added new file entry to parent inode\n");

            // Update parent directory's metadata
            parent_inode->nlinks++;
            parent_inode->mtim = time(NULL);

            printf("wfs_mknod: Successfully created file: %s\n", path);
       
        }
    }

    free(path_copy);
    free(path_copy2);
    return SUCCESS;
}

int wfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    printf("wfs_write: Writing %zu bytes to file at path: %s, offset: %jd\n", size, path, (intmax_t)offset);

    // Check path
    if (path == NULL || path[0] != '/') {
        printf("wfs_mknod: Invalid path argument: %s\n", path ? path : "NULL");
        return FAIL; 
    }

    //Find inode for file
    struct wfs_inode *inode = find_inode_by_path(path);
    if (!inode) {
        printf("wfs_write: File not found: %s\n", path);
        return -ENOENT;
    }

    // Check if file is regular
    if (!S_ISREG(inode->mode)) {
        printf("wfs_write: Path is not a regular file: %s\n", path);
        return FAIL;
    }
   
    size_t total_bytes_written = 0;
    size_t remaining_bytes = size;
    off_t current_offset = offset;
    char *write_ptr = (char *)buf;

    // Calc max number of direct blocks (last index is indirect block)
    const int max_blks_indir = BLOCK_SIZE / sizeof(off_t);
    const int max_dir_blocks = N_BLOCKS - 1;
    off_t max_file_size = (max_dir_blocks + max_blks_indir) * BLOCK_SIZE;

    if (offset > max_file_size) {
        printf("wfs_write: Offset is beyond maximum file size: %jd (max: %jd)\n", (intmax_t)offset, (intmax_t)max_file_size);
        return FAIL;
    }

    if (raid_mode == 0 && num_disks > 1) {
        //Sync metadata
        sync_disks_for_raid0(0);
        printf("wfs_write: Synchronized metadata across all disks for RAID 0\n");
    }

    // Write data block by block
    while (remaining_bytes > 0) {
        printf("------------------------ WRITING AGAIN: Remainig left is %zu-------------------------\n", remaining_bytes); 
        // Calc block number and offset within block
        int block_offset = current_offset % BLOCK_SIZE;
        int block_index = current_offset / BLOCK_SIZE;

        printf("wfs_write: block_index -- %d    block offset -- %d\n", block_index, block_offset);

        // Check if we need to use indirect block
        off_t block_ptr = 0;
        int disk = 0;
    
        if (max_dir_blocks <= block_index) {

            // Check and allocate indirect block if none exist
            if (inode->blocks[max_dir_blocks] == 0) {
                printf("wfs_write: indirect block\n");
                inode->blocks[max_dir_blocks] = allocate_free_data_block(0);
               
                if (inode->blocks[max_dir_blocks] == 0) {
                    printf("wfs_write: Failed to allocate indirect block\n");
                    return -ENOSPC;
                }
                // Clear out new indirect block
                memset(DISK_MAP_PTR(0, inode->blocks[max_dir_blocks]), 0, BLOCK_SIZE);
            }
            // Get pointer to indirect block
            off_t *indir_block_ptrs = (off_t *)DISK_MAP_PTR(0, inode->blocks[max_dir_blocks]);
            // Update block index for indirect blocks
            int indir_blck_index = block_index - max_dir_blocks;
           
            // Check if we've exceeded maximum file size
            if (indir_blck_index >= max_blks_indir) {
                printf("wfs_write: File exceeds maximum supported size\n");
                return FAIL;
            }

            // Allocate data block for indirect block
            if (indir_block_ptrs[indir_blck_index] == 0) {
                if (raid_mode == 0) {
                    // Find disk based on block's global position
                    disk = (max_dir_blocks + indir_blck_index) % num_disks;
                    indir_block_ptrs[indir_blck_index] = allocate_free_data_block(disk);

                } else {
                    // For raid 1/1v: always allocate on first disk
                    indir_block_ptrs[indir_blck_index] = allocate_free_data_block(0);
                }

                if (indir_block_ptrs[indir_blck_index] == 0) {
                    printf("wfs_write: No free data blocks available for indirect block\n");
                    return -ENOSPC;
                }

            } else {
                // Find disk based on raid mode
                if (raid_mode == 0) {
                    disk = (max_dir_blocks + indir_blck_index) % num_disks;
                }
            }

            // Get block pointer for writing
            block_ptr = indir_block_ptrs[indir_blck_index];
            printf("WFS_WRITE DEBUG -- we go to disk %d and index %d in the disk\nindirect_block_ptrs[indirect_block_index]: %ld\n",disk, indir_blck_index, indir_block_ptrs[indir_blck_index]);

        } else {
            // Direct block handling
            printf("wfs_write: direct block\n");

            if (inode->blocks[block_index] == 0) {

                if (raid_mode == 0) {
                    // Check disk based on the block's global position
                    disk = block_index % num_disks;
                    inode->blocks[block_index] = allocate_free_data_block(disk);

                } else {
                    // RAID 1 or 1v: Always allocate on first disk
                    inode->blocks[block_index] = allocate_free_data_block(0);
                }

                if (inode->blocks[block_index] == 0) {
                    printf("wfs_write: No free data blocks available\n");
                    return -ENOSPC;
                }
                printf("wfs_write: Allocated new data block at index %d on disk %d\n", block_index, disk);
            } else {
                // Check disk based on raid mode
                if (raid_mode == 0) {
                    disk = block_index % num_disks;
                }
            }
           
            // Use direct block pointer
            block_ptr = inode->blocks[block_index];
        }

        // Check how much to write in this block
        size_t write_size = MIN(remaining_bytes, BLOCK_SIZE - block_offset);

        if (raid_mode == 0) {        
            size_t stripe_bytes_written = 0;

            while (write_size > stripe_bytes_written) {
                size_t current_stripe_offset = (current_offset + stripe_bytes_written) % BLOCK_SIZE;
                size_t stripe_remaining = BLOCK_SIZE - current_stripe_offset;
                size_t stripe_write_size = MIN(stripe_remaining, write_size - stripe_bytes_written);

                // Check target disk for this stripe
                int stripe_disk = ((current_offset + stripe_bytes_written) / BLOCK_SIZE) % num_disks;

                // Calc block pointer, updating for specific disk
                off_t stripe_block_ptr = block_ptr + (stripe_disk * BLOCK_SIZE);

                // Write to specific stripe on disk
                char *disk_block_ptr = DISK_MAP_PTR(stripe_disk, stripe_block_ptr);
                memcpy(disk_block_ptr + block_offset + current_stripe_offset,
                       write_ptr + stripe_bytes_written,
                       stripe_write_size);

                stripe_bytes_written += stripe_write_size;
            }
        } else if (raid_mode != 0) {
            // Raid 1/Raid 1v: Write to all disks
            for (int disk = 0; disk < num_disks; disk++) {
                char *disk_block_ptr = DISK_MAP_PTR(disk, block_ptr);
                memcpy(block_offset + disk_block_ptr, write_ptr, write_size);
            }
        }

        // Update pointers and counters
        write_ptr += write_size;
        current_offset += write_size;
        remaining_bytes -= write_size;
        total_bytes_written += write_size;
        printf("------------------------------------------------------------------------------------\n");
    }

    // Update inode metadata
    if (current_offset > inode->size) {
        //Update file size if wrote beyond current end
        inode->size = current_offset;
    }  
    //Update modification time
    inode->mtim = time(NULL);

    if ((raid_mode != 0) && num_disks > 1) {
        //Synch file data and metadata
        sync_disks_for_raid1(0);
        printf("wfs_write: Synchronized file write across all disks in RAID 1\n");
    }

    printf("wfs_write: Successfully wrote %zu bytes to file: %s\n", total_bytes_written, path);
    return total_bytes_written; // Return the number of bytes written
}



int wfs_readdir(const char *path, void *output_buffer, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
    printf("wfs_readdir: Reading directory entries for path: %s\n", path);

    // Check path
    if (path == NULL || path[0] != '/') {
        printf("wfs_mknod: Invalid path argument: %s\n", path ? path : "NULL");
        return FAIL; 
    }

    // Find the inode for directory
    struct wfs_inode *dir_inode = find_inode_by_path(path);
    if (!dir_inode) {
        printf("wfs_readdir: Directory not found: %s\n", path);
        return -ENOENT;
    }

    // Check if directory
    if (!S_ISDIR(dir_inode->mode)) {
        printf("wfs_readdir: Path is not a directory: %s\n", path);
        return FAIL;
    }

    // Add '.' and '..' first (current and parent directory)
    if (filler(output_buffer, ".", NULL, 0) != 0 || filler(output_buffer, "..", NULL, 0) != 0) {
        return FAIL;
    }

    // Call the helper function to process the directory blocks
    return process_directory_blocks(dir_inode, output_buffer, filler);
}

int wfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    printf("wfs_read: Attempting to read\n");
    
    // Check path
    if (path == NULL || path[0] != '/') {
        printf("wfs_mknod: Invalid path argument: %s\n", path ? path : "NULL");
        return FAIL; 
    }

    // Find the inode for the file
    struct wfs_inode *file_inode = find_inode_by_path(path);
    if (!file_inode) {
        printf("wfs_read: File not found: %s\n", path);
        return -ENOENT;
    }
    
    if (!S_ISREG(file_inode->mode)) {
        printf("wfs_read: Path is not a regular file: %s\n", path);
        return FAIL;
    }

    // Check if the read request is past the end of the file
    if (file_inode->size <= offset) {
        return 0; // No bytes to read
    }

    // Adjust the read size if it exceeds the remaining file size
    if (offset + size > file_inode->size) {
        size = file_inode->size - offset;
    }

    size_t total_bytes_read = 0;
    size_t bytes_left = size;
    off_t current_file_offset = offset;
    char *buffer_pointer = buf;

    // Read data block by block
    while (bytes_left > 0) {
        // Calculate the block index and offset within the block
        int blk_idx = current_file_offset / BLOCK_SIZE;
        int block_internal_offset = current_file_offset % BLOCK_SIZE;
        int target_disk_index = 0;
        int local_block_index = blk_idx;

        if (raid_mode == 0) {
            // RAID 0 (striping)
            target_disk_index = blk_idx % num_disks;
            local_block_index = blk_idx / num_disks;
        }

        // Handle indirect blocks
        off_t blk_addr = 0;
        if (local_block_index >= N_BLOCKS - 1) {
            // Indirect block handling
            off_t *indirect_block_addresses = (off_t *)DISK_MAP_PTR(target_disk_index, file_inode->blocks[N_BLOCKS - 1]);
      
            // Use the target disk for indirect blocks in RAID 0               
            int indirect_block_index = local_block_index - (N_BLOCKS - 1);

            if (indirect_block_index >= BLOCK_SIZE / sizeof(off_t)) {
                printf("wfs_read: Tried to read beyond file size\n");
                break;
            }

            blk_addr = indirect_block_addresses[indirect_block_index];
        } else {
            // Direct block
            blk_addr = file_inode->blocks[local_block_index];
        }

        // Check if the block is allocated
        if (blk_addr == 0) {
            printf("wfs_read: Tried to read from an unallocated block\n");
            break;
        }
        // Determine how much to read from this block
        size_t bytes_to_read = MIN(BLOCK_SIZE - block_internal_offset, bytes_left);

        // RAID 1v (majority voting)
        if (raid_mode == 2) {
            int majority_votes[num_disks];
            char *block_data[num_disks];

            // Read the block from all disks
            for (int disk = 0; disk < num_disks; disk++) {
                block_data[disk] = DISK_MAP_PTR(disk, blk_addr);
            }

            // Determine the majority block
            int majority_disk_idx = 0;
            for (int i = 0; i < num_disks; i++) {
                majority_votes[i] = 1;
                for (int j = i + 1; j < num_disks; j++) {
                    if (block_data[i] && block_data[j] &&
                        memcmp(block_data[i], block_data[j], BLOCK_SIZE) == 0) {
                        majority_votes[i]++;
                    }
                }
                if (majority_votes[i] > majority_votes[majority_disk_idx] ||
                    (majority_votes[i] == majority_votes[majority_disk_idx] && i < majority_disk_idx)) {
                    majority_disk_idx = i;
                }
            }

            // Use the majority block
            if (block_data[majority_disk_idx]) {
                memcpy(buffer_pointer, block_data[majority_disk_idx] + block_internal_offset, bytes_to_read);
            } else {
                printf("wfs_read: Couldn't verify block majority\n");
                return FAIL;
            }
        } else if (raid_mode == 1) {
            int successful_read = 0;
            for (int disk = 0; disk < num_disks; disk++) {
                char *block_data = DISK_MAP_PTR(disk, blk_addr);

                if (block_data) {
                    memcpy(buffer_pointer, block_data + block_internal_offset, bytes_to_read);
                    successful_read = 1;
                    break;
                }
            }

            if (!successful_read) {
                printf("wfs_read: Failed to read block from any disk\n");
                return FAIL;
            }
        } else if (raid_mode == 0) {
            char *block_data = DISK_MAP_PTR(target_disk_index, blk_addr);

            if (block_data) {
                memcpy(buffer_pointer, block_data + block_internal_offset, bytes_to_read);
            } else {
                printf("wfs_read: Failed to read block in RAID 0\n");
                return FAIL;
            }
        }

        buffer_pointer += bytes_to_read;
        current_file_offset += bytes_to_read;
        bytes_left -= bytes_to_read;
        total_bytes_read += bytes_to_read;
    }

    file_inode->atim = time(NULL);

    printf("wfs_read: Read %zu bytes from file: %s\n", total_bytes_read, path);
    return total_bytes_read;
}


int wfs_unlink(const char *path) {
    printf("wfs_unlink: trying to unlink file: %s\n", path);
    // Check if path is valid
    if (path == NULL || path[0] != '/') {
        return FAIL;
    }

    // Create copies of path and extract parent path and file name
    char *original_path = strdup(path);
    char *duplicate_path = strdup(path);

    if (!original_path || !duplicate_path) {
        printf("wfs_unlink: mem alloc failed for file path\n");
        free(original_path);
        free(duplicate_path);
        return FAIL;
    }
    char *parent = dirname(duplicate_path);
    printf("wfs_unlink: parent: %s\n", parent);

    char *slash = strrchr(original_path, '/');
    *slash = '\0';

    char *target_file = slash + 1;
    printf("wfs_unlink: file name: %s\n", target_file);

    // Find parent inode and check if directory
    struct wfs_inode *parent_inode = find_inode_by_path(parent);
    if (!parent_inode || !S_ISDIR(parent_inode->mode)) {
        printf("wfs_unlink: parent directory not found or not directory: %s\n", parent);
        free(original_path);
        free(duplicate_path);
        return -ENOENT;
    }


    // Find directory entry
    struct wfs_dentry *target_dentry = find_dentry_in_directory(parent_inode, target_file);
    if (!target_dentry) {
        printf("wfs_unlink: file not found: %s\n", target_file);
        free(original_path);
        free(duplicate_path);
        return -ENOENT;
    }

    // Find file's inode
    struct wfs_inode *target_inode = get_inode(target_dentry->num);
    if (!target_inode || !S_ISREG(target_inode->mode)) {
        printf("wfs_unlink: couldn't get inode for file or can't unlink file that's not regular\n");
        free(original_path);
        free(duplicate_path);
        return FAIL;
    }

    printf("wfs_unlink: calling unlink_file_helper\n");
    // Call the helper function to unlink the file
    int result = unlink_file_helper(parent_inode, target_inode, target_file);
   
    // If unlink operation failed, return failure
    if (result != SUCCESS) {
        free(original_path);
        free(duplicate_path);
        return result;
    }


    // Update parent metadata
    parent_inode->nlinks--;
    parent_inode->mtim = time(NULL);

    if (raid_mode == 1 && num_disks > 1) {
        sync_disks_for_raid1(0);
        printf("synced file unlink across disks\n");
    } else if (raid_mode == 0 && num_disks > 1) {
        sync_disks_for_raid0(0);
        printf("synced metadata across disks for raid 0\n");
    }

    free(original_path);
    free(duplicate_path);

    return SUCCESS;
}

int wfs_rmdir(const char *directory_path) {
    printf("wfs_rmdir: trying to remove directory: %s\n", directory_path);

    // Check input
    if (directory_path == NULL || directory_path[0] != '/') {
        return FAIL;
    }

    // Root directory cannot be removed
    if (strcmp(directory_path, "/") == 0) {
        printf("wfs_rmdir: can't remove root directory\n");
        return FAIL;
    }

    // Create copies of the path and extract parent path and directory name
    char *original_path = strdup(directory_path);
    char *duplicate_path = strdup(directory_path);
    if (!original_path || !duplicate_path) {
        printf("wfs_rmdir: mem alloc failed for directory path\n");
        free(original_path);
        free(duplicate_path);
        return FAIL;
    }

    char *parent = dirname(duplicate_path);
    printf("wfs_rmdir: parent: %s\n", parent);

    char *slash = strrchr(original_path, '/');
    *slash = '\0';

    char *target_dir = slash + 1;
    printf("wfs_rmdir: directory name: %s\n", target_dir);

    // Find parent inode and check if directory
    struct wfs_inode *parent_inode = find_inode_by_path(parent);
    if (!parent_inode || !S_ISDIR(parent_inode->mode)) {
        printf("wfs_rmdir: parent directory not found or not directory: %s\n", parent);
        free(original_path);
        free(duplicate_path);
        return -ENOENT;
    }

    // Find directory entry
    struct wfs_dentry *target_dentry = find_dentry_in_directory(parent_inode, target_dir);
    if (!target_dentry) {
        printf("wfs_rmdir: directory not found: %s\n", target_dir);
        free(original_path);
        free(duplicate_path);
        return -ENOENT;
    }

    // Find directory's inode
    struct wfs_inode *target_inode = get_inode(target_dentry->num);
    if (!target_inode || !S_ISDIR(target_inode->mode)) {
        printf("wfs_rmdir: couldn't get inode for directory or not a directory\n");
        free(original_path);
        free(duplicate_path);
        return FAIL;
    }

    int result = remove_directory_helper(parent_inode, target_inode, target_dir);

    free(original_path);
    free(duplicate_path);
    return result;
}


/*
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
*/




static struct fuse_operations ops = {
    .getattr = wfs_getattr,
    .mkdir = wfs_mkdir,
    .mknod = wfs_mknod,// Add other functions (read, write, mkdir, etc.) here as needed
    .write = wfs_write,
    .readdir = wfs_readdir,
    .read = wfs_read,
    .unlink = wfs_unlink,
    .rmdir = wfs_rmdir,
};


int main(int argc, char *argv[]) {
    num_disks = 0;

    // Validate the argv and argc
    while (num_disks + 1 < argc && argv[num_disks + 1][0] != '-') {
        num_disks++;
    }

    if (num_disks == 0) {
        printf("No disk paths provided\n");
        return FAIL;
    }

    if (num_disks > MAX_DISKS) {
        printf("Too many disks provided. Max supported: %d\n", MAX_DISKS);
        return FAIL;
    }

    // Memory-map each disk
    for (int i = 0; i < num_disks; i++) {
        disk_names[i] = argv[i + 1]; // Store disk name

        // Open the disk file
        int fd = open(disk_names[i], O_RDWR);
        if (fd == -1) {
            printf("Failed to open disk");
            return FAIL;
        }

        // Get the disk file size
        struct stat st;
        if (fstat(fd, &st) == -1) {
            printf("Failed to stat disk");
            close(fd);
            return FAIL;
        }
        disk_sizes[i] = st.st_size;

        // Memory-map the disk
        disk_region[i] = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (disk_region[i] == MAP_FAILED) {
            printf("Failed to mmap disk");
            close(fd);
            return FAIL;
        }

        close(fd); 
    }
   
    // Initalize raid mode
    struct wfs_sb *sb = get_superblock();
    raid_mode = sb->raid_mode;

    if(raid_mode == 0){
        sort_disks_for_raid0(num_disks);
    }

    // FUSE arguments
    int fuse_argc = argc - num_disks; // Include the program name "./wfs"
    char **fuse_argv = calloc(fuse_argc, sizeof(char *));
    if (!fuse_argv) {
        printf("Failed to allocate memory for FUSE arguments");
        return FAIL;
    }

    // Populate the FUSE arguments array
    fuse_argv[0] = argv[0]; // Add the program name "./wfs"
    for (int i = 1; i < fuse_argc; i++) {
        fuse_argv[i] = argv[num_disks + i];
    }

    int ret = fuse_main(fuse_argc, fuse_argv, &ops, NULL);

    for (int i = 0; i < num_disks; i++) {
        if (disk_region[i] != NULL) {
            munmap(disk_region[i], disk_sizes[i]);
        }
    }
    free(fuse_argv);
    return ret;
}
