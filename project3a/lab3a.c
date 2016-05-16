#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include <fcntl.h>
#include <inttypes.h>
#include <math.h>

#define BUFFER_SIZE 4096

#define SUPERBLOCK_SIZE 1024
#define SUPERBLOCK_OFFSET 1024

int ifd = 0;
// TODO: test %x output for uint16, 32 and 64
int num_groups;
uint32_t block_size;

struct ext2_super_block {
  uint32_t  s_inodes_count;   /* Inodes count */
  uint32_t  s_blocks_count;   /* Blocks count */
  uint32_t  s_r_blocks_count; /* Reserved blocks count */
  uint32_t  s_free_blocks_count;  /* Free blocks count */
  uint32_t  s_free_inodes_count;  /* Free inodes count */
  uint32_t  s_first_data_block; /* First Data Block */
  uint32_t  s_log_block_size; /* Block size */
  uint32_t  s_log_frag_size;  /* Fragment size */
  uint32_t  s_blocks_per_group; /* # Blocks per group */
  uint32_t  s_frags_per_group;  /* # Fragments per group */
  uint32_t  s_inodes_per_group; /* # Inodes per group */
  uint32_t  s_mtime;    /* Mount time */
  uint32_t  s_wtime;    /* Write time */
  uint16_t  s_mnt_count;    /* Mount count */
  uint16_t  s_max_mnt_count;  /* Maximal mount count */
  uint16_t  s_magic;    /* Magic signature */
  uint16_t  s_state;    /* File system state */
  uint8_t   all_the_rest[SUPERBLOCK_SIZE - 60];
} superblock;

struct ext2_group_desc
{
  uint32_t  bg_block_bitmap;    /* Blocks bitmap block */
  uint32_t  bg_inode_bitmap;    /* Inodes bitmap block */
  uint32_t  bg_inode_table;   /* Inodes table block */
  uint16_t  bg_free_blocks_count; /* Free blocks count */
  uint16_t  bg_free_inodes_count; /* Free inodes count */
  uint16_t  bg_used_dirs_count; /* Directories count */
  uint16_t  bg_pad;
  uint32_t  bg_reserved[3];
};

struct ext2_group_desc * group_desc_table;

// write the superblock
int write_superblock() {
  int ret = pread(ifd, &superblock, SUPERBLOCK_SIZE, SUPERBLOCK_OFFSET);
  if (ret < SUPERBLOCK_SIZE) {
    perror("Unexpected superblock read");
    exit(1);
  }
  
  // special handing for block_size
  block_size = 1024 << superblock.s_log_block_size;
  
  // special handling for fragment_size
  int32_t fragment_shift = (int32_t) superblock.s_log_frag_size;
  uint32_t fragment_size = 0;
  if (fragment_shift > 0)
    fragment_size = 1024 << fragment_shift;
  else
    fragment_size = 1024 >> -fragment_shift;

  int ofd = creat("my-super.csv", 0666);
  if (ofd < 0) {
    perror("Unable to open output file");
    exit(1);
  }
  char output_buffer[BUFFER_SIZE] = {0};
  ret = sprintf(output_buffer, "%x,%"PRIu32",%"PRIu32",%"PRIu32",%"PRIu32",%"PRIu32",%"PRIu32",%"PRIu32",%"PRIu32"\n", 
    superblock.s_magic, superblock.s_inodes_count, superblock.s_blocks_count, 
    block_size, fragment_size, superblock.s_blocks_per_group, 
    superblock.s_inodes_per_group, superblock.s_frags_per_group, superblock.s_first_data_block);
  return write(ofd, output_buffer, ret);
}

// write the group descriptor
int write_group_descriptor() {
  int start = superblock.s_first_data_block + 1;
  num_groups = ceil((float)superblock.s_blocks_count / (float)superblock.s_blocks_per_group);
  group_desc_table = malloc(sizeof(struct ext2_group_desc) * num_groups);
  
  int size = sizeof(struct ext2_group_desc) * num_groups;
  int ret = pread(ifd, group_desc_table, size, start * block_size);
  if (ret < size) {
    perror("Unexpected group_desc_table read");
    exit(1);
  }

  int ofd = creat("my-group.csv", 0666);
  if (ofd < 0) {
    perror("Unable to open output file");
    exit(1);
  }
  char output_buffer[BUFFER_SIZE] = {0};
  int i = 0;
  ret = 0;
  for (i = 0; i < num_groups; i++) {
    uint32_t blocks_contained = superblock.s_blocks_per_group;
    if (i == num_groups - 1) {
      blocks_contained = superblock.s_blocks_count % superblock.s_blocks_per_group;
    }
    ret = sprintf(output_buffer, "%"PRIu32",%"PRIu32",%"PRIu32",%"PRIu32",%x,%x,%x\n", 
      blocks_contained, group_desc_table[i].bg_free_blocks_count, group_desc_table[i].bg_free_inodes_count,
      group_desc_table[i].bg_used_dirs_count, group_desc_table[i].bg_inode_bitmap, group_desc_table[i].bg_block_bitmap, 
      group_desc_table[i].bg_inode_table);
    ret += write(ofd, output_buffer, ret);
  }

  return ret;
}

// write the bitmap entries
int write_bitmap_entry() {
  int ofd = creat("my-bitmap.csv", 0666);
  if (ofd < 0) {
    perror("Unable to open output file");
    exit(1);
  }
  char output_buffer[BUFFER_SIZE] = {0};
  int i = 0, j = 0, k = 0, ret = 0, inode_idx = 1, inode_upper_bound = 0, block_upper_bound = 0, done = 0, block_idx = 1;
  uint8_t *inode_bitmap_block = (uint8_t *)malloc(sizeof(uint8_t) * block_size);
  uint8_t *block_bitmap_block = (uint8_t *)malloc(sizeof(uint8_t) * block_size);

  for (i = 0; i < num_groups; i++) {
    block_upper_bound += superblock.s_blocks_per_group;
    inode_upper_bound += superblock.s_inodes_per_group;
    if (i == num_groups - 1) {
      block_upper_bound = superblock.s_blocks_count;
      inode_upper_bound = superblock.s_inodes_count;
    }

    done = 0;
    pread(ifd, block_bitmap_block, block_size, group_desc_table[i].bg_block_bitmap * block_size);
    for (j = 0; j < block_size; j++) {
      if (done) {
        break;
      }
      for (k = 0; k < 8; k++) {
        if (block_idx <= block_upper_bound) {
          if ((block_bitmap_block[j] & (1 << k)) == 0) {
            ret = sprintf(output_buffer, "%x,%"PRIu32"\n", 
              group_desc_table[i].bg_block_bitmap, block_idx);
            write(ofd, output_buffer, ret);
          }
          block_idx ++;
        } else {
          done = 1;
          break;
        }
      }
    }
    
    done = 0;
    pread(ifd, inode_bitmap_block, block_size, group_desc_table[i].bg_inode_bitmap * block_size);
    for (j = 0; j < block_size; j++) {
      if (done) {
        break;
      }
      for (k = 0; k < 8; k++) {
        if (inode_idx <= inode_upper_bound) {
          if ((inode_bitmap_block[j] & (1 << k)) == 0) {
            ret = sprintf(output_buffer, "%x,%"PRIu32"\n", 
              group_desc_table[i].bg_inode_bitmap, inode_idx);
            write(ofd, output_buffer, ret);
          }
          inode_idx ++;
        } else {
          done = 1;
          break;
        }
      }
    }
  }
  free(inode_bitmap_block);
  free(block_bitmap_block);
  return 1;
}

int main(int argc, char **argv) {
  if (argc > 2) {
    perror("Unexpected amount of arguments");
    exit(1);
  }

  ifd = open(argv[1], O_RDONLY);
  if (ifd < 0) {
    perror("Unable to open image");
    exit(1);
  }
  
  write_superblock();
  write_group_descriptor();
  write_bitmap_entry();

  return 0;
}