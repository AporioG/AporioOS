#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define EXT2_BASE_LBA 140

struct ext2_super {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size; // 0 -> 1024 байта на блок
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
} __attribute__((packed));

struct ext2_bg_desc {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint32_t bg_reserved[3];
} __attribute__((packed));

struct ext2_in {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];
    uint8_t  i_pad[28]; // Выравнивание до 128 байт
} __attribute__((packed));

struct ext2_dir {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[];
} __attribute__((packed));

int main(int argc, char **argv) {
    if (argc < 4) return 1;
    FILE *fimg = fopen(argv[1], "r+b");
    FILE *fapp = fopen(argv[2], "rb");
    if (!fimg || !fapp) return 2;

    fseek(fapp, 0, SEEK_END);
    long app_size = ftell(fapp);
    fseek(fapp, 0, SEEK_SET);

    uint8_t *app_buf = malloc(app_size);
    fread(app_buf, 1, app_size, fapp);
    fclose(fapp);

    // 1. Суперблок ext2 (Block 1 -> LBA 142)
    struct ext2_super es;
    memset(&es, 0, sizeof(es));
    es.s_inodes_count = 32;
    es.s_blocks_count = 256;
    es.s_free_blocks_count = 230;
    es.s_free_inodes_count = 29;
    es.s_first_data_block = 1;
    es.s_log_block_size = 0;
    es.s_blocks_per_group = 256;
    es.s_inodes_per_group = 32;
    es.s_magic = 0xEF53;
    es.s_state = 1;

    fseek(fimg, (EXT2_BASE_LBA + 2) * 512, SEEK_SET);
    fwrite(&es, 1, sizeof(es), fimg);

    // 2. Дескриптор группы (Block 2 -> LBA 144)
    struct ext2_bg_desc bg;
    memset(&bg, 0, sizeof(bg));
    bg.bg_block_bitmap = 3;
    bg.bg_inode_bitmap = 4;
    bg.bg_inode_table  = 5; // Block 5 -> LBA 150
    bg.bg_free_blocks_count = 230;
    bg.bg_free_inodes_count = 29;
    bg.bg_used_dirs_count = 1;

    fseek(fimg, (EXT2_BASE_LBA + 4) * 512, SEEK_SET);
    fwrite(&bg, 1, sizeof(bg), fimg);

    // 3. Таблица инодов (LBA 150)
    // Инод 2: Корневой каталог (offset 128)
    struct ext2_in root_in;
    memset(&root_in, 0, sizeof(root_in));
    root_in.i_mode = 0x41ED;
    root_in.i_size = 1024;
    root_in.i_links_count = 2;
    root_in.i_block[0] = 10; // Block 10 -> LBA 160

    // Инод 11: hello.txt (offset 1280)
    struct ext2_in hello_in;
    memset(&hello_in, 0, sizeof(hello_in));
    hello_in.i_mode = 0x81A4;
    hello_in.i_size = 38;
    hello_in.i_links_count = 1;
    hello_in.i_block[0] = 11; // Block 11 -> LBA 162

    // Инод 12: elfapp (offset 1408)
    struct ext2_in elf_in;
    memset(&elf_in, 0, sizeof(elf_in));
    elf_in.i_mode = 0x81ED;
    elf_in.i_size = (uint32_t)app_size;
    elf_in.i_links_count = 1;
    uint32_t num_blocks = (app_size + 1023) / 1024;
    for (uint32_t b = 0; b < num_blocks && b < 12; b++) {
        elf_in.i_block[b] = 12 + b;
    }

    fseek(fimg, (EXT2_BASE_LBA + 10) * 512 + 128, SEEK_SET);
    fwrite(&root_in, 1, sizeof(root_in), fimg);
    fseek(fimg, (EXT2_BASE_LBA + 10) * 512 + 1280, SEEK_SET);
    fwrite(&hello_in, 1, sizeof(hello_in), fimg);
    fseek(fimg, (EXT2_BASE_LBA + 10) * 512 + 1408, SEEK_SET);
    fwrite(&elf_in, 1, sizeof(elf_in), fimg);

    // 4. Корневая директория (LBA 160)
    uint8_t dir_blk[1024];
    memset(dir_blk, 0, sizeof(dir_blk));
    uint8_t *p = dir_blk;

    struct ext2_dir *d1 = (struct ext2_dir *)p;
    d1->inode = 2; d1->rec_len = 12; d1->name_len = 1; d1->file_type = 2;
    memcpy(p + 8, ".", 1);
    p += 12;

    struct ext2_dir *d2 = (struct ext2_dir *)p;
    d2->inode = 2; d2->rec_len = 12; d2->name_len = 2; d2->file_type = 2;
    memcpy(p + 8, "..", 2);
    p += 12;

    struct ext2_dir *d3 = (struct ext2_dir *)p;
    d3->inode = 11; d3->rec_len = 20; d3->name_len = 9; d3->file_type = 1;
    memcpy(p + 8, "hello.txt", 9);
    p += 20;

    struct ext2_dir *d4 = (struct ext2_dir *)p;
    d4->inode = 12;
    d4->rec_len = 1024 - (p - dir_blk);
    d4->name_len = strlen(argv[3]);
    d4->file_type = 1;
    memcpy(p + 8, argv[3], d4->name_len);

    fseek(fimg, (EXT2_BASE_LBA + 20) * 512, SEEK_SET);
    fwrite(dir_blk, 1, sizeof(dir_blk), fimg);

    // 5. Данные hello.txt (LBA 162)
    const char *hello_txt = "Aporia OS ext2 VFS subsystem verified!";
    uint8_t hbuf[1024] = {0};
    memcpy(hbuf, hello_txt, strlen(hello_txt));
    fseek(fimg, (EXT2_BASE_LBA + 22) * 512, SEEK_SET);
    fwrite(hbuf, 1, sizeof(hbuf), fimg);

    // 6. Данные elfapp (Block 12+ -> LBA 164+)
    fseek(fimg, (EXT2_BASE_LBA + 24) * 512, SEEK_SET);
    fwrite(app_buf, 1, app_size, fimg);

    free(app_buf);
    fclose(fimg);
    return 0;
}
