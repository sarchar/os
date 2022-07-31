#ifndef __EXT2_H__
#define __EXT2_H__

enum EXT2_MODE_FLAGS {
    EXT2_S_IFMODE = 0xF000,
    EXT2_S_IFSOCK = 0xC000,
    EXT2_S_IFLNK  = 0xA000,
    EXT2_S_IFREG  = 0x8000,
    EXT2_S_IFBLK  = 0x6000,
    EXT2_S_IFDIR  = 0x4000,
    EXT2_S_IFCHR  = 0x2000,
    EXT2_S_IFFIFO = 0x1000,
};

enum EXT2_INODE_FLAGS {
    EXT2_SECRM_FL        = 0x00000001,
    EXT2_UNRM_FL         = 0x00000002,
    EXT2_COMPR_FL        = 0x00000004,
    EXT2_SYNC_FL         = 0x00000008,
    EXT2_IMMUTABLE_FL    = 0x00000010,
    EXT2_APPEND_FL       = 0x00000020,
    EXT2_NODUMP_FL       = 0x00000040,
    EXT2_NOATIME_FL      = 0x00000080,
    // compression flags
    EXT2_DIRTY_FL        = 0x00000100,
    EXT2_COMPRBLK_FL     = 0x00000200,
    EXT2_NOCOMPR_FL      = 0x00000400,
    EXT2_ECOMPR_FL       = 0x00000800,
    // index flags
    EXT2_BTREE_FL        = 0x00001000,
    EXT2_INDEX_FL        = 0x00001000,
    EXT2_IMAGIC_FL       = 0x00002000,
    EXT3_JOURNAL_DATA_FL = 0x00004000,
    EXT2_RESERVED_FL     = 0x80000000
};

#define _EXT2_IS(inode,mode) (((inode)->i_mode & EXT2_S_IFMODE) == mode)
#define EXT2_ISDIR(inode)    _EXT2_IS(inode,EXT2_S_IFDIR)

struct filesystem_callbacks {
    bool (*read_sectors)(struct filesystem_callbacks*, u64, u64, intp);
    bool (*write_sectors)(struct filesystem_callbacks*, u64, u64, intp);
    u32   device_sector_size;
    void* userdata;
};

struct ext2_inode {
    u16 i_mode;
    u16 i_uid;
    u32 i_size;
    u32 i_atime;
    u32 i_ctime;
    u32 i_mtime;
    u32 i_dtime;
    u16 i_gid;
    u16 i_links_count;
    u32 i_blocks;
    u32 i_flags;
    u32 i_osd1;
    u32 i_block[15];
    u32 i_generation;
    u32 i_file_acl;
    u32 i_dir_acl;
    u32 i_faddr;
    u8  i_osd2[12];
};

struct ext2_dirent {
    u32 inode;
    u16 rec_len;
    u8  name_len;
    u8  file_type;
    u8  name[];
};

struct ext2_dirent_iter {
    struct ext2_inode* dir;
    intp   offset;
    intp   current_data_block;
    u64    end_of_current_block_offset;
};

#define EXT2_DIRENT_ITER_INIT(dir_inode) { dir_inode, 0, 0, 0 }

s64 ext2_open(struct filesystem_callbacks*);
u64 ext2_block_size();
s64 ext2_read_inode(u64, struct ext2_inode**);
void ext2_free_inode(struct ext2_inode*);
s64 ext2_read_inode_block(struct ext2_inode*, u64, intp*);

struct ext2_dirent* ext2_dirent_iter_next(struct ext2_dirent_iter*);
void ext2_dirent_iter_done(struct ext2_dirent_iter*);


#endif
