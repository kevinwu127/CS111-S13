#include <linux/autoconf.h>
#include <linux/version.h>
#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#include <linux/module.h>
#include <linux/moduleparam.h>
#include "ospfs.h"
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <asm/uaccess.h>
#include <linux/kernel.h>
#include <linux/sched.h>

/****************************************************************************
 * ospfsmod
 *
 *   This is the OSPFS module!  It contains both library code for your use,
 *   and exercises where you must add code.
 *
 ****************************************************************************/

/* Define eprintk() to be a version of printk(), which prints messages to
 * the console.
 * (If working on a real Linux machine, change KERN_NOTICE to KERN_ALERT or
 * KERN_EMERG so that you are sure to see the messages.  By default, the
 * kernel does not print all messages to the console.  Levels like KERN_ALERT
 * and KERN_EMERG will make sure that you will see messages.) */
#define eprintk(format, ...) printk(KERN_NOTICE format, ## __VA_ARGS__)

// The actual disk data is just an array of raw memory.
// The initial array is defined in fsimg.c, based on your 'base' directory.
extern uint8_t ospfs_data[];
extern uint32_t ospfs_length;

// A pointer to the superblock; see ospfs.h for details on the struct.
static ospfs_super_t * const ospfs_super =
	(ospfs_super_t *) &ospfs_data[OSPFS_BLKSIZE];

static int change_size(ospfs_inode_t *oi, uint32_t want_size);
static ospfs_direntry_t *find_direntry(ospfs_inode_t *dir_oi, const char *name, int namelen);

/*****************************************************************************
 * FILE SYSTEM OPERATIONS STRUCTURES
 *
 *   Linux filesystems are based around three interrelated structures.
 *
 *   These are:
 *
 *   1. THE LINUX SUPERBLOCK.  This structure represents the whole file system.
 *      Example members include the root directory and the number of blocks
 *      on the disk.
 *   2. LINUX INODES.  Each file and directory in the file system corresponds
 *      to an inode.  Inode operations include "mkdir" and "create" (add to
 *      directory).
 *   3. LINUX FILES.  Corresponds to an open file or directory.  Operations
 *      include "read", "write", and "readdir".
 *
 *   When Linux wants to perform some file system operation,
 *   it calls a function pointer provided by the file system type.
 *   (Thus, Linux file systems are object oriented!)
 *
 *   These function pointers are grouped into structures called "operations"
 *   structures.
 *
 *   The initial portion of the file declares all the operations structures we
 *   need to support ospfsmod: one for the superblock, several for different
 *   kinds of inodes and files.  There are separate inode_operations and
 *   file_operations structures for OSPFS directories and for regular OSPFS
 *   files.  The structures are actually defined near the bottom of this file.
 */

// Basic file system type structure
// (links into Linux's list of file systems it supports)
static struct file_system_type ospfs_fs_type;
// Inode and file operations for regular files
static struct inode_operations ospfs_reg_inode_ops;
static struct file_operations ospfs_reg_file_ops;
// Inode and file operations for directories
static struct inode_operations ospfs_dir_inode_ops;
static struct file_operations ospfs_dir_file_ops;
// Inode operations for symbolic links
static struct inode_operations ospfs_symlink_inode_ops;
// Other required operations
static struct dentry_operations ospfs_dentry_ops;
static struct super_operations ospfs_superblock_ops;



/*****************************************************************************
 * BITVECTOR OPERATIONS
 *
 *   OSPFS uses a free bitmap to keep track of free blocks.
 *   These bitvector operations, which set, clear, and test individual bits
 *   in a bitmap, may be useful.
 */

// bitvector_set -- Set 'i'th bit of 'vector' to 1.
static inline void
bitvector_set(void *vector, int i)
{
	((uint32_t *) vector) [i / 32] |= (1 << (i % 32));
}

// bitvector_clear -- Set 'i'th bit of 'vector' to 0.
static inline void
bitvector_clear(void *vector, int i)
{
	((uint32_t *) vector) [i / 32] &= ~(1 << (i % 32));
}

// bitvector_test -- Return the value of the 'i'th bit of 'vector'.
static inline int
bitvector_test(const void *vector, int i)
{
	return (((const uint32_t *) vector) [i / 32] & (1 << (i % 32))) != 0;
}



/*****************************************************************************
 * OSPFS HELPER FUNCTIONS
 */

// ospfs_size2nblocks(size)
//	Returns the number of blocks required to hold 'size' bytes of data.
//
//   Input:   size -- file size
//   Returns: a number of blocks

uint32_t
ospfs_size2nblocks(uint32_t size)
{
	return (size + OSPFS_BLKSIZE - 1) / OSPFS_BLKSIZE;
}


// ospfs_block(blockno)
//	Use this function to load a block's contents from "disk".
//
//   Input:   blockno -- block number
//   Returns: a pointer to that block's data

static void *
ospfs_block(uint32_t blockno)
{
	return &ospfs_data[blockno * OSPFS_BLKSIZE];
}


// ospfs_inode(ino)
//	Use this function to load a 'ospfs_inode' structure from "disk".
//
//   Input:   ino -- inode number
//   Returns: a pointer to the corresponding ospfs_inode structure

static inline ospfs_inode_t *
ospfs_inode(ino_t ino)
{
	ospfs_inode_t *oi;
	if (ino >= ospfs_super->os_ninodes)
		return 0;
	oi = ospfs_block(ospfs_super->os_firstinob);
	return &oi[ino];
}


// ospfs_inode_blockno(oi, offset)
//	Use this function to look up the blocks that are part of a file's
//	contents.
//
//   Inputs:  oi     -- pointer to a OSPFS inode
//	      offset -- byte offset into that inode
//   Returns: the block number of the block that contains the 'offset'th byte
//	      of the file

static inline uint32_t
ospfs_inode_blockno(ospfs_inode_t *oi, uint32_t offset)
{
	uint32_t blockno = offset / OSPFS_BLKSIZE;
	if (offset >= oi->oi_size || oi->oi_ftype == OSPFS_FTYPE_SYMLINK)
		return 0;
	else if (blockno >= OSPFS_NDIRECT + OSPFS_NINDIRECT) {
		uint32_t blockoff = blockno - (OSPFS_NDIRECT + OSPFS_NINDIRECT);
		uint32_t *indirect2_block = ospfs_block(oi->oi_indirect2);
		uint32_t *indirect_block = ospfs_block(indirect2_block[blockoff / OSPFS_NINDIRECT]);
		return indirect_block[blockoff % OSPFS_NINDIRECT];
	} else if (blockno >= OSPFS_NDIRECT) {
		uint32_t *indirect_block = ospfs_block(oi->oi_indirect);
		return indirect_block[blockno - OSPFS_NDIRECT];
	} else
		return oi->oi_direct[blockno];
}


// ospfs_inode_data(oi, offset)
//	Use this function to load part of inode's data from "disk",
//	where 'offset' is relative to the first byte of inode data.
//
//   Inputs:  oi     -- pointer to a OSPFS inode
//	      offset -- byte offset into 'oi's data contents
//   Returns: a pointer to the 'offset'th byte of 'oi's data contents
//
//	Be careful: the returned pointer is only valid within a single block.
//	This function is a simple combination of 'ospfs_inode_blockno'
//	and 'ospfs_block'.

static inline void *
ospfs_inode_data(ospfs_inode_t *oi, uint32_t offset)
{
	uint32_t blockno = ospfs_inode_blockno(oi, offset);
	return (uint8_t *) ospfs_block(blockno) + (offset % OSPFS_BLKSIZE);
}


/*****************************************************************************
 * LOW-LEVEL FILE SYSTEM FUNCTIONS
 * There are no exercises in this section, and you don't need to understand
 * the code.
 */

// ospfs_mk_linux_inode(sb, ino)
//	Linux's in-memory 'struct inode' structure represents disk
//	objects (files and directories).  Many file systems have their own
//	notion of inodes on disk, and for such file systems, Linux's
//	'struct inode's are like a cache of on-disk inodes.
//
//	This function takes an inode number for the OSPFS and constructs
//	and returns the corresponding Linux 'struct inode'.
//
//   Inputs:  sb  -- the relevant Linux super_block structure (one per mount)
//	      ino -- OSPFS inode number
//   Returns: 'struct inode'

static struct inode *
ospfs_mk_linux_inode(struct super_block *sb, ino_t ino)
{
	ospfs_inode_t *oi = ospfs_inode(ino);
	struct inode *inode;

	if (!oi)
		return 0;
	if (!(inode = new_inode(sb)))
		return 0;

	inode->i_ino = ino;
	// Make it look like everything was created by root.
	inode->i_uid = inode->i_gid = 0;
	inode->i_size = oi->oi_size;

	if (oi->oi_ftype == OSPFS_FTYPE_REG) {
		// Make an inode for a regular file.
		inode->i_mode = oi->oi_mode | S_IFREG;
		inode->i_op = &ospfs_reg_inode_ops;
		inode->i_fop = &ospfs_reg_file_ops;
		inode->i_nlink = oi->oi_nlink;

	} else if (oi->oi_ftype == OSPFS_FTYPE_DIR) {
		// Make an inode for a directory.
		inode->i_mode = oi->oi_mode | S_IFDIR;
		inode->i_op = &ospfs_dir_inode_ops;
		inode->i_fop = &ospfs_dir_file_ops;
		inode->i_nlink = oi->oi_nlink + 1 /* dot-dot */;

	} else if (oi->oi_ftype == OSPFS_FTYPE_SYMLINK) {
		// Make an inode for a symbolic link.
		inode->i_mode = S_IRUSR | S_IRGRP | S_IROTH
			| S_IWUSR | S_IWGRP | S_IWOTH
			| S_IXUSR | S_IXGRP | S_IXOTH | S_IFLNK;
		inode->i_op = &ospfs_symlink_inode_ops;
		inode->i_nlink = oi->oi_nlink;

	} else
		panic("OSPFS: unknown inode type!");

	// Access and modification times are now.
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	return inode;
}


// ospfs_fill_super, ospfs_get_sb
//	These functions are called by Linux when the user mounts a version of
//	the OSPFS onto some directory.  They help construct a Linux
//	'struct super_block' for that file system.

static int
ospfs_fill_super(struct super_block *sb, void *data, int flags)
{
	struct inode *root_inode;

	sb->s_blocksize = OSPFS_BLKSIZE;
	sb->s_blocksize_bits = OSPFS_BLKSIZE_BITS;
	sb->s_magic = OSPFS_MAGIC;
	sb->s_op = &ospfs_superblock_ops;

	if (!(root_inode = ospfs_mk_linux_inode(sb, OSPFS_ROOT_INO))
	    || !(sb->s_root = d_alloc_root(root_inode))) {
		iput(root_inode);
		sb->s_dev = 0;
		return -ENOMEM;
	}

	return 0;
}

static int
ospfs_get_sb(struct file_system_type *fs_type, int flags, const char *dev_name, void *data, struct vfsmount *mount)
{
	return get_sb_single(fs_type, flags, data, ospfs_fill_super, mount);
}


// ospfs_delete_dentry
//	Another bookkeeping function.

static int
ospfs_delete_dentry(struct dentry *dentry)
{
	return 1;
}


/*****************************************************************************
 * DIRECTORY OPERATIONS
 *
 * EXERCISE: Finish 'ospfs_dir_readdir' and 'ospfs_symlink'.
 */

// ospfs_dir_lookup(dir, dentry, ignore)
//	This function implements the "lookup" directory operation, which
//	looks up a named entry.
//
//	We have written this function for you.
//
//   Input:  dir    -- The Linux 'struct inode' for the directory.
//		       You can extract the corresponding 'ospfs_inode_t'
//		       by calling 'ospfs_inode' with the relevant inode number.
//	     dentry -- The name of the entry being looked up.
//   Effect: Looks up the entry named 'dentry'.  If found, attaches the
//	     entry's 'struct inode' to the 'dentry'.  If not found, returns
//	     a "negative dentry", which has no inode attachment.

static struct dentry *
ospfs_dir_lookup(struct inode *dir, struct dentry *dentry, struct nameidata *ignore)
{
	// Find the OSPFS inode corresponding to 'dir'
	ospfs_inode_t *dir_oi = ospfs_inode(dir->i_ino);
	struct inode *entry_inode = NULL;
	int entry_off;

	// Make sure filename is not too long
	if (dentry->d_name.len > OSPFS_MAXNAMELEN)
		return (struct dentry *) ERR_PTR(-ENAMETOOLONG);

	// Mark with our operations
	dentry->d_op = &ospfs_dentry_ops;

	// Search through the directory block
	for (entry_off = 0; entry_off < dir_oi->oi_size;
	     entry_off += OSPFS_DIRENTRY_SIZE) {
		// Find the OSPFS inode for the entry
		ospfs_direntry_t *od = ospfs_inode_data(dir_oi, entry_off);

		// Set 'entry_inode' if we find the file we are looking for
		if (od->od_ino > 0
		    && strlen(od->od_name) == dentry->d_name.len
		    && memcmp(od->od_name, dentry->d_name.name, dentry->d_name.len) == 0) {
			entry_inode = ospfs_mk_linux_inode(dir->i_sb, od->od_ino);
			if (!entry_inode)
				return (struct dentry *) ERR_PTR(-EINVAL);
			break;
		}
	}

	// We return a dentry whether or not the file existed.
	// The file exists if and only if 'entry_inode != NULL'.
	// If the file doesn't exist, the dentry is called a "negative dentry".

	// d_splice_alias() attaches the inode to the dentry.
	// If it returns a new dentry, we need to set its operations.
	if ((dentry = d_splice_alias(entry_inode, dentry)))
		dentry->d_op = &ospfs_dentry_ops;
	return dentry;
}


// ospfs_dir_readdir(filp, dirent, filldir)
//   This function is called when the kernel reads the contents of a directory
//   (i.e. when file_operations.readdir is called for the inode).
//
//   Inputs:  filp	-- The 'struct file' structure correspoding to
//			   three open directory.
//			   The most important member is 'filp->f_pos', the
//			   File POSition.  This remembers how far into the
//			   directory we are, so if the user calls 'readdir'
//			   twice, we don't forget our position.
//			   This function must update 'filp->f_pos'.
//	      dirent	-- Used to pass to 'filldir'.
//	      filldir	-- A pointer to a callback function.
//			   This function should call 'filldir' once for each
//			   directory entry, passing it six arguments:
//		  (1) 'dirent'.
//		  (2) The directory entry's name.
//		  (3) The length of the directory entry's name.
//		  (4) The 'f_pos' value corresponding to the directory entry.
//		  (5) The directory entry's inode number.
//		  (6) DT_REG, for regular files; DT_DIR, for subdirectories;
//		      or DT_LNK, for symbolic links.
//			   This function should stop returning directory
//			   entries either when the directory is complete, or
//			   when 'filldir' returns < 0, whichever comes first.
//
//   Returns: 1 at end of directory, 0 if filldir returns < 0 before the end
//     of the directory, and -(error number) on error.
//
//   EXERCISE: Finish implementing this function.

static int
ospfs_dir_readdir(struct file *filp, void *dirent, filldir_t filldir)
{
	struct inode *dir_inode = filp->f_dentry->d_inode;
	ospfs_inode_t *dir_oi = ospfs_inode(dir_inode->i_ino);
	uint32_t f_pos = filp->f_pos;
	int r = 0;		/* Error return value, if any */
	int ok_so_far = 0;	/* Return value from 'filldir' */

	// f_pos is an offset into the directory's data, plus two.
	// The "plus two" is to account for "." and "..".
	if (r == 0 && f_pos == 0) {
		ok_so_far = filldir(dirent, ".", 1, f_pos, dir_inode->i_ino, DT_DIR);
		if (ok_so_far >= 0)
			f_pos++;
	}

	if (r == 0 && ok_so_far >= 0 && f_pos == 1) {
		ok_so_far = filldir(dirent, "..", 2, f_pos, filp->f_dentry->d_parent->d_inode->i_ino, DT_DIR);
		if (ok_so_far >= 0)
			f_pos++;
	}

//lab3 code begins
	uint32_t offset = 0;
	uint32_t d_size = dir_oi->oi_size;
//lab3 code ends

	// actual entries
	while (r == 0 && ok_so_far >= 0 && f_pos >= 2) {
		ospfs_direntry_t *od;
		ospfs_inode_t *entry_oi;

		/* If at the end of the directory, set 'r' to 1 and exit
		 * the loop.  For now we do this all the time.
		 *
		 * EXERCISE: Your code here */
//lab3 code begins
		offset = (f_pos-2)*OSPFS_DIRENTRY_SIZE;
		if (offset >= d_size) { //end of directory
			r = 1;		/* Fix me! */
			break;		/* Fix me! */
		}
//lab3 code ends
		/* Get a pointer to the next entry (od) in the directory.
		 * The file system interprets the contents of a
		 * directory-file as a sequence of ospfs_direntry structures.
		 * You will find 'f_pos' and 'ospfs_inode_data' useful.
		 *
		 * Then use the fields of that file to fill in the directory
		 * entry.  To figure out whether a file is a regular file or
		 * another directory, use 'ospfs_inode' to get the directory
		 * entry's corresponding inode, and check out its 'oi_ftype'
		 * member.
		 *
		 * Make sure you ignore blank directory entries!  (Which have
		 * an inode number of 0.)
		 *
		 * If the current entry is successfully read (the call to
		 * filldir returns >= 0), or the current entry is skipped,
		 * your function should advance f_pos by the proper amount to
		 * advance to the next directory entry.
		 */

		/* EXERCISE: Your code here */
//lab3 code begins
		od = ospfs_inode_data(dir_oi, offset);
		if(od->od_ino) { //valid entry
			ospfs_inode_t *oi = ospfs_inode(od->od_ino);
			uint32_t typef;
			if (oi->oi_ftype == OSPFS_FTYPE_REG)
				typef = DT_REG;
			else if (oi->oi_ftype == OSPFS_FTYPE_DIR)
				typef = DT_DIR;
			else 
				typef = DT_LNK;
			ok_so_far = filldir(dirent, od->od_name, strlen(od->od_name), f_pos, od->od_ino, typef);
			if (ok_so_far < 0) {
				r = 0;
				break;
			}
		}
		f_pos++;	
//lab3 code ends
	}

	// Save the file position and return!
	filp->f_pos = f_pos;
	return r;
}


// ospfs_unlink(dirino, dentry)
//   This function is called to remove a file.
//
//   Inputs: dirino  -- You may ignore this.
//           dentry  -- The 'struct dentry' structure, which contains the inode
//                      the directory entry points to and the directory entry's
//                      directory.
//
//   Returns: 0 if success and -ENOENT on entry not found.
//
//   EXERCISE: Make sure that deleting symbolic links works correctly.

static int
ospfs_unlink(struct inode *dirino, struct dentry *dentry)
{
	ospfs_inode_t *oi = ospfs_inode(dentry->d_inode->i_ino);
	ospfs_inode_t *dir_oi = ospfs_inode(dentry->d_parent->d_inode->i_ino);
	int entry_off;
	ospfs_direntry_t *od;

	od = NULL; // silence compiler warning; entry_off indicates when !od
	for (entry_off = 0; entry_off < dir_oi->oi_size;
	     entry_off += OSPFS_DIRENTRY_SIZE) {
		od = ospfs_inode_data(dir_oi, entry_off);
		if (od->od_ino > 0
		    && strlen(od->od_name) == dentry->d_name.len
		    && memcmp(od->od_name, dentry->d_name.name, dentry->d_name.len) == 0)
			break;
	}

	if (entry_off == dir_oi->oi_size) {
		printk("<1>ospfs_unlink should not fail!\n");
		return -ENOENT;
	}

	od->od_ino = 0;
	oi->oi_nlink--;
	return 0;
}



/*****************************************************************************
 * FREE-BLOCK BITMAP OPERATIONS
 *
 * EXERCISE: Implement these functions.
 */

// allocate_block()
//	Use this function to allocate a block.
//
//   Inputs:  none
//   Returns: block number of the allocated block,
//	      or 0 if the disk is full
//
//   This function searches the free-block bitmap, which starts at Block 2, for
//   a free block, allocates it (by marking it non-free), and returns the block
//   number to the caller.  The block itself is not touched.
//
//   Note:  A value of 0 for a bit indicates the corresponding block is
//      allocated; a value of 1 indicates the corresponding block is free.
//
//   You can use the functions bitvector_set(), bitvector_clear(), and
//   bitvector_test() to do bit operations on the map.

static uint32_t
allocate_block(void)
{
	/* EXERCISE: Your code here */ //deepak
//lab3 code begins
	uint32_t ninodeblocks = ospfs_size2nblocks(ospfs_super->os_ninodes*OSPFS_INODESIZE);
	uint32_t firstdb = ospfs_super->os_firstinob + ninodeblocks;
	uint32_t lastdb = firstdb + ospfs_super->os_nblocks - 1;

	uint32_t dblockno;
	for (dblockno = firstdb; dblockno <= lastdb; dblockno++) {
		if (bitvector_test(&ospfs_data[OSPFS_FREEMAP_BLK*OSPFS_BLKSIZE], dblockno)) { 
			//free - allocate and return
			bitvector_clear(&ospfs_data[OSPFS_FREEMAP_BLK*OSPFS_BLKSIZE], dblockno);
			return dblockno;
		}
			
	}
//lab3 code ends
	return 0;
}


// free_block(blockno)
//	Use this function to free an allocated block.
//
//   Inputs:  blockno -- the block number to be freed
//   Returns: none
//
//   This function should mark the named block as free in the free-block
//   bitmap.  (You might want to program defensively and make sure the block
//   number isn't obviously bogus: the boot sector, superblock, free-block
//   bitmap, and inode blocks must never be freed.  But this is not required.)

static void
free_block(uint32_t blockno)
{
	/* EXERCISE: Your code here */ //deepak
//lab3 code begins
	uint32_t ninodeblocks = ospfs_size2nblocks(ospfs_super->os_ninodes*OSPFS_INODESIZE);
	uint32_t firstdb = ospfs_super->os_firstinob + ninodeblocks;
	uint32_t lastdb = firstdb + ospfs_super->os_nblocks - 1;

	if ((blockno >= firstdb) && (blockno <= lastdb)) // valid block number
		bitvector_set(&ospfs_data[OSPFS_FREEMAP_BLK*OSPFS_BLKSIZE], blockno);
	//return 0;
//lab3 code ends
}


/*****************************************************************************
 * FILE OPERATIONS
 *
 * EXERCISE: Finish off change_size, read, and write.
 *
 * The find_*, add_block, and remove_block functions are only there to support
 * the change_size function.  If you prefer to code change_size a different
 * way, then you may not need these functions.
 *
 */

// The following functions are used in our code to unpack a block number into
// its consituent pieces: the doubly indirect block number (if any), the
// indirect block number (which might be one of many in the doubly indirect
// block), and the direct block number (which might be one of many in an
// indirect block).  We use these functions in our implementation of
// change_size.


// int32_t indir2_index(uint32_t b)
//	Returns the doubly-indirect block index for file block b.
//
// Inputs:  b -- the zero-based index of the file block (e.g., 0 for the first
//		 block, 1 for the second, etc.)
// Returns: 0 if block index 'b' requires using the doubly indirect
//	       block, -1 if it does not.
//
// EXERCISE: Fill in this function.

static int32_t
indir2_index(uint32_t b)
{
	// Your code here.
//lab3 code begins
	if (b >= (OSPFS_NDIRECT + OSPFS_NINDIRECT))
		return 0;
//lab3 code ends;
	return -1;
}


// int32_t indir_index(uint32_t b)
//	Returns the indirect block index for file block b.
//
// Inputs:  b -- the zero-based index of the file block
// Returns: -1 if b is one of the file's direct blocks;
//	    0 if b is located under the file's first indirect block;
//	    otherwise, the offset of the relevant indirect block within
//		the doubly indirect block.
//
// EXERCISE: Fill in this function.

static int32_t
indir_index(uint32_t b)
{
	// Your code here.
//lab3 code begins
	if (b >= (OSPFS_NDIRECT + OSPFS_NINDIRECT))
		return (b - (OSPFS_NDIRECT + OSPFS_NINDIRECT))/256;
	if (b >= OSPFS_NDIRECT)
		return 0;
//lab3 code ends;
	return -1;
}


// int32_t indir_index(uint32_t b)
//	Returns the indirect block index for file block b.
//
// Inputs:  b -- the zero-based index of the file block
// Returns: the index of block b in the relevant indirect block or the direct
//	    block array.
//
// EXERCISE: Fill in this function.

static int32_t
direct_index(uint32_t b)
{
	// Your code here.
//lab3 code begins
	if (b < OSPFS_NDIRECT) //direct
		return b;
	if (b < (OSPFS_NDIRECT + OSPFS_NINDIRECT)) //first indirect
		return (b - OSPFS_NDIRECT);
	return (b - (OSPFS_NDIRECT + OSPFS_NINDIRECT))%256;
	//return -1;
//lab3 code ends;
	
}


// add_block(ospfs_inode_t *oi)
//   Adds a single data block to a file, adding indirect and
//   doubly-indirect blocks if necessary. (Helper function for
//   change_size).
//
// Inputs: oi -- pointer to the file we want to grow
// Returns: 0 if successful, < 0 on error.  Specifically:
//          -ENOSPC if you are unable to allocate a block
//          due to the disk being full or
//          -EIO for any other error.
//          If the function is successful, then oi->oi_size
//          should be set to the maximum file size in bytes that could
//          fit in oi's data blocks.  If the function returns an error,
//          then oi->oi_size should remain unchanged. Any newly
//          allocated blocks should be erased (set to zero).
//
// EXERCISE: Finish off this function.
//
// Remember that allocating a new data block may require allocating
// as many as three disk blocks, depending on whether a new indirect
// block and/or a new indirect^2 block is required. If the function
// fails with -ENOSPC or -EIO, then you need to make sure that you
// free any indirect (or indirect^2) blocks you may have allocated!
//
// Also, make sure you:
//  1) zero out any new blocks that you allocate
//  2) store the disk block number of any newly allocated block
//     in the appropriate place in the inode or one of the
//     indirect blocks.
//  3) update the oi->oi_size field

static int
add_block(ospfs_inode_t *oi)
{
	// current number of blocks in file
	uint32_t n = ospfs_size2nblocks(oi->oi_size);

	// keep track of allocations to free in case of -ENOSPC
	uint32_t *allocated[2] = { 0, 0 };

	/* EXERCISE: Your code here */
//lab3 code begins
	if (n == OSPFS_MAXFILEBLKS)
		return -ENOSPC;

	uint32_t oindex = n-1; //old block index, starts with 0
	uint32_t nindex = oindex+1; //new block index, starts with 0
	uint32_t zerofill = 0;
	int retval = 0;
	uint32_t dirbno, indirbno, indir2bno;
	uint32_t *dirb = NULL, *indirb = NULL, *indir2b = NULL;

	if (dirbno = allocate_block()) {
		retval = -ENOSPC;
		goto done;
	}
	dirb = ospfs_block(dirbno);
	if(copy_from_user(dirb, &zerofill, OSPFS_BLKSIZE)) { //erase data block
		retval = -EIO;
		goto freedirb;
	}

	//find indices of last block of file
	int32_t oindex_indir = indir_index(oindex);

	//find indices of new block
	int32_t nindex_direct = direct_index(nindex);
	int32_t nindex_indir = indir_index(nindex);

	int32_t index_type, index, offset;

	if (nindex_indir == -1) { //direct
		index_type = 0;
		index = -1;
		offset = nindex_direct;
	}
	else if (nindex_indir == 0) { //first indirect
		index_type = 1;
		index = 0;
		offset = nindex_direct;
	}
	else { //double indirect
		index_type = 2;
		index = nindex_indir;
		offset = nindex_direct;
	}
	
	if (index_type == 0) { //direct
		oi->oi_direct[offset] = dirbno;
	}
	else if (index_type == 1) { //first indirect
		if (oindex_indir == 0) { //previous block also in first indirect
			indirbno = oi->oi_indirect;
			indirb = ospfs_block(indirbno);
			indirb += offset; //indirb increments by 32 bits, or 4 bytes
			if (copy_from_user(indirb, &dirbno, 4)) {
				retval = -EIO;
				goto freedirb;
			}
		}
		else { //allocate indirect block
			if (indirbno = allocate_block()) {
				retval = -ENOSPC;
				goto freedirb;
			}
			indirb = ospfs_block(indirbno);
			if (copy_from_user(indirb, &zerofill, OSPFS_BLKSIZE)) { //erase indirect block
				retval = -EIO;
				goto freeindirb;

			}
			if (copy_from_user(indirb, &dirbno, 4)) { //offset = 0
				retval = -EIO;
				goto freeindirb;
			}
			oi->oi_indirect = indirbno;
		}
	}
	else { //double indirect
		if (oindex_indir > 0) { //previous block also in double indirect block
			indir2bno = oi->oi_indirect2;
			indir2b = ospfs_block(indir2bno);
			if (oindex_indir  == nindex_indir) { //also in same indirect block
				indir2b += index;
				if (copy_to_user(&indirbno, indir2b, 4)) {
					retval = -EIO;
					goto freedirb;
				}
				indirb = ospfs_block(indirbno);
				indirb += offset; //indirb increments by 32 bits, or 4 bytes
				if (copy_from_user(indirb, &dirbno, 4)) {
					retval = -EIO;
					goto freedirb;
				}		
			}
			else { //allocate indirect block
				if (indirbno = allocate_block()) {
					retval = -ENOSPC;
					goto freedirb;
				}
				indirb = ospfs_block(indirbno);
				if (copy_from_user(indirb, &zerofill, OSPFS_BLKSIZE)) { //erase indirect block
					retval = -EIO;
					goto freeindirb;

				}
				if (copy_from_user(indirb, &dirbno, 4)) { //offset = 0
					retval = -EIO;
					goto freeindirb;
				}
				indir2b += index; //indir2b increments by 32 bits, or 4 bytes
				if (copy_from_user(indir2b, &indirbno, 4)) { //offset = 0
					retval = -EIO;
					goto freeindirb;
				}
			}
		}
		else { //allocate double indirect block
			if (indirbno = allocate_block()) {
				retval = -ENOSPC;
				goto freedirb;
			}
			indirb = ospfs_block(indirbno);
			if (copy_from_user(indirb, &zerofill, OSPFS_BLKSIZE)) { //erase indirect block
				retval = -EIO;
				goto freeindirb;

			}
			if (copy_from_user(indirb, &dirbno, 4)) { //offset = 0
				retval = -EIO;
				goto freeindirb;
			}
			if (indir2bno = allocate_block()) {
				retval = -ENOSPC;
				goto freeindirb;
			}			
			indir2b = ospfs_block(indir2bno);
			if (copy_from_user(indir2b, &zerofill, OSPFS_BLKSIZE)) { //erase double indirect block
				retval = -EIO;
				goto freeindir2b;

			}
			if (copy_from_user(indir2b, &indirbno, 4)) { //offset = 0
				retval = -EIO;
				goto freeindir2b;
			}
			oi->oi_indirect2 = indir2bno;
		}
	}
	
	oi->oi_size += OSPFS_BLKSIZE;
	retval = 0;
	goto done;

	freeindir2b:
		free_block(indir2bno);
	freeindirb:
		free_block(indirbno);
	freedirb:
		free_block(dirbno); //file size not changed
	done:
		return retval;
	//return -EIO; // Replace this line
//lab3 code ends
}


// remove_block(ospfs_inode_t *oi)
//   Removes a single data block from the end of a file, freeing
//   any indirect and indirect^2 blocks that are no
//   longer needed. (Helper function for change_size)
//
// Inputs: oi -- pointer to the file we want to shrink
// Returns: 0 if successful, < 0 on error.
//          If the function is successful, then oi->oi_size
//          should be set to the maximum file size that could
//          fit in oi's blocks.  If the function returns -EIO (for
//          instance if an indirect block that should be there isn't),
//          then oi->oi_size should remain unchanged.
//
// EXERCISE: Finish off this function.
//
// Remember that you must free any indirect and doubly-indirect blocks
// that are no longer necessary after shrinking the file.  Removing a
// single data block could result in as many as 3 disk blocks being
// deallocated.  Also, if you free a block, make sure that
// you set the block pointer to 0.  Don't leave pointers to
// deallocated blocks laying around!

static int
remove_block(ospfs_inode_t *oi)
{
	// current number of blocks in file
	uint32_t n = ospfs_size2nblocks(oi->oi_size);

	/* EXERCISE: Your code here */
//lab3 code begins
	if (!n)
		return 0;

	uint32_t cindex = n-1; //previous block index, starts with 0
	uint32_t pindex = cindex-1; //previous block index, starts with 0
	uint32_t zerofill = 0;
	int retval = 0;
	uint32_t dirbno, indirbno, indir2bno;
	uint32_t *dirb = NULL, *indirb = NULL, *indir2b = NULL;

	//find indices of current block of file
	int32_t cindex_direct = direct_index(cindex);
	int32_t cindex_indir = indir_index(cindex);

	//find indices of previous block
	int32_t pindex_indir = indir_index(pindex);

	int32_t index_type, index, offset;

	if (cindex_indir == -1) { //direct
		index_type = 0;
		index = -1;
		offset = cindex_direct;
	}
	else if (cindex_indir == 0) { //first indirect
		index_type = 1;
		index = 0;
		offset = cindex_direct;
	}
	else { //double indirect
		index_type = 2;
		index = cindex_indir;
		offset = cindex_direct;
	}
	
	if (index_type == 0) { //direct
		dirbno = oi->oi_direct[offset];
		oi->oi_direct[offset] = 0;
		goto freedirb;
	}
	else if (index_type == 1) { //first indirect
		indirbno = oi->oi_indirect;
		indirb = ospfs_block(indirbno);
		if (pindex_indir == 0) { //previous block also in first indirect
			indirb += offset; //indirb increments by 32 bits, or 4 bytes
			if (copy_to_user(&dirbno, indirb, 4)) {
				retval = -EIO;
				goto done;
			}
			if (copy_from_user(indirb, &zerofill, 4))
				retval = -EIO;
			goto freedirb;
		}
		else { //free indirect block
			oi->oi_indirect = 0;
			if (copy_to_user(&dirbno, indirb, 4)) { //offset = 0
				retval = -EIO;
				goto done;
			}
			goto freeindirb;
		}
	}
	else { //double indirect
		indir2bno = oi->oi_indirect2;
		indir2b = ospfs_block(indir2bno);
		if (pindex_indir > 0) { //previous block also in double indirect block
			indir2b += index;
			if (copy_to_user(&indirbno, indir2b, 4)) {
				retval = -EIO;
				goto done;
			}
			indirb = ospfs_block(indirbno);
			if (pindex_indir  == cindex_indir) { //previous also in same indirect block
				indirb += offset; //indirb increments by 32 bits, or 4 bytes
				if (copy_to_user(&dirbno, indirb, 4)) {
					retval = -EIO;
					goto done;
				}
				if (copy_from_user(indirb, &zerofill, 4))
					retval = -EIO;
				goto freedirb;	
			}
			else { //free indirect block
				if (copy_to_user(&dirbno, indirb, 4)) { //offset = 0
					retval = -EIO;
					goto done;
				}
				if (copy_from_user(indir2b, &zerofill, 4))
					retval = -EIO;
				goto freeindirb;
			}
		}
		else { //free double indirect block
			indir2bno = oi->oi_indirect2;
			oi->oi_indirect2 = 0;
			indir2b = ospfs_block(indir2bno);
			if (copy_to_user(&indirbno, indir2b, 4)) { //offset = 0
				retval = -EIO;
				goto done;
			}
			indirb = ospfs_block(indirbno);
			if (copy_to_user(&dirbno, indirb, 4)) { //offset = 0
				retval = -EIO;
				goto done;
			}
			free_block(indir2bno);
		}
	}

	freeindirb:
		free_block(indirbno);
	freedirb:
		free_block(dirbno); //file size not changed
		oi->oi_size -= OSPFS_BLKSIZE;
	done:
		return retval;
	//return -EIO; // Replace this line
//lab3 code ends
}


// change_size(oi, want_size)
//	Use this function to change a file's size, allocating and freeing
//	blocks as necessary.
//
//   Inputs:  oi	-- pointer to the file whose size we're changing
//	      want_size -- the requested size in bytes
//   Returns: 0 on success, < 0 on error.  In particular:
//		-ENOSPC: if there are no free blocks available
//		-EIO:    an I/O error -- for example an indirect block should
//			 exist, but doesn't
//	      If the function succeeds, the file's oi_size member should be
//	      changed to want_size, with blocks allocated as appropriate.
//	      Any newly-allocated blocks should be erased (set to 0).
//	      If there is an -ENOSPC error when growing a file,
//	      the file size and allocated blocks should not change from their
//	      original values!!!
//            (However, if there is an -EIO error, do not worry too much about
//	      restoring the file.)
//
//   If want_size has the same number of blocks as the current file, life
//   is good -- the function is pretty easy.  But the function might have
//   to add or remove blocks.
//
//   If you need to grow the file, then do so by adding one block at a time
//   using the add_block function you coded above. If one of these additions
//   fails with -ENOSPC, you must shrink the file back to its original size!
//
//   If you need to shrink the file, remove blocks from the end of
//   the file one at a time using the remove_block function you coded above.
//
//   Also: Don't forget to change the size field in the metadata of the file.
//         (The value that the final add_block or remove_block set it to
//          is probably not correct).
//
//   EXERCISE: Finish off this function.

static int
change_size(ospfs_inode_t *oi, uint32_t new_size)
{
	uint32_t old_size = oi->oi_size;
	int r = 0;

	while (ospfs_size2nblocks(oi->oi_size) < ospfs_size2nblocks(new_size)) {
	        /* EXERCISE: Your code here */
//lab3 code begins
		if (new_size > OSPFS_MAXFILESIZE)
			return -ENOSPC;
		int addresult = add_block(oi);
		if (addresult < 0)
			return addresult;
		//return -EIO; // Replace this line
//lab3 code ends
	}
	while (ospfs_size2nblocks(oi->oi_size) > ospfs_size2nblocks(new_size)) {
	        /* EXERCISE: Your code here */
//lab3 code begins
		if (!new_size)
			return 0;
		int removeresult = remove_block(oi);
		if (removeresult < 0)
			return removeresult;
		//return -EIO; // Replace this line
//lab3 code ends
	}

	/* EXERCISE: Make sure you update necessary file meta data
	             and return the proper value. */
//lab3 code begins
//All metadata updates handled in add_block() and remove_block() functions. 
	//return -EIO; // Replace this line
//lab3 code ends
}


// ospfs_notify_change
//	This function gets called when the user changes a file's size,
//	owner, or permissions, among other things.
//	OSPFS only pays attention to file size changes (see change_size above).
//	We have written this function for you -- except for file quotas.

static int
ospfs_notify_change(struct dentry *dentry, struct iattr *attr)
{
	struct inode *inode = dentry->d_inode;
	ospfs_inode_t *oi = ospfs_inode(inode->i_ino);
	int retval = 0;

	if (attr->ia_valid & ATTR_SIZE) {
		// We should not be able to change directory size
		if (oi->oi_ftype == OSPFS_FTYPE_DIR)
			return -EPERM;
		if ((retval = change_size(oi, attr->ia_size)) < 0)
			goto out;
	}

	if (attr->ia_valid & ATTR_MODE)
		// Set this inode's mode to the value 'attr->ia_mode'.
		oi->oi_mode = attr->ia_mode;

	if ((retval = inode_change_ok(inode, attr)) < 0
	    || (retval = inode_setattr(inode, attr)) < 0)
		goto out;

    out:
	return retval;
}


// ospfs_read
//	Linux calls this function to read data from a file.
//	It is the file_operations.read callback.
//
//   Inputs:  filp	-- a file pointer
//            buffer    -- a user space ptr where data should be copied
//            count     -- the amount of data requested
//            f_pos     -- points to the file position
//   Returns: Number of chars read on success, -(error code) on error.
//
//   This function copies the corresponding bytes from the file into the user
//   space ptr (buffer).  Use copy_to_user() to accomplish this.
//   The current file position is passed into the function
//   as 'f_pos'; read data starting at that position, and update the position
//   when you're done.
//
//   EXERCISE: Complete this function.

static ssize_t
ospfs_read(struct file *filp, char __user *buffer, size_t count, loff_t *f_pos)
{
	ospfs_inode_t *oi = ospfs_inode(filp->f_dentry->d_inode->i_ino);
	int retval = 0;
	size_t amount = 0;

	// Make sure we don't read past the end of the file!
	// Change 'count' so we never read past the end of the file.
	/* EXERCISE: Your code here */ 
//lab3 code begins
	uint32_t fsize = (size_t)oi->oi_size;
	if (*f_pos >= fsize) { //invalid offset
		retval = -EFAULT;
		goto done;
	}
	uint32_t fsize_pos = fsize - (*f_pos + 1); //pos starts from 0, so add 1
	if (count > fsize_pos)
		count = fsize_pos;
//lab3 code ends

	// Copy the data to user block by block
	while (amount < count && retval >= 0) {
		uint32_t blockno = ospfs_inode_blockno(oi, *f_pos);
		uint32_t n;
		char *data;

		// ospfs_inode_blockno returns 0 on error
		if (blockno == 0) {
			retval = -EIO;
			goto done;
		}

		data = ospfs_block(blockno);

		// Figure out how much data is left in this block to read.
		// Copy data into user space. Return -EFAULT if unable to write
		// into user space.
		// Use variable 'n' to track number of bytes moved.
		/* EXERCISE: Your code here */
//lab3 code begins
		size_t blockstart = (size_t)(blockno * OSPFS_BLKSIZE);
		size_t blockend = blockstart + OSPFS_BLKSIZE - 1;
		size_t blockoff = *f_pos - blockstart;
		data +=  blockoff;

		size_t blockremain = blockend - blockoff + 1; //includes both ends
		size_t totalremain = count - amount;
		n = (totalremain > blockremain)? blockremain : totalremain;

		if (copy_to_user(buffer, data, n)) {
			retval = -EFAULT;
			goto done;
		}

		//retval = -EIO; // Replace these lines
		//goto done;
//lab3 code ends
		buffer += n;
		amount += n;
		*f_pos += n;
	}

    done:
	return (retval >= 0 ? amount : retval);
}


// ospfs_write
//	Linux calls this function to write data to a file.
//	It is the file_operations.write callback.
//
//   Inputs:  filp	-- a file pointer
//            buffer    -- a user space ptr where data should be copied from
//            count     -- the amount of data to write
//            f_pos     -- points to the file position
//   Returns: Number of chars written on success, -(error code) on error.
//
//   This function copies the corresponding bytes from the user space ptr
//   into the file.  Use copy_from_user() to accomplish this. Unlike read(),
//   where you cannot read past the end of the file, it is OK to write past
//   the end of the file; this should simply change the file's size.
//
//   EXERCISE: Complete this function.

static ssize_t
ospfs_write(struct file *filp, const char __user *buffer, size_t count, loff_t *f_pos)
{
	ospfs_inode_t *oi = ospfs_inode(filp->f_dentry->d_inode->i_ino);
	int retval = 0;
	size_t amount = 0;

	// Support files opened with the O_APPEND flag.  To detect O_APPEND,
	// use struct file's f_flags field and the O_APPEND bit.
	/* EXERCISE: Your code here */ 
//lab3 code begins
	if (!count)
		return count;

	uint32_t csize = oi->oi_size, nsize;
	int chszret = 0;

	if (filp->f_flags & O_APPEND) { // openned in append mode, so append
		if (*f_pos > csize)
			nsize = *f_pos + count;
		else 
			nsize = csize + count;
	}
	else { //overwrite from start
		nsize = *f_pos + count;
	}
	chszret = change_size(oi, nsize);
	if (!chszret)
		return chszret;
//lab3 code ends

	// If the user is writing past the end of the file, change the file's
	// size to accomodate the request.  (Use change_size().)
	/* EXERCISE: Your code here */
//lab3 code begins
//handled in above code
//lab3 code ends

	// Copy data block by block
	while (amount < count && retval >= 0) {
		uint32_t blockno = ospfs_inode_blockno(oi, *f_pos);
		uint32_t n;
		char *data;

		if (blockno == 0) {
			retval = -EIO;
			goto done;
		}

		data = ospfs_block(blockno);

		// Figure out how much data is left in this block to write.
		// Copy data from user space. Return -EFAULT if unable to read
		// read user space.
		// Keep track of the number of bytes moved in 'n'.
		/* EXERCISE: Your code here */
//lab3 code begins
		size_t blockstart = (size_t)(blockno * OSPFS_BLKSIZE);
		size_t blockend = blockstart + OSPFS_BLKSIZE - 1;
		size_t blockoff = *f_pos - blockstart;
		data +=  blockoff;

		size_t blockremain = blockend - blockoff + 1; //includes both ends
		size_t totalremain = count - amount;
		n = (totalremain > blockremain)? blockremain : totalremain;

		if (copy_from_user(data, buffer, n)) {
			retval = -EFAULT;
			goto done;
		}

		//retval = -EIO; // Replace these lines
		//goto done;
//lab3 code ends

		buffer += n;
		amount += n;
		*f_pos += n;
	}

    done:
	return (retval >= 0 ? amount : retval);
}


// find_direntry(dir_oi, name, namelen)
//	Looks through the directory to find an entry with name 'name' (length
//	in characters 'namelen').  Returns a pointer to the directory entry,
//	if one exists, or NULL if one does not.
//
//   Inputs:  dir_oi  -- the OSP inode for the directory
//	      name    -- name to search for
//	      namelen -- length of 'name'.  (If -1, then use strlen(name).)
//
//	We have written this function for you.

static ospfs_direntry_t *
find_direntry(ospfs_inode_t *dir_oi, const char *name, int namelen)
{
	int off;
	if (namelen < 0)
		namelen = strlen(name);
	for (off = 0; off < dir_oi->oi_size; off += OSPFS_DIRENTRY_SIZE) {
		ospfs_direntry_t *od = ospfs_inode_data(dir_oi, off);
		if (od->od_ino
		    && strlen(od->od_name) == namelen
		    && memcmp(od->od_name, name, namelen) == 0)
			return od;
	}
	return 0;
}


// create_blank_direntry(dir_oi)
//	'dir_oi' is an OSP inode for a directory.
//	Return a blank directory entry in that directory.  This might require
//	adding a new block to the directory.  Returns an error pointer (see
//	below) on failure.
//
// ERROR POINTERS: The Linux kernel uses a special convention for returning
// error values in the form of pointers.  Here's how it works.
//	- ERR_PTR(errno): Creates a pointer value corresponding to an error.
//	- IS_ERR(ptr): Returns true iff 'ptr' is an error value.
//	- PTR_ERR(ptr): Returns the error value for an error pointer.
//	For example:
//
//	static ospfs_direntry_t *create_blank_direntry(...) {
//		return ERR_PTR(-ENOSPC);
//	}
//	static int ospfs_create(...) {
//		...
//		ospfs_direntry_t *od = create_blank_direntry(...);
//		if (IS_ERR(od))
//			return PTR_ERR(od);
//		...
//	}
//
//	The create_blank_direntry function should use this convention.
//
// EXERCISE: Write this function.

static ospfs_direntry_t *
create_blank_direntry(ospfs_inode_t *dir_oi)
{
	// Outline:
	// 1. Check the existing directory data for an empty entry.  Return one
	//    if you find it.
	// 2. If there's no empty entries, add a block to the directory.
	//    Use ERR_PTR if this fails; otherwise, clear out all the directory
	//    entries and return one of them.

	/* EXERCISE: Your code here. */
//lab3 code begins
	if (dir_oi->oi_ftype != OSPFS_FTYPE_DIR) //not directory
		return ERR_PTR(-EINVAL);

	size_t csize = dir_oi->oi_size;
	uint32_t cblocks = ospfs_size2nblocks(csize);
	
	size_t nsize = csize + OSPFS_DIRENTRY_SIZE;
	uint32_t nblocks = ospfs_size2nblocks(nsize);

	uint32_t offset, zerofill = 0, retval;
	ospfs_direntry_t *od = NULL;

	for (offset = 0; offset < csize; offset += OSPFS_DIRENTRY_SIZE) {
		od = ospfs_inode_data(dir_oi, offset);
		if (od->od_ino == 0) { //empty entry
			if (copy_from_user(od, &zerofill, OSPFS_DIRENTRY_SIZE))
				return ERR_PTR(-EIO);
			else
				return od;
		}
	}

	if (retval = change_size(dir_oi, nsize))
		return ERR_PTR(retval);
			
	od = ospfs_inode_data(dir_oi, csize);
	if (copy_from_user(od, &zerofill, OSPFS_DIRENTRY_SIZE))
		return ERR_PTR(-EIO);
	else
		return od;
	//return ERR_PTR(-EINVAL); // Replace this line
//lab3 code ends
}

// ospfs_link(src_dentry, dir, dst_dentry
//   Linux calls this function to create hard links.
//   It is the ospfs_dir_inode_ops.link callback.
//
//   Inputs: src_dentry   -- a pointer to the dentry for the source file.  This
//                           file's inode contains the real data for the hard
//                           linked filae.  The important elements are:
//                             src_dentry->d_name.name
//                             src_dentry->d_name.len
//                             src_dentry->d_inode->i_ino
//           dir          -- a pointer to the containing directory for the new
//                           hard link.
//           dst_dentry   -- a pointer to the dentry for the new hard link file.
//                           The important elements are:
//                             dst_dentry->d_name.name
//                             dst_dentry->d_name.len
//                             dst_dentry->d_inode->i_ino
//                           Two of these values are already set.  One must be
//                           set by you, which one?
//   Returns: 0 on success, -(error code) on error.  In particular:
//               -ENAMETOOLONG if dst_dentry->d_name.len is too large, or
//			       'symname' is too long;
//               -EEXIST       if a file named the same as 'dst_dentry' already
//                             exists in the given 'dir';
//               -ENOSPC       if the disk is full & the file can't be created;
//               -EIO          on I/O error.
//
//   EXERCISE: Complete this function.

static int
ospfs_link(struct dentry *src_dentry, struct inode *dir, struct dentry *dst_dentry) {
	/* EXERCISE: Your code here. */
//lab3 code begins
	if (!src_dentry || !dir || !dst_dentry)
		return -EINVAL; 

	if ((src_dentry->d_name.len == 0) || (src_dentry->d_inode->i_ino == 0) || (dst_dentry->d_name.len == 0))
		return -EINVAL;

	uint32_t src_oino = src_dentry->d_inode->i_ino;
	ospfs_inode_t *src_oi = ospfs_inode(src_oino);
	if (src_oi->oi_ftype == OSPFS_FTYPE_DIR)
		return -EINVAL;

	if (dst_dentry->d_name.len > OSPFS_MAXNAMELEN)
		return -ENAMETOOLONG;

	struct dentry *dst_tmp = ospfs_dir_lookup(dir, dst_dentry, NULL);
	if (IS_ERR(dst_tmp))
		return PTR_ERR(dst_tmp);
	if (dst_tmp->d_inode)
		return -EEXIST;

	dst_dentry->d_inode->i_ino = src_oino;

	ospfs_inode_t *dir_oi = ospfs_inode(dir->i_ino);
	ospfs_direntry_t *od = create_blank_direntry(dir_oi);
	if (IS_ERR(od))
		return PTR_ERR(od);

	if(copy_from_user(od, &src_oino, 4)) //inode number
		return -EIO;
	if(copy_from_user(od+4, dst_dentry->d_name.name, dst_dentry->d_name.len)) //name
		return -EIO;

	src_oi->oi_nlink++;

	return 0;
	//return -EIO; // Replace this lines
//lab3 code ends
}

// ospfs_create
//   Linux calls this function to create a regular file.
//   It is the ospfs_dir_inode_ops.create callback.
//
//   Inputs:  dir	-- a pointer to the containing directory's inode
//            dentry    -- the name of the file that should be created
//                         The only important elements are:
//                         dentry->d_name.name: filename (char array, not null
//                            terminated)
//                         dentry->d_name.len: length of filename
//            mode	-- the permissions mode for the file (set the new
//			   inode's oi_mode field to this value)
//	      nd	-- ignore this
//   Returns: 0 on success, -(error code) on error.  In particular:
//               -ENAMETOOLONG if dentry->d_name.len is too large;
//               -EEXIST       if a file named the same as 'dentry' already
//                             exists in the given 'dir';
//               -ENOSPC       if the disk is full & the file can't be created;
//               -EIO          on I/O error.
//
//   We have provided strictly less skeleton code for this function than for
//   the others.  Here's a brief outline of what you need to do:
//   1. Check for the -EEXIST error and find an empty directory entry using the
//	helper functions above.
//   2. Find an empty inode.  Set the 'entry_ino' variable to its inode number.
//   3. Initialize the directory entry and inode.
//
//   EXERCISE: Complete this function.

static int
ospfs_create(struct inode *dir, struct dentry *dentry, int mode, struct nameidata *nd)
{
	ospfs_inode_t *dir_oi = ospfs_inode(dir->i_ino);
	uint32_t entry_ino = 0;
	/* EXERCISE: Your code here. */
//lab3 code begins
	if (!dir || !dentry)
		return -EINVAL;

	if (dentry->d_name.len == 0)
		return -EINVAL;

	if (dentry->d_name.len > OSPFS_MAXNAMELEN)
		return -ENAMETOOLONG;

	struct dentry *f_tmp = ospfs_dir_lookup(dir, dentry, NULL);
	if (IS_ERR(f_tmp))
		return PTR_ERR(f_tmp);
	if (f_tmp->d_inode)
		return -EEXIST;

	ospfs_inode_t *oi = ospfs_block(ospfs_super->os_firstinob);
	ospfs_inode_t *f_oi = NULL;
	int zerofill = 0;
	for (entry_ino = 0; entry_ino < ospfs_super->os_ninodes; entry_ino++) {
		f_oi = &oi[entry_ino];
		if (!f_oi->oi_nlink) //link count 0 means free inode
			break;
		f_oi = NULL;
	}

	if (!f_oi || (entry_ino == ospfs_super->os_ninodes)) //no free inode
		return -ENOSPC;
	if (copy_from_user(f_oi, &zerofill, OSPFS_INODESIZE))
		return -EIO;

	ospfs_direntry_t *od = create_blank_direntry(dir_oi);
	if (IS_ERR(od))
		return PTR_ERR(od);

	if(copy_from_user(od, &entry_ino, 4)) //inode number
		return -EIO;
	if(copy_from_user(od+4, dentry->d_name.name, dentry->d_name.len)) //name
		return -EIO;

	ospfs_inode_t temp;
	temp.oi_size = 0;
	temp.oi_ftype = OSPFS_FTYPE_REG;
	temp.oi_nlink = 1;
	temp.oi_mode = mode;
	memset(temp.oi_direct, 0, OSPFS_NDIRECT);
	temp.oi_indirect = 0;
	temp.oi_indirect2 = 0;

	if(copy_from_user(f_oi, &temp, OSPFS_INODESIZE))
		return -EIO;

	//return -EINVAL; // Replace this line
//lab3 code ends

	/* Execute this code after your function has successfully created the
	   file.  Set entry_ino to the created file's inode number before
	   getting here. */
	{
		struct inode *i = ospfs_mk_linux_inode(dir->i_sb, entry_ino);
		if (!i)
			return -ENOMEM;
		d_instantiate(dentry, i);
		return 0;
	}
}


// ospfs_symlink(dirino, dentry, symname)
//   Linux calls this function to create a symbolic link.
//   It is the ospfs_dir_inode_ops.symlink callback.
//
//   Inputs: dir     -- a pointer to the containing directory's inode
//           dentry  -- the name of the file that should be created
//                      The only important elements are:
//                      dentry->d_name.name: filename (char array, not null
//                           terminated)
//                      dentry->d_name.len: length of filename
//           symname -- the symbolic link's destination
//
//   Returns: 0 on success, -(error code) on error.  In particular:
//               -ENAMETOOLONG if dentry->d_name.len is too large, or
//			       'symname' is too long;
//               -EEXIST       if a file named the same as 'dentry' already
//                             exists in the given 'dir';
//               -ENOSPC       if the disk is full & the file can't be created;
//               -EIO          on I/O error.
//
//   EXERCISE: Complete this function.

static int
ospfs_symlink(struct inode *dir, struct dentry *dentry, const char *symname)
{
	ospfs_inode_t *dir_oi = ospfs_inode(dir->i_ino);
	uint32_t entry_ino = 0;

	/* EXERCISE: Your code here. */
//lab3 code begins
	if (!dir || !dentry || !symname)
		return -EINVAL;

	if ((dentry->d_name.len == 0) || (strlen(symname) == 0))
		return -EINVAL;

	if ((dentry->d_name.len > OSPFS_MAXNAMELEN) || (strlen(symname) > OSPFS_MAXSYMLINKLEN))
		return -ENAMETOOLONG;

	struct dentry *sym_tmp = ospfs_dir_lookup(dir, dentry, NULL);
	if (IS_ERR(sym_tmp))
		return PTR_ERR(sym_tmp);
	if (sym_tmp->d_inode)
		return -EEXIST;

	ospfs_inode_t *oi = ospfs_block(ospfs_super->os_firstinob);
	ospfs_inode_t *sym_oi = NULL;
	int zerofill = 0;
	for (entry_ino = 0; entry_ino < ospfs_super->os_ninodes; entry_ino++) {
		sym_oi = &oi[entry_ino];
		if (!sym_oi->oi_nlink) //link count 0 means free inode
			break;
		sym_oi = NULL;
	}

	if (!sym_oi || (entry_ino == ospfs_super->os_ninodes)) //no free inode
		return -ENOSPC;
	if (copy_from_user(sym_oi, &zerofill, OSPFS_INODESIZE))
		return -EIO;
	
	ospfs_direntry_t *od = create_blank_direntry(dir_oi);
	if (IS_ERR(od))
		return PTR_ERR(od);

	if(copy_from_user(od, &entry_ino, 4)) //inode number
		return -EIO;
	if(copy_from_user(od+4, dentry->d_name.name, dentry->d_name.len)) //name
		return -EIO;

	ospfs_symlink_inode_t temp;
	temp.oi_size = strlen(symname);
	temp.oi_ftype = OSPFS_FTYPE_SYMLINK;
	temp.oi_nlink = 1;
	strcpy(temp.oi_symlink, symname);
	
	if(copy_from_user(sym_oi, &temp, OSPFS_INODESIZE))
		return -EIO;

	//return -EIO; // Replace this lines
//lab3 code ends

	/* Execute this code after your function has successfully created the
	   file.  Set entry_ino to the created file's inode number before
	   getting here. */
	{
		struct inode *i = ospfs_mk_linux_inode(dir->i_sb, entry_ino);
		if (!i)
			return -ENOMEM;
		d_instantiate(dentry, i);
		return 0;
	}
}


// ospfs_follow_link(dentry, nd)
//   Linux calls this function to follow a symbolic link.
//   It is the ospfs_symlink_inode_ops.follow_link callback.
//
//   Inputs: dentry -- the symbolic link's directory entry
//           nd     -- to be filled in with the symbolic link's destination
//
//   Exercise: Expand this function to handle conditional symlinks.  Conditional
//   symlinks will always be created by users in the following form
//     root?/path/1:/path/2.
//   (hint: Should the given form be changed in any way to make this method
//   easier?  With which character do most functions expect C strings to end?)

static void *
ospfs_follow_link(struct dentry *dentry, struct nameidata *nd)
{
	ospfs_symlink_inode_t *oi =
		(ospfs_symlink_inode_t *) ospfs_inode(dentry->d_inode->i_ino);
	// Exercise: Your code here.
//lab3 code begins
	if (!oi->oi_size || (oi->oi_ftype != OSPFS_FTYPE_SYMLINK))
		return ERR_PTR(-EINVAL);

	char *uid, *dest1, *dest2, *str, *nt, *link;
	uint32_t uidl, dest1l, dest2l, strl;
	uid = dest1 = dest2 = str = NULL;
	char *root = "root";

	strcpy(str, oi->oi_symlink);
	strl = strlen(str);
	
	uid  = str;
	dest1 = strchr(str, '?');
	if (!dest1) // simple symbolic link
		nd_set_link(nd, oi->oi_symlink);

	else {
		dest2 = strchr(str, ':');
		nt = strchr(str, '\0');
		
		uidl = (uint32_t)(dest1 - uid);

		dest1++;
		dest1l = (uint32_t)(dest2 - dest1);
	
		dest2++;
		dest2l = (uint32_t)(nt - dest2);

		if (strcmp(uid, root)) //unknown user id
			return ERR_PTR(-EINVAL);

		if (current_euid() == 0) //root
			memcpy(link, dest1, dest1l);
		else	
			memcpy(link, dest2, dest2l);
		strcat (link, nt);
		nd_set_link(nd, link);
	}
	//nd_set_link(nd, oi->oi_symlink
//lab3 code ends
	return (void *) 0;
}


// Define the file system operations structures mentioned above.

static struct file_system_type ospfs_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "ospfs",
	.get_sb		= ospfs_get_sb,
	.kill_sb	= kill_anon_super
};

static struct inode_operations ospfs_reg_inode_ops = {
	.setattr	= ospfs_notify_change
};

static struct file_operations ospfs_reg_file_ops = {
	.llseek		= generic_file_llseek,
	.read		= ospfs_read,
	.write		= ospfs_write
};

static struct inode_operations ospfs_dir_inode_ops = {
	.lookup		= ospfs_dir_lookup,
	.link		= ospfs_link,
	.unlink		= ospfs_unlink,
	.create		= ospfs_create,
	.symlink	= ospfs_symlink
};

static struct file_operations ospfs_dir_file_ops = {
	.read		= generic_read_dir,
	.readdir	= ospfs_dir_readdir
};

static struct inode_operations ospfs_symlink_inode_ops = {
	.readlink	= generic_readlink,
	.follow_link	= ospfs_follow_link
};

static struct dentry_operations ospfs_dentry_ops = {
	.d_delete	= ospfs_delete_dentry
};

static struct super_operations ospfs_superblock_ops = {
};


// Functions used to hook the module into the kernel!

static int __init init_ospfs_fs(void)
{
	eprintk("Loading ospfs module...\n");
	return register_filesystem(&ospfs_fs_type);
}

static void __exit exit_ospfs_fs(void)
{
	unregister_filesystem(&ospfs_fs_type);
	eprintk("Unloading ospfs module\n");
}

module_init(init_ospfs_fs)
module_exit(exit_ospfs_fs)

// Information about the module
MODULE_AUTHOR("Skeletor");
MODULE_DESCRIPTION("OSPFS");
MODULE_LICENSE("GPL");
