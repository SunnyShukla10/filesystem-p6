# Filesystem Project (FUSE + RAID)

## Overview
This project implements a custom filesystem using FUSE (Filesystem in Userspace) with integrated RAID (Redundant Array of Independent Disks) capabilities. The filesystem supports RAID 0 (striping) and RAID 1 (mirroring), handling basic operations like file creation, deletion, reading, and writing.

The layout of a disk is shown below.

![filesystem layout on disk](disk-layout.svg)

## Features
- Create and remove files/directories.
- Read/write file contents with support for large files via indirect blocks.
- RAID 0 and RAID 1 functionality.
- Basic attributes (`st_uid`, `st_gid`, `st_atime`, `st_mtime`, `st_mode`, `st_size`) filled for files and directories.

## Setup and Usage

### 1. Compile the Code
```bash
make
```
### 2. Initialize the Disks
```bash
truncate -s 1M <disk_name>
```
- `1M`: 1 MB for simplicity
- `disk_name`: Your own disk pathname

### 3. Initialize the Filesystem
```bash
./mkfs -r <raid_mode> -d <disk_name1> -d <disk_name2> -i <num_inodes> -b <num_blocks>
```
- `raid_mode`: RAID type (`0` for striping, `1` for mirroring, `1v` for verified mirroring).
- `disk_name1`, `disk_name2`: Paths to disk images.
- `num_inodes`: Number of inodes.
- `num_blocks`: Number of data blocks (rounded to nearest multiple of 32).

**Example:**
```bash
./mkfs -r 1 -d <disk_name1> -d <disk_name2> -i 32 -b 224
```

### 4. Mount the Filesystem
```bash
./wfs <disk_name1> <disk_name2> [FUSE options] <mount_point>
```
- Use `-s` to disable multi-threading (mandatory).
- Use `-f` for running FUSE in the foreground (recommended for debugging).

**Example:**
```bash
./wfs disk1.img disk2.img -f -s mnt
```

### 5. Interact with the Filesystem
Once mounted, you can use standard file commands like `mkdir`, `ls`, `echo`, and `cat` to interact with the filesystem:
```bash
mkdir mnt/new_dir
ls mnt
stat mnt/new_dir
echo "Hello" > mnt/file.txt
cat mnt/file.txt
```

### 6. Unmount disk after done interacting
```bash
./umount.sh mnt
```


## Example Workflow
```bash
truncate -s 1M disk1.img
truncate -s 1M disk2.img
./mkfs -r 1 -d disk1.img -d disk2.img -i 32 -b 224
mkdir mnt
./wfs disk1.img disk2.img -f -s mnt
stat mnt
mkdir mnt/test_dir
ls mnt
echo "data" > mnt/file.txt
cat mnt/file.txt
./umount.sh mnt
```
