#!/usr/bin/python3

# usage: ./check-mkfs.py <numinodes> <numdatablocks> disks ...

import sys
import os

superblock = ['inodes', 'datablocks', 'ibit', 'dbit', 'iblocks', 'dblocks']
inode = [('num', 4), ('mode', 4), ('uid', 4), ('gid', 4), ('size', 8),
         ('nlinks', 8), ('atim', 8), ('mtim', 8), ('ctim', 8), ('blocks', 64)]
sbsize = 48
inodesize = 120
blksize = 512

def roundup(n, k):
    remain = n % k
    return n if remain == 0 else (n + (k - remain))

def read_inode(diskf, loc):    
    diskf.seek(roundup(loc, 512))
    i = diskf.read(inodesize)
    return {
        name: int.from_bytes(i[offset:offset + size], sys.byteorder)
        for offset, (name, size) in zip(
                [sum(size for _, size in inode[:i]) for i in range(len(inode))],
                inode)
    }
    
def find_allocations(ibit):
    """Return a list of all the allocated inodes in a disk."""
    return [bytep * 8 + bitp for bytep, byte in enumerate(ibit)
            for bitp in range(8) if byte & (1 << bitp)]

def test_compare(disk, teststr, found, expected):
    """Compare 'found' and 'expected', print a message and exit if not equal."""
    if (found != expected):
        print(f"{teststr} [{disk}]: found {found} expected {expected}")
        exit()

def test_nonzero(disk, teststr, found):
    """Ensure 'found' is not zero."""
    if (found == 0):
        print(f"{teststr} [{disk}]: unexpectedly zero or empty")
        exit()
    
def verify_mkfs(disk, inodes, datablocks):
    """Verify the superblock and root inode on a wfs disk."""
    with open(disk, "rb") as diskf:
        # check the superblock
        sb = diskf.read(sbsize)
        sbfields = [int.from_bytes(sb[i:i+8], sys.byteorder)
                    for i in range(0, len(sb), 8)]

        read_sb = dict(zip(superblock, sbfields))

        # correct number of inodes and datablocks?
        test_compare(disk, "inodes", read_sb['inodes'], inodes)
        test_compare(disk, "datablocks", read_sb['datablocks'], datablocks)

        # regions the right size?
        ibit_size = read_sb['dbit'] - read_sb['ibit']
        test_compare(disk, "inode bitmap size", ibit_size, inodes / 8)

        dbit_size = read_sb['iblocks'] - read_sb['dbit']
        test_compare(disk, "data bitmap size", dbit_size, datablocks / 8)
        
        iregion_size = read_sb['dblocks'] - read_sb['iblocks']
        test_compare(disk, "inode region size", iregion_size, inodes * blksize)

        # check root inode
        diskf.seek(read_sb['ibit'])
        ibit = diskf.read(ibit_size)
        allocated_inodes = find_allocations(ibit)
        test_compare(disk, "inode allocation", len(allocated_inodes), 1)

        # data bitmap should be empty
        dbit = diskf.read(dbit_size)
        allocated_datablocks = find_allocations(dbit)
        test_compare(disk, "datablock allocation", len(allocated_datablocks), 0)

        for inodep in allocated_inodes:
            inode = read_inode(diskf, read_sb['iblocks'] + (inodep * blksize))
            test_compare(disk, "inode number matches allocation", inode['num'], inodep)
            for field in ['mode', 'uid', 'gid', 'atim', 'mtim', 'ctim', 'nlinks']:
                test_nonzero(disk, field, inode[field])
            test_compare(disk, "inode size", inode['size'], 0)

def main():
    args = sys.argv[1:]
    inodes = int(args[0])
    datablocks = int(args[1])
    disks = args[2:]

    for disk in disks:
        verify_mkfs(disk, inodes, datablocks)

    print("Success")

if __name__ == '__main__':
    main()
