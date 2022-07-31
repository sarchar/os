#include "common.h"

#include "kalloc.h"
#include "kernel.h"
#include "paging.h"
#include "palloc.h"
#include "stdio.h"
#include "string.h"

#include "fs/ext2/ext2.h"
//#include "ext2/superblock.h"
//#include "ext2/inode.h"

#define NUM_BLOCKS(v)  (((v) + ((1024 << ext2_data.superblock->s_log_block_size) - 1)) / (1024 << ext2_data.superblock->s_log_block_size))
#define NUM_SECTORS(v) (((v) + ext2_data.filesystem_callbacks->device_sector_size - 1) / ext2_data.filesystem_callbacks->device_sector_size)
#define BLOCK_SIZE     (1024 << ext2_data.superblock->s_log_block_size)
#define INODE_SIZE     (ext2_data.superblock->s_inode_size)

#define INODE_BLOCK_INDIRECT0 13ULL
#define INODE_BLOCK_INDIRECT1 (INODE_BLOCK_INDIRECT0 + (BLOCK_SIZE / INODE_SIZE))
#define INODE_BLOCK_INDIRECT2 (INODE_BLOCK_INDIRECT1 + ((BLOCK_SIZE * BLOCK_SIZE) / INODE_SIZE))
#define INODE_BLOCK_INDIRECT3 (INODE_BLOCK_INDIRECT2 + ((BLOCK_SIZE * BLOCK_SIZE * BLOCK_SIZE) / INODE_SIZE))

#define EXT2_SUPER_MAGIC 0xEF53

#define EXT2_ROOT_INO 2

struct ext2_superblock {
    u32 s_inodes_count;
    u32 s_blocks_count;
    u32 s_r_blocks_count;
    u32 s_free_blocks_count;
    u32 s_free_inodes_count;
    u32 s_first_data_block;
    u32 s_log_block_size;
    u32 s_log_frag_size;
    u32 s_blocks_per_group;
    u32 s_frags_per_group;
    u32 s_inodes_per_group;
    u32 s_mtime;
    u32 s_wtime;
    u16 s_mnt_count;
    u16 s_max_mnt_count;
    u16 s_magic;
    u16 s_state;
    u16 s_errors;
    u16 s_minor_rev_level;
    u32 s_lastcheck;
    u32 s_checkinterval;
    u32 s_creator_os;
    u32 s_rev_level;
    u16 s_def_resuid;
    u16 s_def_resgid;
    // EXT2_DYNAMIC_REV
    u32 s_first_ino;
    u16 s_inode_size;
    u16 s_block_group_nr;
    u32 s_feature_compat;
    u32 s_feature_incompat;
    u32 s_feature_ro_compat;
    u8  s_uuid[16];
    u8  s_volume_name[16];
    u8  s_last_mounted[64];
    u32 s_algo_bitmap;
    // performance hints
    u8  s_prealloc_blocks;
    u8  s_prealloc_dir_blocks;
    u8  padding0[2];
    // journaling
    u8  s_journal_uuid[16];
    u32 s_journal_inum;
    u32 s_journal_dev;
    u32 s_last_orphan;
    // directory indexing
    u32 s_hash_seed[4];
    u8  s_def_hash_version;
    u8  padding1[3];
    // other options
    u32 s_default_mount_options;
    u32 s_first_meta_bg;
    u8  reserved0[760];
} __packed;

struct ext2_block_group_descriptor {
    u32 bg_block_bitmap;
    u32 bg_inode_bitmap;
    u32 bg_inode_table;
    u16 bg_free_blocks_count;
    u16 bg_free_inodes_count;
    u16 bg_used_dirs_count;
    u16 bg_pad;
    u8  bg_reserved[12];
} __packed;

struct {
    struct filesystem_callbacks* filesystem_callbacks;
    struct ext2_superblock* superblock;
    struct ext2_block_group_descriptor* bg_table;
    u32    num_block_groups;
} ext2_data;

s64 ext2_read_blocks(u64 block_index, u32 block_count, intp* ret, u8* order)
{
    u32 offs = block_index * BLOCK_SIZE; // offs will always be at least device sector aligned
    u32 size = block_count * BLOCK_SIZE;

    // number of pages for allocation
    u32 num_pages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;

    // allocate memory
    u32 n = next_power_of_2(num_pages);
    *ret = palloc_claim(n);

    // read sector is truncated down from offs
    u32 sector = offs / ext2_data.filesystem_callbacks->device_sector_size; 

    // read sectors is rounded up
    if(!ext2_data.filesystem_callbacks->read_sectors(ext2_data.filesystem_callbacks, sector, NUM_SECTORS(size), *ret)) {
        palloc_abandon(*ret, n);
        return -1;
    }

    // save order so the caller can free the allocated memory
    if(order != null) *order = n;

    return 0;
}

s64 ext2_read_inode(u64 inode_number, struct ext2_inode** ret)
{
    //fprintf(stderr, "ext2: ext2_read_inode(inode_number=%d, **ret=0x%lX)\n", inode_number, ret);

    // locate the inode block group
    intp block_group = (inode_number - 1) / ext2_data.superblock->s_inodes_per_group;
    intp block_inode = (inode_number - 1) % ext2_data.superblock->s_inodes_per_group;
    //fprintf(stderr, "ext2: inode %d has block_group=%d block_inode=%d\n", inode_number, block_group, block_inode);

    // locate the inode table
    intp inode_table = ext2_data.bg_table[block_group].bg_inode_table;
    //fprintf(stderr, "ext2: inode %d has inode_table at block %d\n", inode_number, inode_table);

    // need to read the block where the the inode data is
    // TODO might be a nice optimization to just read the *sector* where the data is. inodes are 128 bytes.
    intp table_offset_byte = INODE_SIZE * block_inode;
    intp table_offset_block = table_offset_byte / BLOCK_SIZE;
    //fprintf(stderr, "ext2: inode %d has table_offset_byte=%d table_offset_block=%d\n", inode_number, table_offset_byte, table_offset_block);

    // read the block that has the inode
    intp table_data; 
    u8 order;
    //fprintf(stderr, "ext2: inode %d reading block %d\n", inode_number, inode_table + table_offset_block);
    if(ext2_read_blocks(inode_table + table_offset_block, 1, &table_data, &order) < 0) return -1;

    // allocate space for the inode
    *ret = (struct ext2_inode*)kalloc(INODE_SIZE);

    // copy the inode
    //fprintf(stderr, "ext2: inode %d offset into inode table block %d\n", inode_number, table_offset_byte + (table_offset_block * BLOCK_SIZE));
    memcpy(*ret, (void*)(table_data + table_offset_byte - (table_offset_block * BLOCK_SIZE)), INODE_SIZE);

    // free the storage allocated for the table data
    palloc_abandon(table_data, order);

    return 0;
}

void ext2_free_inode(struct ext2_inode* inode)
{
    kfree(inode);
}

u64 ext2_block_size()
{
    return BLOCK_SIZE;
}

// block_offset is relative to the start of inode data
// blocks are <= page size, so order of allocated memory is always 0
s64 ext2_read_inode_block(struct ext2_inode* inode, u64 block_offset, intp* ret)
{
    if(block_offset < INODE_BLOCK_INDIRECT0) {
        if(ext2_read_blocks(inode->i_block[block_offset], 1, ret, null) < 0) return -1;
    } else if(block_offset < INODE_BLOCK_INDIRECT1) {
        assert(false, "TODO read nested blocks");
        return -1;
    } else if(block_offset < INODE_BLOCK_INDIRECT2) {
        assert(false, "TODO read doubly nested blocks");
        return -1;
    } else if(block_offset < INODE_BLOCK_INDIRECT3) {
        assert(false, "TODO read trebly nested blocks");
        return -1;
    } else {
        fprintf(stderr, "ext2: invalid block index %d\n", block_offset);
        return -1;
    }

    return 0;
}

s64 ext2_read_superblock()
{
    intp dest = palloc_claim_one();
    u32 read_size = (sizeof(struct ext2_superblock) + ext2_data.filesystem_callbacks->device_sector_size - 1) / ext2_data.filesystem_callbacks->device_sector_size;

    u32 sector = 1024 / ext2_data.filesystem_callbacks->device_sector_size;
    if(!ext2_data.filesystem_callbacks->read_sectors(ext2_data.filesystem_callbacks, sector, read_size, dest)) {
        palloc_abandon(dest, 0);
        return -1;
    }

    struct ext2_superblock* sb = (struct ext2_superblock*)dest;
    ext2_data.superblock = sb;

    fprintf(stderr, "ext2: superblock: s_rev_level=%d\n", sb->s_rev_level);
    fprintf(stderr, "ext2: superblock: s_minor_rev_level=%d\n", sb->s_minor_rev_level);
    fprintf(stderr, "ext2: superblock: s_blocks_count=%d\n", sb->s_blocks_count);
    fprintf(stderr, "ext2: superblock: s_r_blocks_count=%d\n", sb->s_r_blocks_count);
    fprintf(stderr, "ext2: superblock: s_free_inodes_count=%d\n", sb->s_free_inodes_count);
    fprintf(stderr, "ext2: superblock: s_free_blocks_count=%d\n", sb->s_free_blocks_count);
    fprintf(stderr, "ext2: superblock: s_log_block_size=%d (%d KiB)\n", sb->s_log_block_size, BLOCK_SIZE/1024);
    fprintf(stderr, "ext2: superblock: s_log_frag_size=%d (%d KiB)\n", sb->s_log_frag_size, (1024 << sb->s_log_frag_size)/1024);
    fprintf(stderr, "ext2: superblock: s_blocks_per_group=%d\n", sb->s_blocks_per_group);
    fprintf(stderr, "ext2: superblock: s_inodes_per_group=%d\n", sb->s_inodes_per_group);
    fprintf(stderr, "ext2: superblock: s_first_data_block=%d\n", sb->s_first_data_block);
    fprintf(stderr, "ext2: superblock: s_inodes_count=%d\n", sb->s_inodes_count);
    fprintf(stderr, "ext2: superblock: s_inode_size=%d\n", sb->s_inode_size);
    fprintf(stderr, "ext2: superblock: s_creator_os=%d\n", sb->s_creator_os);
    fprintf(stderr, "ext2: superblock: s_feature_compat=0x%02X\n", sb->s_feature_compat);
    fprintf(stderr, "ext2: superblock: s_feature_incompat=0x%02X\n", sb->s_feature_incompat);
    fprintf(stderr, "ext2: superblock: s_feature_ro_compat=0x%02X\n", sb->s_feature_ro_compat);
    fprintf(stderr, "ext2: superblock: s_volume_name=%s\n", sb->s_volume_name);
    fprintf(stderr, "ext2: superblock: s_algo_bitmap=0x%02X\n", sb->s_algo_bitmap);
    fprintf(stderr, "ext2: superblock: s_first_ino=0x%02X\n", sb->s_first_ino);

    fprintf(stderr, "ext2: superblock magic = 0x%04X\n", sb->s_magic);
    fprintf(stderr, "ext2: filesystem size = %llu MiB\n", ((u64)sb->s_blocks_count * BLOCK_SIZE) / (1024*1024));
    fprintf(stderr, "ext2: free space size = %llu MiB\n", ((u64)sb->s_free_blocks_count * BLOCK_SIZE) / (1024*1024));
    fprintf(stderr, "ext2: num block groups = %d\n", (sb->s_blocks_count + (sb->s_blocks_per_group-1)) / sb->s_blocks_per_group);

    assert(__alignof(BLOCK_SIZE, ext2_data.filesystem_callbacks->device_sector_size) == 0, "block sizes must be a multiple of sector size");
    assert(sb->s_log_block_size == sb->s_log_frag_size, "fragment size must be equal to blcok size");

    // cache some commonly used values
    ext2_data.num_block_groups = (ext2_data.superblock->s_blocks_count + (ext2_data.superblock->s_blocks_per_group-1)) / ext2_data.superblock->s_blocks_per_group;

    return 0;
}

s64 ext2_open(struct filesystem_callbacks* fscbs)
{
    ext2_data.filesystem_callbacks = fscbs;
    
    if(ext2_read_superblock() < 0) return -1;
    u64 sb_block_index = ext2_data.superblock->s_first_data_block;
    assert(sb_block_index == (1024ULL / BLOCK_SIZE), "s_first_data_block incorrect");

    u32 read_block_count = NUM_BLOCKS(ext2_data.num_block_groups / sizeof(struct ext2_block_group_descriptor)); // number of blocks required to store all of the block group descriptors
    if(ext2_read_blocks(sb_block_index + 1, read_block_count, (intp*)&ext2_data.bg_table, null) < 0) return -1;

    struct ext2_block_group_descriptor* bg0 = &ext2_data.bg_table[32];
    fprintf(stderr, "ext2: bg0: bg_block_bitmap=%d\n", bg0->bg_block_bitmap);
    fprintf(stderr, "ext2: bg0: bg_inode_bitmap=%d\n", bg0->bg_inode_bitmap);
    fprintf(stderr, "ext2: bg0: bg_inode_table=%d\n", bg0->bg_inode_table);
    fprintf(stderr, "ext2: bg0: bg_free_blocks_count=%d\n", bg0->bg_free_blocks_count);
    fprintf(stderr, "ext2: bg0: bg_free_inodes_count=%d\n", bg0->bg_free_inodes_count);
    fprintf(stderr, "ext2: bg0: bg_used_dirs_count=%d\n", bg0->bg_used_dirs_count);

//    struct ext2_inode* root_directory;
//    if(ext2_read_inode(EXT2_ROOT_INO, &root_directory) < 0) return -1;
//
//    fprintf(stderr, "ext2: inode: i_mode=0x%04X\n", root_directory->i_mode);
//
//    if(EXT2_ISDIR(root_directory)) {
//        intp dirdata;
//        if(ext2_read_inode_block(root_directory, 0, &dirdata) < 0) return -1;
//
//        intp offset = dirdata;
//        while(offset < (dirdata + root_directory->i_size)) {
//            struct ext2_dirent* dirent = (struct ext2_dirent*)offset;
//
//            if(dirent->inode != 0) {
//                char buf[256];
//                memcpy(buf, dirent->name, dirent->name_len);
//                buf[dirent->name_len] = 0;
//                fprintf(stderr, "ext2: dirent [%s] rec_len=%d\n", buf, dirent->rec_len);
//            }
//
//            offset += dirent->rec_len;
//        }
//
//        palloc_abandon(dirdata, 0);
//    }

    return 0;
}

struct ext2_dirent* ext2_dirent_iter_next(struct ext2_dirent_iter* iter)
{
    // return false when we elapse the entire directory
    if(iter->offset >= iter->dir->i_size) return null;

    // current offset pointer is valid, so load the next dirent block
    if(iter->current_data_block == 0) {
        if(iter->dir->i_flags & EXT2_INDEX_FL) {
            fprintf(stderr, "directory has hash index\n");
        }

        // read the next block
        u32 block_index = iter->offset / BLOCK_SIZE;
        if(ext2_read_inode_block(iter->dir, block_index, &iter->current_data_block) < 0) {
            fprintf(stderr, "ext2: error reading directory block %d\n", block_index);
            return null;
        }

        // this value starts at 0 and increments by BLOCK_SIZE every time we move onto the next block
        iter->end_of_current_block_offset += BLOCK_SIZE;

        goto valid;
    }

    // move onto the next entry
    // we have to have had dirent set as valid at least one time before
    struct ext2_dirent* dirent = (struct ext2_dirent*)(iter->current_data_block + (iter->offset & (BLOCK_SIZE - 1)));
    iter->offset += dirent->rec_len;

    // if the offset goes beyond this block, go into the next one by recursively calling iter_next once
    if(iter->offset >= iter->end_of_current_block_offset) {
        // free the current block
        palloc_abandon(iter->current_data_block, 0);
        iter->current_data_block = 0;
        return ext2_dirent_iter_next(iter);
    }

valid:
    return (struct ext2_dirent*)(iter->current_data_block + (iter->offset & (BLOCK_SIZE - 1)));
}

void ext2_dirent_iter_done(struct ext2_dirent_iter* iter)
{
    if(iter->current_data_block != 0) {
        palloc_abandon(iter->current_data_block, 0);
    }
}
