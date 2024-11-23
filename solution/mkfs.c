#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAIL 1
#define SUCCESS 0

int main(int argc, char*argv []) {
    int raid_mode = -1;
    char * disk_files[10];
    int num_inodes = 0;
    int num_blocks = 0;
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
            num_inodes = argv[++i];
        }
        else if (strcmp(argv[i], "-b") == 0){
            if (i+1 > argc) {
                printf("Missing value for -b\n");
                exit(FAIL);
            }
            num_blocks = argv[++i];
        }
        else {
            printf("Unknown Flag: %s\n", argv[i]);
            exit(FAIL);
        }
    }

    // Validate the arguments
    if (raid_mode == -1 || num_blocks == 0 || num_disks == 0 || num_inodes == 0) {
        printf("Missing required arguments\n");
        exit(FAIL);
    }

    // Round num_blocks to nearest multiple of 32
    if (num_blocks % 32 != 0) {
        num_blocks = (num_blocks / 32 + 1) * 32;
    }



    return SUCCESS;
}