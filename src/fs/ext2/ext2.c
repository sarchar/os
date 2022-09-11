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

#define NUM_BLOCKS(v)    (((v) + ((1024 << ext2_data.superblock->s_log_block_size) - 1)) / (1024 << ext2_data.superblock->s_log_block_size))
#define NUM_SECTORS(v)   (((v) + ext2_data.filesystem_callbacks->device_sector_size - 1) / ext2_data.filesystem_callbacks->device_sector_size)
#define BLOCK_SIZE       (1024ULL << ext2_data.superblock->s_log_block_size)
#define EXT2_INODE_SIZE  (ext2_data.superblock->s_inode_size)

#define INODE_BLOCK_INDIRECT0 13ULL
#define INODE_BLOCK_INDIRECT1 (INODE_BLOCK_INDIRECT0 + (BLOCK_SIZE / EXT2_INODE_SIZE))
#define INODE_BLOCK_INDIRECT2 (INODE_BLOCK_INDIRECT1 + ((BLOCK_SIZE * BLOCK_SIZE) / EXT2_INODE_SIZE))
#define INODE_BLOCK_INDIRECT3 (INODE_BLOCK_INDIRECT2 + ((BLOCK_SIZE * BLOCK_SIZE * BLOCK_SIZE) / EXT2_INODE_SIZE))

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

static u64 ext2_allocate_disk_item(u32 mode, bool want_inode);

static s64 ext2_read_blocks(u64 block_index, u32 block_count, intp* ret, u8* order)
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

static s64 ext2_write_blocks(u64 block_index, u32 block_count, intp src)
{
    //fprintf(stderr, "ext2: ext2_write_blocks(block_index=%d, block_count=%d, src=0x%lX)\n", block_index, block_count, src);

    u32 offs = block_index * BLOCK_SIZE; // offs will always be at least device sector aligned
    u32 size = block_count * BLOCK_SIZE;

    // read sector is truncated down from offs
    u32 sector = offs / ext2_data.filesystem_callbacks->device_sector_size; 

    if(!ext2_data.filesystem_callbacks->write_sectors(ext2_data.filesystem_callbacks, sector, NUM_SECTORS(size), src)) {
        return -1;
    }

    return 0;
}

static s64 ext2_write_superblock()
{
    u32 write_sector_count = (sizeof(struct ext2_superblock) + ext2_data.filesystem_callbacks->device_sector_size - 1) / ext2_data.filesystem_callbacks->device_sector_size;
    u32 sector = 1024 / ext2_data.filesystem_callbacks->device_sector_size;

    //fprintf(stderr, "ext2: ext2_write_superblock() write_sector_count=%d sector=%d\n", write_sector_count, sector);

    bool ret = ext2_data.filesystem_callbacks->write_sectors(ext2_data.filesystem_callbacks, sector, write_sector_count, (intp)ext2_data.superblock);
    if(!ret) return -1;
    return 0;
}

static s64 ext2_write_block_group_descriptor_table()
{
    u64 sb_block_index = ext2_data.superblock->s_first_data_block;
    u32 write_block_count = NUM_BLOCKS(ext2_data.num_block_groups / sizeof(struct ext2_block_group_descriptor));

    //fprintf(stderr, "ext2: ext2_write_block_group_descriptor_table() write_block_count=%d block=%d\n", write_block_count, sb_block_index + 1);

    if(ext2_write_blocks(sb_block_index + 1, write_block_count, (intp)ext2_data.bg_table) < 0) return -1;
    return 0;
}

s64 ext2_read_inode(u64 inode_number, struct inode** ret)
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
    intp table_offset_byte = EXT2_INODE_SIZE * block_inode;
    intp table_offset_block = table_offset_byte / BLOCK_SIZE;
    //fprintf(stderr, "ext2: inode %d has table_offset_byte=%d table_offset_block=%d\n", inode_number, table_offset_byte, table_offset_block);

    // read the block that has the inode
    intp table_data; 
    u8 order;
    //fprintf(stderr, "ext2: inode %d reading block %d\n", inode_number, inode_table + table_offset_block);
    if(ext2_read_blocks(inode_table + table_offset_block, 1, &table_data, &order) < 0) return -1;

    // allocate space for the inode
    struct ext2_inode* ext2_inode = (struct ext2_inode*)kalloc(EXT2_INODE_SIZE);
    *ret = (struct inode*)kalloc(sizeof(struct inode));
    zero(*ret);

    // set up the return inode
    (*ret)->inode_number = inode_number;
    (*ret)->ext2_inode = ext2_inode;

    // copy the inode
    //fprintf(stderr, "ext2: inode %d offset into inode table block %d\n", inode_number, table_offset_byte + (table_offset_block * BLOCK_SIZE));
    memcpy(ext2_inode, (void*)(table_data + table_offset_byte - (table_offset_block * BLOCK_SIZE)), EXT2_INODE_SIZE);

    // free the storage allocated for the table data
    palloc_abandon(table_data, order);

    return 0;
}

s64 ext2_write_inode(struct inode* inode)
{
    struct ext2_inode* ext2_inode = inode->ext2_inode;

    //fprintf(stderr, "ext2: ext2_read_inode(inode_number=%d, **ret=0x%lX)\n", inode_number, ret);

    // locate the inode block group
    intp block_group = (inode->inode_number - 1) / ext2_data.superblock->s_inodes_per_group;
    intp block_inode = (inode->inode_number - 1) % ext2_data.superblock->s_inodes_per_group;
    //fprintf(stderr, "ext2: inode %d has block_group=%d block_inode=%d\n", inode_number, block_group, block_inode);

    // locate the inode table
    intp inode_table = ext2_data.bg_table[block_group].bg_inode_table;
    //fprintf(stderr, "ext2: inode %d has inode_table at block %d\n", inode_number, inode_table);

    // need to read the block where the the inode data is
    // TODO might be a nice optimization to just read the *sector* where the data is. inodes are 128 bytes.
    intp table_offset_byte = EXT2_INODE_SIZE * block_inode;
    intp table_offset_block = table_offset_byte / BLOCK_SIZE;
    //fprintf(stderr, "ext2: inode %d has table_offset_byte=%d table_offset_block=%d\n", inode_number, table_offset_byte, table_offset_block);

    // read the block that has the inode
    intp table_data; 
    u8 order;
    //fprintf(stderr, "ext2: inode %d reading block %d\n", inode_number, inode_table + table_offset_block);
    if(ext2_read_blocks(inode_table + table_offset_block, 1, &table_data, &order) < 0) return -1;

    // copy the inode data into the table
    memcpy((void*)(table_data + table_offset_byte - (table_offset_block * BLOCK_SIZE)), (void*)ext2_inode, EXT2_INODE_SIZE);

    // write the block back to disk
    if(ext2_write_blocks(inode_table + table_offset_block, 1, table_data) < 0) return -1;

    // free the storage allocated for the table data
    palloc_abandon(table_data, order);

    return 0;
}

void ext2_free_inode(struct inode* inode)
{
    if(inode->ext2_inode != null) kfree(inode->ext2_inode, EXT2_INODE_SIZE);
    kfree(inode, sizeof(struct inode));
}

u64 ext2_block_size()
{
    return BLOCK_SIZE;
}

// inode_block_index is relative to the start of inode data
s64 ext2_write_inode_block(struct inode* inode, u64 inode_block_index, intp src)
{
    struct ext2_inode* ext2_inode = inode->ext2_inode;

    if(inode_block_index < INODE_BLOCK_INDIRECT0) {
        if(ext2_write_blocks(ext2_inode->i_block[inode_block_index], 1, src) < 0) return -1;
    } else if(inode_block_index < INODE_BLOCK_INDIRECT1) {
        assert(false, "TODO write nested blocks");
        return -1;
    } else if(inode_block_index < INODE_BLOCK_INDIRECT2) {
        assert(false, "TODO write doubly nested blocks");
        return -1;
    } else if(inode_block_index < INODE_BLOCK_INDIRECT3) {
        assert(false, "TODO write trebly nested blocks");
        return -1;
    } else {
        fprintf(stderr, "ext2: invalid block index %d\n", inode_block_index);
        return -1;
    }

    return 0;
}

// inode_block_index is relative to the start of inode data
// blocks are <= page size, so order of allocated memory is always 0
s64 ext2_read_inode_block(struct inode* inode, u64 inode_block_index, intp* ret)
{
    struct ext2_inode* ext2_inode = inode->ext2_inode;

    if(inode_block_index < INODE_BLOCK_INDIRECT0) {
        if(ext2_read_blocks(ext2_inode->i_block[inode_block_index], 1, ret, null) < 0) return -1;
    } else if(inode_block_index < INODE_BLOCK_INDIRECT1) {
        assert(false, "TODO read nested blocks");
        return -1;
    } else if(inode_block_index < INODE_BLOCK_INDIRECT2) {
        assert(false, "TODO read doubly nested blocks");
        return -1;
    } else if(inode_block_index < INODE_BLOCK_INDIRECT3) {
        assert(false, "TODO read trebly nested blocks");
        return -1;
    } else {
        fprintf(stderr, "ext2: invalid block index %d\n", inode_block_index);
        return -1;
    }

    return 0;
}

s64 ext2_ensure_inode_block(struct inode* inode, u64 inode_block_index)
{
    struct ext2_inode* ext2_inode = inode->ext2_inode;

    if(inode_block_index < INODE_BLOCK_INDIRECT0) {
        if(ext2_inode->i_block[inode_block_index] == 0) {
            ext2_inode->i_block[inode_block_index] = ext2_allocate_disk_item(0, false);
            ext2_inode->i_blocks += (BLOCK_SIZE >> 9); // i_blocks is allocated block space / 512
            ext2_write_inode(inode);
        }
    } else if(inode_block_index < INODE_BLOCK_INDIRECT1) {
        assert(false, "TODO ensure nested blocks");
        return -1;
    } else if(inode_block_index < INODE_BLOCK_INDIRECT2) {
        assert(false, "TODO ensure doubly nested blocks");
        return -1;
    } else if(inode_block_index < INODE_BLOCK_INDIRECT3) {
        assert(false, "TODO ensure trebly nested blocks");
        return -1;
    } else {
        fprintf(stderr, "ext2: invalid block index %d\n", inode_block_index);
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

    //fprintf(stderr, "ext2: superblock: s_rev_level=%d\n", sb->s_rev_level);
    //fprintf(stderr, "ext2: superblock: s_minor_rev_level=%d\n", sb->s_minor_rev_level);
    //fprintf(stderr, "ext2: superblock: s_blocks_count=%d\n", sb->s_blocks_count);
    //fprintf(stderr, "ext2: superblock: s_r_blocks_count=%d\n", sb->s_r_blocks_count);
    //fprintf(stderr, "ext2: superblock: s_free_inodes_count=%d\n", sb->s_free_inodes_count);
    //fprintf(stderr, "ext2: superblock: s_free_blocks_count=%d\n", sb->s_free_blocks_count);
    //fprintf(stderr, "ext2: superblock: s_log_block_size=%d (%d KiB)\n", sb->s_log_block_size, BLOCK_SIZE/1024);
    //fprintf(stderr, "ext2: superblock: s_log_frag_size=%d (%d KiB)\n", sb->s_log_frag_size, (1024 << sb->s_log_frag_size)/1024);
    //fprintf(stderr, "ext2: superblock: s_blocks_per_group=%d\n", sb->s_blocks_per_group);
    //fprintf(stderr, "ext2: superblock: s_inodes_per_group=%d\n", sb->s_inodes_per_group);
    //fprintf(stderr, "ext2: superblock: s_first_data_block=%d\n", sb->s_first_data_block);
    //fprintf(stderr, "ext2: superblock: s_inodes_count=%d\n", sb->s_inodes_count);
    //fprintf(stderr, "ext2: superblock: s_inode_size=%d\n", sb->s_inode_size);
    //fprintf(stderr, "ext2: superblock: s_creator_os=%d\n", sb->s_creator_os);
    //fprintf(stderr, "ext2: superblock: s_feature_compat=0x%02X\n", sb->s_feature_compat);
    //fprintf(stderr, "ext2: superblock: s_feature_incompat=0x%02X\n", sb->s_feature_incompat);
    //fprintf(stderr, "ext2: superblock: s_feature_ro_compat=0x%02X\n", sb->s_feature_ro_compat);
    //fprintf(stderr, "ext2: superblock: s_volume_name=%s\n", sb->s_volume_name);
    //fprintf(stderr, "ext2: superblock: s_algo_bitmap=0x%02X\n", sb->s_algo_bitmap);
    //fprintf(stderr, "ext2: superblock: s_first_ino=0x%02X\n", sb->s_first_ino);

    //fprintf(stderr, "ext2: superblock magic = 0x%04X\n", sb->s_magic);
    //fprintf(stderr, "ext2: filesystem size = %llu MiB\n", ((u64)sb->s_blocks_count * BLOCK_SIZE) / (1024*1024));
    //fprintf(stderr, "ext2: free space size = %llu MiB\n", ((u64)sb->s_free_blocks_count * BLOCK_SIZE) / (1024*1024));
    //fprintf(stderr, "ext2: num block groups = %d\n", (sb->s_blocks_count + (sb->s_blocks_per_group-1)) / sb->s_blocks_per_group);

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

    //struct ext2_block_group_descriptor* bg0 = &ext2_data.bg_table[32];
    //fprintf(stderr, "ext2: bg0: bg_block_bitmap=%d\n", bg0->bg_block_bitmap);
    //fprintf(stderr, "ext2: bg0: bg_inode_bitmap=%d\n", bg0->bg_inode_bitmap);
    //fprintf(stderr, "ext2: bg0: bg_inode_table=%d\n", bg0->bg_inode_table);
    //fprintf(stderr, "ext2: bg0: bg_free_blocks_count=%d\n", bg0->bg_free_blocks_count);
    //fprintf(stderr, "ext2: bg0: bg_free_inodes_count=%d\n", bg0->bg_free_inodes_count);
    //fprintf(stderr, "ext2: bg0: bg_used_dirs_count=%d\n", bg0->bg_used_dirs_count);

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
    struct ext2_inode* ext2_inode = iter->dir->ext2_inode;

    //fprintf(stderr, "ext2_dirent_iter_next: iter=0x%lX, iter->dir=0x%lX, iter->dir->ext2_inode=0x%lX\n",
    //        iter, iter->dir, iter->dir->ext2_inode);

    // return false when we elapse the entire directory
    if(iter->offset >= ext2_inode->i_size) return null;

    // current offset pointer is valid, so load the next dirent block
    if(iter->current_data_block == 0) {
        if(ext2_inode->i_flags & EXT2_INDEX_FL) {
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

// for blocks and inodes, finding a "good" block group isn't trivial. it's easy to find "a"
// block group by just iterating over them (what I'm doing here), but maybe in the future
// there will be some performance optimizations done.  See `find_group_orlov` in the linux kernel.
// However, one very simple optimization is to use the first block with less than the average of the free inodes
// and if there aren't any, then use some other heuristic.
// 
// this code allocates blocks or inodes, depending on whether want_inode==1. mode is only useful for
// knowing if the allocated inode is a directory
static u64 ext2_allocate_disk_item(u32 mode, bool want_inode)
{
    u8* bitmap_data = 0;
    u64 result = 0;

    // average free blocks/inodes per group
    u32 free_average = (want_inode ? ext2_data.superblock->s_free_inodes_count : ext2_data.superblock->s_free_blocks_count) / ext2_data.num_block_groups;
    //fprintf(stderr, "ext2: want_inode=%d free_average=%d\n", want_inode, free_average);

    for(u32 bg = 0; bg < ext2_data.num_block_groups; bg++) {
        u32 bitmap_block;
        u32 num_items_per_group;

        if(want_inode) {
            // no inodes? next block group
            if(ext2_data.bg_table[bg].bg_free_inodes_count == 0) continue;

            // more than the average? next block
            if(ext2_data.bg_table[bg].bg_free_inodes_count > free_average) continue;

            bitmap_block = ext2_data.bg_table[bg].bg_inode_bitmap;
            num_items_per_group = ext2_data.superblock->s_inodes_per_group;
        } else {
            // no blocks? next block group
            if(ext2_data.bg_table[bg].bg_free_blocks_count == 0) continue;

            // more than the average? next block
            if(ext2_data.bg_table[bg].bg_free_blocks_count > free_average) continue;

            bitmap_block = ext2_data.bg_table[bg].bg_block_bitmap;
            num_items_per_group = ext2_data.superblock->s_blocks_per_group;
        }

        // found a block group with a free inode/block, read the bitmap. the bitmap for both is 1 block in size
        if(ext2_read_blocks(bitmap_block, 1, (intp*)&bitmap_data, null) < 0) goto error;

        // loop over bits to find a free entry
        for(u32 bit = 0; bit < num_items_per_group; bit++) {
            u8 byte_index = bit >> 3;
            u8 bit_index = bit & 0x07;
            if(bitmap_data[byte_index] & (1 << bit_index)) continue;

            // found a bit, so determine the index
            result = num_items_per_group * bg + bit;

            // mark the bit set
            bitmap_data[byte_index] |= (1 << bit_index);

            // write the bitmap back to disk
            if(ext2_write_blocks(bitmap_block, 1, (intp)bitmap_data) < 0) goto error;

            // decrement the count in the block group table and superblock
            if(want_inode) {
                ext2_data.bg_table[bg].bg_free_inodes_count -= 1;
                if(mode & EXT2_S_IFDIR) ext2_data.bg_table[bg].bg_used_dirs_count += 1;
                ext2_data.superblock->s_free_inodes_count -= 1;
            } else {
                ext2_data.bg_table[bg].bg_free_blocks_count -= 1;
                ext2_data.superblock->s_free_blocks_count -= 1;
            }

            // write block group table to disk
            if(ext2_write_block_group_descriptor_table() < 0) goto error;

            // write superblock to disk
            if(ext2_write_superblock() < 0) goto error;

            // free memory
            palloc_abandon((intp)bitmap_data, 0);
            return (want_inode) ? (result + 1) : result;
        }
    }

    assert(false, "no disk space left?");
    return 0;

error:
    if(bitmap_data != 0) palloc_abandon((intp)bitmap_data, 0);
    return 0;
}

// TODO use the hash table
static s64 ext2_add_directory_entry(struct inode* dir, char* filename, struct inode* entry)
{
    u32 name_len = strlen(filename);
    u32 rec_len = sizeof(struct ext2_dirent) + name_len;
    u8* buf = __builtin_alloca(rec_len);
    struct ext2_dirent* new_dirent = (struct ext2_dirent*)buf;

    // fill out the new_dirent structure
    new_dirent->inode_number = entry->inode_number;
    new_dirent->rec_len = rec_len;
    new_dirent->name_len = name_len;
    new_dirent->file_type = EXT2_ISDIR(entry) ? 2 : 1; // TODO vfs file types
    memcpy((void*)(buf + sizeof(struct ext2_dirent)), filename, name_len); // copy filename over

    // loop over the directory entries looking for the last one
    struct ext2_dirent_iter iter = EXT2_DIRENT_ITER_INIT(dir);
    struct ext2_dirent* dirent = ext2_dirent_iter_next(&iter);

    // if the first call to ext2_dirent_iter_next was null, that means the directory was empty
    if(dirent == null) {
        fprintf(stderr, "dirent is empty\n"); // probably never happens due to '.' and '..' entries

        // fixup rec_len to point to the end of the block
        new_dirent->rec_len = BLOCK_SIZE;
        ext2_write_inode_data(dir, 0, (u8*)buf, rec_len);

        dir->ext2_inode->i_size += BLOCK_SIZE;
        ext2_write_inode(dir);

        ext2_dirent_iter_done(&iter);
        return 0;
    }
    
    do {
        // iter wasn't null, so this entry is valid. check if the rec_len points beyond the directory size,
        // and if so, fix it to point to the new entry and set our new entry to 
        if((iter.offset + dirent->rec_len) < dir->ext2_inode->i_size) continue;

        // our new entry goes right after the current one
        u32 actual_rec_len = (u32)((intp)__alignup(sizeof(struct ext2_dirent) + dirent->name_len, 4) & 0xFFFFFFFF); // directory entries must be 4 byte aligned
        u32 new_offset = iter.offset + actual_rec_len;

        // but we need enough space before the end of the block
        u32 end_offset = new_offset + actual_rec_len;
        if(end_offset > iter.end_of_current_block_offset) { // need a new block
            assert(false, "need a new block");

            //TODO ensure a block exists at offset X
            //ext2_ensure_inode_block(...)

            //TODO update the size of dir (directories only grow in blocks)
            //!dir->ext2_inode->i_size += BLOCK_SIZE;
            //!ext2_write_inode(dir);
        } 

        // fix up the last node's pointer and point ours to the end of the block
        dirent->rec_len = actual_rec_len;
        new_dirent->rec_len = iter.end_of_current_block_offset - new_offset;

        // copy new entry data into the block
        memcpy((void*)((u8*)dirent + dirent->rec_len), buf, rec_len);

        // write block to disk. use the offset of the current 'dirent' so that it gets updated too
        //ext2_write_inode_data(dir, iter.offset, (u8*)dirent, actual_rec_len + rec_len);
        ext2_write_inode_block(dir, (iter.end_of_current_block_offset - BLOCK_SIZE) / BLOCK_SIZE, iter.current_data_block);

        // done
        ext2_dirent_iter_done(&iter);
        return 0;

    } while((dirent = ext2_dirent_iter_next(&iter)) != null); // loop until the end of the directory entries

    // should never get here
    assert(false, "problem with directory structure");
}

s64 ext2_write_inode_data(struct inode* inode, u64 offset, u8 const* data, u64 count)
{
    u64 total = 0;
    intp block_data;

    while(count > 0) {
        u64 inode_block_index = offset / BLOCK_SIZE;
        u64 inode_block_offset = offset % BLOCK_SIZE;
        u64 wrsize = min(count, BLOCK_SIZE - inode_block_offset);

        // inode must have this block
        if(ext2_ensure_inode_block(inode, inode_block_index) < 0) return total;

        // read it off disk if less than a full block of data is being written
        // TODO *and* the block is within the bounds of the file (i.e., it has data there)
        if(wrsize < BLOCK_SIZE) {
            ext2_read_inode_block(inode, inode_block_index, &block_data);
        } else {
            block_data = palloc_claim_one();
        }

        // overwrite data
        memcpy((void*)block_data, data, wrsize);

        // write back to disk
        ext2_write_inode_block(inode, inode_block_index, block_data);

        // free allocated space
        palloc_abandon(block_data, 0);

        data += wrsize;
        offset += wrsize;
        total += wrsize;
        count -= wrsize;
    }

    return 0;
}

s64 ext2_create_file(struct inode* dir, char* filename, struct inode** file_inode)
{
    // create a new inode
    struct ext2_inode* ext2_inode = (struct ext2_inode*)kalloc(EXT2_INODE_SIZE);
    memset(ext2_inode, 0, EXT2_INODE_SIZE);
    ext2_inode->i_uid = 1000;
    ext2_inode->i_gid = 1000;
    ext2_inode->i_links_count = 1;
    ext2_inode->i_mode = EXT2_S_IFREG | 0x0180; // user read/write

    struct inode* inode = (struct inode*)kalloc(sizeof(struct inode));
    zero(inode);
    inode->ext2_inode = ext2_inode;

    // allocate an inode on disk
    inode->inode_number = ext2_allocate_disk_item(0, true);
    //fprintf(stderr, "ext2_allocate_disk_inode returned %d\n", inode->inode_number);

    // write the inode to disk
    ext2_write_inode(inode);

    // add inode to the directory
    if(ext2_add_directory_entry(dir, filename, inode) < 0) {
        ext2_free_inode(inode);
        return -1;
    }

    *file_inode = inode;
    return 0;
}

s64 ext2_create_directory(struct inode* dir, char* dirname, struct inode** newdir)
{
    // create a new inode
    struct ext2_inode* ext2_inode = (struct ext2_inode*)kalloc(EXT2_INODE_SIZE);
    memset(ext2_inode, 0, EXT2_INODE_SIZE);
    ext2_inode->i_uid = 1000;
    ext2_inode->i_gid = 1000;
    ext2_inode->i_links_count = 2; // parent's referral to this directory and '.' directory references
    ext2_inode->i_mode = EXT2_S_IFDIR | 0x01C0; // user read/write/exec

    struct inode* inode = (struct inode*)kalloc(sizeof(struct inode));
    zero(inode);
    inode->ext2_inode = ext2_inode;

    // allocate an inode on disk
    inode->inode_number = ext2_allocate_disk_item(ext2_inode->i_mode, true);
    //fprintf(stderr, "ext2_allocate_disk_inode returned %d\n", inode->inode_number);

    // write the inode to disk
    ext2_write_inode(inode);

    // add '.' to this new directory
    if(ext2_add_directory_entry(inode, ".", inode) < 0) {
        ext2_free_inode(inode);
        return -1;
    }

    // add '..' to this new directory (increasing the parent directory link count)
    if(ext2_add_directory_entry(inode, "..", dir) < 0) {
        ext2_free_inode(inode);
        return -1;
    }

    // add inode to the parent directory
    if(ext2_add_directory_entry(dir, dirname, inode) < 0) {
        ext2_free_inode(inode);
        return -1;
    }

    // link count to the parent directory increased
    dir->ext2_inode->i_links_count += 1;
    ext2_write_inode(dir);

    // return
    if(newdir != null) *newdir = inode;
    return 0;
}


