#define VGA_ADDR ((volatile char *)0xB8000)
#define VGA_COLS 80
#define VGA_ROWS 25
#define VGA_ATTR_DEFAULT 0x07
#define VGA_ATTR_PROMPT  0x0B
#define VGA_ATTR_BANNER  0x0A
#define VGA_ATTR_PANIC   0x4F

#define HEAP_START 0x100000
#define HEAP_SIZE  0x200000
#define MAX_TASKS  4
#define STACK_SIZE 2048

#define TASK_STATE_DEAD    0
#define TASK_STATE_RUNNING 1
#define USER_FRAMES_BASE   0x200000

#define TERM_X0 192
#define TERM_Y0 184

typedef unsigned char  uint8_t;
typedef signed char    int8_t;
typedef unsigned short uint16_t;
typedef signed short   int16_t;
typedef unsigned int   uint32_t;
typedef signed int     int32_t;

// --- 1. Аппаратные порты x86 ---
static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret; asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port)); return ret;
}
static inline void outw(uint16_t port, uint16_t val) {
    asm volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint16_t inw(uint16_t port) {
    uint16_t ret; asm volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port)); return ret;
}
static inline void outl(uint16_t port, uint32_t val) {
    asm volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t ret; asm volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port)); return ret;
}
static inline void io_wait(void) { outb(0x80, 0); }

// --- 2. Строковые утилиты ---
static int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

static int strncmp(const char *s1, const char *s2, int n) {
    for (int i = 0; i < n; i++) {
        if (s1[i] != s2[i] || s1[i] == '\0') return s1[i] - s2[i];
    }
    return 0;
}

static uint32_t parse_dec(const char *str) {
    uint32_t res = 0;
    while (*str >= '0' && *str <= '9') {
        res = res * 10 + (*str - '0');
        str++;
    }
    return res;
}

// --- 3. CMOS RTC низкоуровневые функции ---
static uint8_t cmos_read(uint8_t reg) {
    outb(0x70, reg | 0x80);
    return inb(0x71);
}
static int cmos_is_updating(void) {
    outb(0x70, 0x0A);
    return inb(0x71) & 0x80;
}
#define BCD_TO_BIN(val) (((val) & 0x0F) + (((val) >> 4) * 10))

// --- 4. Сканирование шины PCI ---
static uint32_t pci_read_config32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (uint32_t)((1U << 31) | ((uint32_t)bus << 16) |
                                  ((uint32_t)slot << 11) | ((uint32_t)func << 8) |
                                  (offset & 0xFC));
    outl(0xCF8, address);
    return inl(0xCFC);
}

static uint16_t pci_read_config16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t val = pci_read_config32(bus, slot, func, offset);
    return (uint16_t)((val >> ((offset & 2) * 8)) & 0xFFFF);
}

struct pci_dev {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
};

#define MAX_PCI_DEVS 16
static struct pci_dev pci_devices[MAX_PCI_DEVS];
static int pci_dev_count = 0;

static void pci_scan(void) {
    pci_dev_count = 0;
    for (uint16_t bus = 0; bus < 8; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint16_t vendor = pci_read_config16((uint8_t)bus, slot, 0, 0x00);
            if (vendor == 0xFFFF || vendor == 0x0000) continue;

            uint32_t htype_reg = pci_read_config32((uint8_t)bus, slot, 0, 0x0C);
            uint8_t header_type = (htype_reg >> 16) & 0xFF;
            uint8_t max_func = (header_type & 0x80) ? 8 : 1;

            for (uint8_t func = 0; func < max_func; func++) {
                uint16_t v = pci_read_config16((uint8_t)bus, slot, func, 0x00);
                if (v == 0xFFFF || v == 0x0000) continue;
                if (pci_dev_count >= MAX_PCI_DEVS) return;

                uint16_t dev_id = pci_read_config16((uint8_t)bus, slot, func, 0x02);
                uint32_t class_reg = pci_read_config32((uint8_t)bus, slot, func, 0x08);

                pci_devices[pci_dev_count].bus = (uint8_t)bus;
                pci_devices[pci_dev_count].slot = slot;
                pci_devices[pci_dev_count].func = func;
                pci_devices[pci_dev_count].vendor_id = v;
                pci_devices[pci_dev_count].device_id = dev_id;
                pci_devices[pci_dev_count].class_code = (class_reg >> 24) & 0xFF;
                pci_devices[pci_dev_count].subclass   = (class_reg >> 16) & 0xFF;
                pci_dev_count++;
            }
        }
    }
}

// --- 5. VBE Framebuffer Контекст ---
static uint32_t fb_phys;
static uint16_t fb_pitch;
static uint16_t fb_width;
static uint16_t fb_height;
static uint8_t  fb_bpp;
static volatile uint8_t *fb_base;

static const uint32_t vga_rgb[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF
};

static inline void put_pixel(int x, int y, uint32_t color) {
    if (x < 0 || x >= fb_width || y < 0 || y >= fb_height) return;
    if (fb_bpp == 32) {
        *(volatile uint32_t *)(fb_base + y * fb_pitch + x * 4) = color;
    } else if (fb_bpp == 24) {
        volatile uint8_t *p = fb_base + y * fb_pitch + x * 3;
        p[0] = color & 0xFF;
        p[1] = (color >> 8) & 0xFF;
        p[2] = (color >> 16) & 0xFF;
    }
}

static inline uint32_t get_pixel(int x, int y) {
    if (x < 0 || x >= fb_width || y < 0 || y >= fb_height) return 0;
    if (fb_bpp == 32) {
        return *(volatile uint32_t *)(fb_base + y * fb_pitch + x * 4);
    } else if (fb_bpp == 24) {
        volatile uint8_t *p = fb_base + y * fb_pitch + x * 3;
        return p[0] | (p[1] << 8) | (p[2] << 16);
    }
    return 0;
}

static const uint8_t font8x8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 32 ' '
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, // 33 !
    {0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00}, // 34 "
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, // 35 #
    {0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00}, // 36 $
    {0x00,0x63,0x66,0x0C,0x18,0x33,0x63,0x00}, // 37 %
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, // 38 &
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, // 39 '
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, // 40 (
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, // 41 )
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // 42 *
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, // 43 +
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, // 44 ,
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, // 45 -
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, // 46 .
    {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00}, // 47 /
    {0x7C,0xC6,0xCE,0xD6,0xE6,0xC6,0x7C,0x00}, // 48 0
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, // 49 1
    {0x7C,0xC6,0x06,0x1C,0x30,0x66,0xFE,0x00}, // 50 2
    {0x7C,0xC6,0x06,0x3C,0x06,0xC6,0x7C,0x00}, // 51 3
    {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x1E,0x00}, // 52 4
    {0xFE,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0x00}, // 53 5
    {0x7C,0xC6,0xC0,0xFC,0xC6,0xC6,0x7C,0x00}, // 54 6
    {0xFE,0x06,0x0C,0x18,0x30,0x30,0x30,0x00}, // 55 7
    {0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x00}, // 56 8
    {0x7C,0xC6,0xC6,0x7E,0x06,0xC6,0x7C,0x00}, // 57 9
    {0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00}, // 58 :
    {0x00,0x18,0x18,0x00,0x18,0x18,0x30,0x00}, // 59 ;
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, // 60 <
    {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00}, // 61 =
    {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00}, // 62 >
    {0x7C,0xC6,0x0C,0x18,0x18,0x00,0x18,0x00}, // 63 ?
    {0x7C,0xC6,0xDE,0xDE,0xDC,0xC0,0x7C,0x00}, // 64 @
    {0x38,0x6C,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, // 65 A
    {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x00}, // 66 B
    {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00}, // 67 C
    {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x00}, // 68 D
    {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00}, // 69 E
    {0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x00}, // 70 F
    {0x3C,0x66,0xC0,0xC0,0xCE,0x66,0x3E,0x00}, // 71 G
    {0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, // 72 H
    {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, // 73 I
    {0x1E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00}, // 74 J
    {0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00}, // 75 K
    {0xF0,0x60,0x60,0x60,0x62,0x66,0xFE,0x00}, // 76 L
    {0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0x00}, // 77 M
    {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00}, // 78 N
    {0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, // 79 O
    {0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00}, // 80 P
    {0x7C,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x0E}, // 81 Q
    {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00}, // 82 R
    {0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00}, // 83 S
    {0x7E,0x7E,0x18,0x18,0x18,0x18,0x3C,0x00}, // 84 T
    {0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, // 85 U
    {0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00}, // 86 V
    {0xC6,0xC6,0xD6,0xFE,0xFE,0xEE,0xC6,0x00}, // 87 W
    {0xC6,0x6C,0x38,0x38,0x6C,0xC6,0xC6,0x00}, // 88 X
    {0x66,0x66,0x66,0x3C,0x18,0x18,0x3C,0x00}, // 89 Y
    {0xFE,0xC6,0x0C,0x18,0x30,0x63,0xFE,0x00}, // 90 Z
    {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00}, // 91 [
    {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00}, /* 92 backslash */
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, // 93 ]
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00}, // 94 ^
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, // 95 _
    {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, // 96 `
    {0x00,0x00,0x78,0x0C,0x7C,0xCC,0x76,0x00}, // 97 a
    {0xE0,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00}, // 98 b
    {0x00,0x00,0x7C,0xC6,0xC0,0xC6,0x7C,0x00}, // 99 c
    {0x1C,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00}, // 100 d
    {0x00,0x00,0x7C,0xC6,0xFE,0xC0,0x7C,0x00}, // 101 e
    {0x1C,0x36,0x30,0x7C,0x30,0x30,0x78,0x00}, // 102 f
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0xF8}, // 103 g
    {0xE0,0x60,0x6C,0x76,0x66,0x66,0xE6,0x00}, // 104 h
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00}, // 105 i
    {0x06,0x00,0x06,0x06,0x06,0x66,0x66,0x3C}, // 106 j
    {0xE0,0x60,0x66,0x6C,0x78,0x6C,0xE6,0x00}, // 107 k
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, // 108 l
    {0x00,0x00,0xEC,0xFE,0xD6,0xD6,0xD6,0x00}, // 109 m
    {0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x00}, // 110 n
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0x00}, // 111 o
    {0x00,0x00,0xDC,0x66,0x66,0x7C,0x60,0xF0}, // 112 p
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x1E}, // 113 q
    {0x00,0x00,0xDC,0x76,0x60,0x60,0xF0,0x00}, // 114 r
    {0x00,0x00,0x7C,0xC0,0x7C,0x06,0x7C,0x00}, // 115 s
    {0x30,0x30,0xFC,0x30,0x30,0x34,0x18,0x00}, // 116 t
    {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x76,0x00}, // 117 u
    {0x00,0x00,0xC6,0xC6,0xC6,0x6C,0x38,0x00}, // 118 v
    {0x00,0x00,0xC6,0xD6,0xFE,0xFE,0x6C,0x00}, // 119 w
    {0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00}, // 120 x
    {0x00,0x00,0xC6,0xC6,0xC6,0x7E,0x06,0xFC}, // 121 y
    {0x00,0x00,0x7E,0x4C,0x18,0x32,0x7E,0x00}, // 122 z
    {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00}, // 123 {
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, // 124 |
    {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00}, // 125 }
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00}  // 126 ~
};

// Высокоскоростной растеризатор символов с оптимизацией пробелов (без bit-shift overhead)
static void draw_char_8x16(int px, int py, char c, uint32_t fg, uint32_t bg) {
    if (px < 0 || px + 8 > fb_width || py < 0 || py + 16 > fb_height) return;
    if (c < 32 || c > 126) c = ' ';
    if (fb_bpp == 32) {
        volatile uint8_t *row_ptr = fb_base + py * fb_pitch + (px * 4);
        if (c == ' ') {
            for (int r = 0; r < 8; r++) {
                volatile uint32_t *p1 = (volatile uint32_t *)row_ptr;
                volatile uint32_t *p2 = (volatile uint32_t *)(row_ptr + fb_pitch);
                p1[0] = bg; p1[1] = bg; p1[2] = bg; p1[3] = bg;
                p1[4] = bg; p1[5] = bg; p1[6] = bg; p1[7] = bg;
                p2[0] = bg; p2[1] = bg; p2[2] = bg; p2[3] = bg;
                p2[4] = bg; p2[5] = bg; p2[6] = bg; p2[7] = bg;
                row_ptr += fb_pitch * 2;
            }
            return;
        }
        const uint8_t *glyph = font8x8[c - 32];
        for (int r = 0; r < 8; r++) {
            uint8_t row = glyph[r];
            volatile uint32_t *p1 = (volatile uint32_t *)row_ptr;
            volatile uint32_t *p2 = (volatile uint32_t *)(row_ptr + fb_pitch);
            for (int col = 0; col < 8; col++) {
                uint32_t val = (row & (0x80 >> col)) ? fg : bg;
                p1[col] = val;
                p2[col] = val;
            }
            row_ptr += fb_pitch * 2;
        }
    } else {
        const uint8_t *glyph = font8x8[c - 32];
        for (int r = 0; r < 8; r++) {
            uint8_t row = glyph[r];
            for (int sub = 0; sub < 2; sub++) {
                int y = py + r * 2 + sub;
                for (int col = 0; col < 8; col++) {
                    put_pixel(px + col, y, (row & (0x80 >> col)) ? fg : bg);
                }
            }
        }
    }
}

static void fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > fb_width) w = fb_width - x;
    if (y + h > fb_height) h = fb_height - y;
    if (w <= 0 || h <= 0) return;
    for (int j = y; j < y + h; j++) {
        if (fb_bpp == 32) {
            volatile uint32_t *row = (volatile uint32_t *)(fb_base + j * fb_pitch + x * 4);
            for (int i = 0; i < w; i++) row[i] = color;
        } else {
            for (int i = x; i < x + w; i++) put_pixel(i, j, color);
        }
    }
}

// Позиция перемещаемого окна терминала (Drag & Drop)
static int win_x = TERM_X0;
static int win_y = TERM_Y0;

// Отрисовка декораций окна без стирания клиентской области (устранение стробоскопического мерцания)
static void render_window_frame(int wx, int wy) {
    fill_rect(wx - 4, wy - 24, 640 + 8, 24, 0x282E3D);
    fill_rect(wx - 4, wy + 400, 640 + 8, 4, 0x282E3D);
    fill_rect(wx - 4, wy, 4, 400, 0x282E3D);
    fill_rect(wx + 640, wy, 4, 400, 0x282E3D);
    fill_rect(wx, wy - 20, 640, 16, 0x3A4459);
    const char *title = "Aporia OS v1.0.0 Workstation [VESA VBE 1024x768x32bpp]";
    for (int i = 0; title[i]; i++) {
        draw_char_8x16(wx + 12 + i * 8, wy - 20, title[i], 0xFFFFFF, 0x3A4459);
    }
}

// Очистка только открывшихся участков обоев при сдвиге (Damage Rectangles)
static void clear_exposed_rects(int ox, int oy, int nx, int ny, int w, int h, uint32_t bg_col) {
    if (ox + w <= nx || nx + w <= ox || oy + h <= ny || ny + h <= oy) {
        fill_rect(ox, oy, w, h, bg_col);
        return;
    }
    if (nx >= ox && ny >= oy) {
        if (ny > oy) fill_rect(ox, oy, w, ny - oy, bg_col);
        if (nx > ox) fill_rect(ox, ny, nx - ox, oy + h - ny, bg_col);
    } else if (nx >= ox && ny < oy) {
        if (oy > ny) fill_rect(ox, ny + h, w, oy - ny, bg_col);
        if (nx > ox) fill_rect(ox, oy, nx - ox, ny + h - oy, bg_col);
    } else if (nx < ox && ny >= oy) {
        if (ny > oy) fill_rect(ox, oy, w, ny - oy, bg_col);
        if (ox > nx) fill_rect(nx + w, ny, ox - nx, oy + h - ny, bg_col);
    } else if (nx < ox && ny < oy) {
        if (oy > ny) fill_rect(ox, ny + h, w, oy - ny, bg_col);
        if (ox > nx) fill_rect(nx + w, oy, ox - nx, ny + h - oy, bg_col);
    }
}

static void render_desktop(void) {
    fill_rect(0, 0, fb_width, fb_height, 0x141923);
    render_window_frame(win_x, win_y);
}

// --- 6. Виртуальный терминал ---
static char vga_char_buf[VGA_ROWS][VGA_COLS];
static uint8_t vga_attr_buf[VGA_ROWS][VGA_COLS];
static int cursor_x = 0;
static int cursor_y = 1;
static int editor_active = 0;

static void vga_sync_cell(int x, int y) {
    char c = vga_char_buf[y][x];
    uint8_t attr = vga_attr_buf[y][x];
    uint32_t fg = vga_rgb[attr & 0x0F];
    uint32_t bg = vga_rgb[(attr >> 4) & 0x0F];
    draw_char_8x16(win_x + x * 8, win_y + y * 16, c, fg, bg);
}

static void render_header_menu(void);

static void vga_clear(void) {
    for (int y = 0; y < VGA_ROWS; y++) {
        for (int x = 0; x < VGA_COLS; x++) {
            vga_char_buf[y][x] = ' ';
            vga_attr_buf[y][x] = VGA_ATTR_DEFAULT;
            vga_sync_cell(x, y);
        }
    }
    cursor_x = 0;
    cursor_y = 1;
    render_header_menu();
}

static void vga_scroll(void) {
    for (int row = 2; row < 21; row++) {
        for (int col = 0; col < VGA_COLS; col++) {
            vga_char_buf[row - 1][col] = vga_char_buf[row][col];
            vga_attr_buf[row - 1][col] = vga_attr_buf[row][col];
            vga_sync_cell(col, row - 1);
        }
    }
    for (int col = 0; col < VGA_COLS; col++) {
        vga_char_buf[20][col] = ' ';
        vga_attr_buf[20][col] = VGA_ATTR_DEFAULT;
        vga_sync_cell(col, 20);
    }
    cursor_y = 20;
}

static void vga_putc_color(char c, uint8_t color) {
    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
    } else if (c == '\b') {
        if (cursor_x > 0) {
            cursor_x--;
            vga_char_buf[cursor_y][cursor_x] = ' ';
            vga_attr_buf[cursor_y][cursor_x] = VGA_ATTR_DEFAULT;
            vga_sync_cell(cursor_x, cursor_y);
        }
    } else {
        vga_char_buf[cursor_y][cursor_x] = c;
        vga_attr_buf[cursor_y][cursor_x] = color;
        vga_sync_cell(cursor_x, cursor_y);
        cursor_x++;
        if (cursor_x >= VGA_COLS) {
            cursor_x = 0;
            cursor_y++;
        }
    }

    if (!editor_active && cursor_y >= 21) {
        vga_scroll();
    }
}

static void vga_puts(const char *str, uint8_t color) {
    for (int i = 0; str[i] != '\0'; i++) vga_putc_color(str[i], color);
}

static void vga_putc_at(int x, int y, char c, uint8_t color) {
    if (x < 0 || x >= VGA_COLS || y < 0 || y >= VGA_ROWS) return;
    vga_char_buf[y][x] = c;
    vga_attr_buf[y][x] = color;
    vga_sync_cell(x, y);
}

static void vga_puts_at(int x, int y, const char *str, uint8_t color) {
    for (int i = 0; str[i] != '\0'; i++) vga_putc_at(x + i, y, str[i], color);
}

static void vga_print_hex8(uint8_t val, uint8_t color) {
    const char hex[] = "0123456789ABCDEF";
    vga_putc_color(hex[(val >> 4) & 0x0F], color);
    vga_putc_color(hex[val & 0x0F], color);
}

static void vga_print_hex32(uint32_t val, uint8_t color) {
    const char hex[] = "0123456789ABCDEF";
    vga_puts("0x", color);
    for (int i = 28; i >= 0; i -= 4) vga_putc_color(hex[(val >> i) & 0x0F], color);
}

static void vga_print_dec_at(int x, int y, uint32_t val, uint8_t color) {
    char buf[12];
    int idx = 0;
    if (val == 0) buf[idx++] = '0';
    while (val > 0) {
        buf[idx++] = '0' + (val % 10);
        val /= 10;
    }
    for (int i = idx - 1; i >= 0; i--) vga_putc_at(x++, y, buf[i], color);
}

static void vga_print_dec(uint32_t val, uint8_t color) {
    if (val == 0) { vga_putc_color('0', color); return; }
    char buf[12]; int idx = 0;
    while (val > 0) { buf[idx++] = '0' + (val % 10); val /= 10; }
    for (int i = idx - 1; i >= 0; i--) vga_putc_color(buf[i], color);
}

// --- 7. Безопасный ATA PIO Драйвер ---
#define ATA_DATA       0x1F0
#define ATA_ERROR      0x1F1
#define ATA_SECCOUNT   0x1F2
#define ATA_LBA_LO     0x1F3
#define ATA_LBA_MID    0x1F4
#define ATA_LBA_HI     0x1F5
#define ATA_DRIVE_HEAD 0x1F6
#define ATA_STATUS_CMD 0x1F7

static void ata_delay_400ns(void) {
    for (int i = 0; i < 4; i++) inb(ATA_STATUS_CMD);
}

int ata_read_sector(uint32_t lba, uint8_t *buffer) {
    uint32_t timeout = 50000;
    while ((inb(ATA_STATUS_CMD) & 0x80) && --timeout);
    if (timeout == 0) return -1;

    outb(ATA_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_ERROR, 0x00);
    outb(ATA_SECCOUNT, 1);
    outb(ATA_LBA_LO, (uint8_t)lba);
    outb(ATA_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_LBA_HI, (uint8_t)(lba >> 16));
    outb(ATA_STATUS_CMD, 0x20);

    ata_delay_400ns();

    timeout = 50000;
    while (--timeout) {
        uint8_t status = inb(ATA_STATUS_CMD);
        if (status & 0x01) return -2;
        if (!(status & 0x80) && (status & 0x08)) break;
    }
    if (timeout == 0) return -1;

    uint16_t *buf16 = (uint16_t *)buffer;
    for (int i = 0; i < 256; i++) buf16[i] = inw(ATA_DATA);
    return 0;
}

int ata_write_sector(uint32_t lba, const uint8_t *buffer) {
    uint32_t timeout = 50000;
    while ((inb(ATA_STATUS_CMD) & 0x80) && --timeout);
    if (timeout == 0) return -1;

    outb(ATA_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_ERROR, 0x00);
    outb(ATA_SECCOUNT, 1);
    outb(ATA_LBA_LO, (uint8_t)lba);
    outb(ATA_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_LBA_HI, (uint8_t)(lba >> 16));
    outb(ATA_STATUS_CMD, 0x30);

    ata_delay_400ns();

    timeout = 50000;
    while (--timeout) {
        uint8_t status = inb(ATA_STATUS_CMD);
        if (status & 0x01) return -2;
        if (!(status & 0x80) && (status & 0x08)) break;
    }
    if (timeout == 0) return -1;

    const uint16_t *buf16 = (const uint16_t *)buffer;
    for (int i = 0; i < 256; i++) outw(ATA_DATA, buf16[i]);

    outb(ATA_STATUS_CMD, 0xE7);
    ata_delay_400ns();
    return 0;
}

static void cmd_xxd(uint32_t lba) {
    static uint8_t xxd_buf[512] __attribute__((aligned(4)));
    int res = ata_read_sector(lba, xxd_buf);
    if (res != 0) {
        vga_puts("ATA Driver: Sector out of bounds (ERR).\n", 0x0C);
        return;
    }
    vga_puts("LBA ", 0x0A); vga_print_dec(lba, 0x0E); vga_puts(" (32 Bytes):\n", 0x0A);
    for (int r = 0; r < 2; r++) {
        vga_print_hex8(r * 16, 0x07);
        vga_puts(": ", 0x07);
        for (int i = 0; i < 16; i++) {
            vga_print_hex8(xxd_buf[r * 16 + i], 0x0F);
            vga_putc_color(' ', 0x07);
        }
        vga_puts("|", 0x07);
        for (int i = 0; i < 16; i++) {
            char c = xxd_buf[r * 16 + i];
            if (c >= 32 && c <= 126) vga_putc_color(c, 0x0E);
            else vga_putc_color('.', 0x08);
        }
        vga_puts("|\n", 0x07);
    }
}

// --- 8. Полноразмерный Inode ext2 (ровно 128 байт) и структуры ext2 ---
#define EXT2_BASE_LBA    140
#define EXT2_SUPER_MAGIC 0xEF53

struct ext2_superblock {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
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

struct ext2_inode {
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
    uint8_t  i_pad[28];
} __attribute__((packed));

struct ext2_group_desc {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint32_t bg_reserved[3];
} __attribute__((packed));

struct ext2_dir_entry {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[];
} __attribute__((packed));

static int ext2_read_inode(uint32_t inode_num, struct ext2_inode *out_inode) {
    static uint8_t bg_buf[512] __attribute__((aligned(4)));
    if (ata_read_sector(EXT2_BASE_LBA + 4, bg_buf) != 0) return -1;
    struct ext2_group_desc *bg = (struct ext2_group_desc *)bg_buf;

    uint32_t inode_table_lba = EXT2_BASE_LBA + (bg->bg_inode_table * 2);
    uint32_t inode_index = inode_num - 1;
    uint32_t sector_offset = (inode_index * 128) / 512;
    uint32_t byte_offset   = (inode_index * 128) % 512;

    static uint8_t in_buf[512] __attribute__((aligned(4)));
    if (ata_read_sector(inode_table_lba + sector_offset, in_buf) != 0) return -1;

    for (int i = 0; i < 128; i++) {
        ((uint8_t *)out_inode)[i] = in_buf[byte_offset + i];
    }
    return 0;
}

static int ext2_read_file(const char *filename, uint8_t *dest, uint32_t *file_size) {
    struct ext2_inode root_in;
    if (ext2_read_inode(2, &root_in) != 0) return -1;

    static uint8_t dir_blk[1024] __attribute__((aligned(4)));
    uint32_t dir_lba = EXT2_BASE_LBA + (root_in.i_block[0] * 2);
    if (ata_read_sector(dir_lba, dir_blk) != 0 ||
        ata_read_sector(dir_lba + 1, dir_blk + 512) != 0) return -1;

    uint32_t offset = 0;
    while (offset < 1024) {
        struct ext2_dir_entry *d = (struct ext2_dir_entry *)&dir_blk[offset];
        if (d->inode == 0 || d->rec_len == 0) break;

        int len = 0; while (filename[len]) len++;
        if (len == d->name_len) {
            int match = 1;
            for (int i = 0; i < len; i++) {
                if (filename[i] != d->name[i]) { match = 0; break; }
            }
            if (match) {
                struct ext2_inode fin;
                if (ext2_read_inode(d->inode, &fin) != 0) return -1;
                *file_size = fin.i_size;
                uint32_t num_blocks = (fin.i_size + 1023) / 1024;
                for (uint32_t b = 0; b < num_blocks && b < 12; b++) {
                    uint32_t f_lba = EXT2_BASE_LBA + (fin.i_block[b] * 2);
                    ata_read_sector(f_lba, dest + b * 1024);
                    ata_read_sector(f_lba + 1, dest + b * 1024 + 512);
                }
                return 0;
            }
        }
        offset += d->rec_len;
    }
    return -1;
}

static void cmd_ext2_ls(void) {
    struct ext2_inode root_in;
    if (ext2_read_inode(2, &root_in) != 0) {
        vga_puts("ext2: Failed reading root inode.\n", 0x0C);
        return;
    }

    static uint8_t dir_blk[1024] __attribute__((aligned(4)));
    uint32_t dir_lba = EXT2_BASE_LBA + (root_in.i_block[0] * 2);
    if (ata_read_sector(dir_lba, dir_blk) != 0 ||
        ata_read_sector(dir_lba + 1, dir_blk + 512) != 0) {
        vga_puts("ext2: Failed reading root directory data.\n", 0x0C);
        return;
    }

    vga_puts("Inode   Type   Size (Bytes)   Filename\n", 0x0E);
    vga_puts("--------------------------------------\n", 0x07);

    uint32_t offset = 0;
    while (offset < 1024) {
        struct ext2_dir_entry *d = (struct ext2_dir_entry *)&dir_blk[offset];
        if (d->inode == 0 || d->rec_len == 0) break;

        vga_print_dec(d->inode, 0x0B);
        vga_puts("       ", 0x07);
        if (d->file_type == 2) vga_puts("DIR    ", 0x0A);
        else vga_puts("FILE   ", 0x0E);

        struct ext2_inode fin;
        if (ext2_read_inode(d->inode, &fin) == 0) {
            vga_print_dec(fin.i_size, 0x0F);
        } else {
            vga_puts("???", 0x0C);
        }
        vga_puts("           ", 0x07);

        for (int i = 0; i < d->name_len; i++) vga_putc_color(d->name[i], 0x0F);
        vga_putc_color('\n', 0x07);

        offset += d->rec_len;
    }
}

static void cmd_ext2_cat(const char *filename) {
    static uint8_t fbuf[4096] __attribute__((aligned(4)));
    uint32_t fsize = 0;
    if (ext2_read_file(filename, fbuf, &fsize) == 0) {
        vga_puts("--- ext2 Content of ", 0x0E);
        vga_puts(filename, 0x0E);
        vga_puts(" ---\n", 0x0E);
        for (uint32_t i = 0; i < fsize && i < 1024; i++) {
            vga_putc_color(fbuf[i], 0x0F);
        }
        vga_putc_color('\n', 0x07);
    } else {
        vga_puts("ext2: File not found.\n", 0x0C);
    }
}

static void cmd_time(void) {
    uint32_t wait_count = 10000;
    while (cmos_is_updating() && --wait_count);

    uint8_t sec   = cmos_read(0x00);
    uint8_t min   = cmos_read(0x02);
    uint8_t hour  = cmos_read(0x04);
    uint8_t day   = cmos_read(0x07);
    uint8_t mon   = cmos_read(0x08);
    uint8_t year  = cmos_read(0x09);
    uint8_t reg_b = cmos_read(0x0B);

    if (!(reg_b & 0x04)) {
        sec  = BCD_TO_BIN(sec);
        min  = BCD_TO_BIN(min);
        hour = BCD_TO_BIN(hour & 0x7F) | (hour & 0x80);
        day  = BCD_TO_BIN(day);
        mon  = BCD_TO_BIN(mon);
        year = BCD_TO_BIN(year);
    }
    if (!(reg_b & 0x02) && (hour & 0x80)) {
        hour = ((hour & 0x7F) + 12) % 24;
    }

    vga_puts("CMOS RTC Hardware Time: 20", 0x0A);
    if (year < 10) vga_putc_color('0', 0x0E);
    vga_print_dec(year, 0x0E);
    vga_putc_color('-', 0x07);
    if (mon < 10) vga_putc_color('0', 0x0E);
    vga_print_dec(mon, 0x0E);
    vga_putc_color('-', 0x07);
    if (day < 10) vga_putc_color('0', 0x0E);
    vga_print_dec(day, 0x0E);
    vga_puts(" ", 0x07);
    if (hour < 10) vga_putc_color('0', 0x0B);
    vga_print_dec(hour, 0x0B);
    vga_putc_color(':', 0x07);
    if (min < 10) vga_putc_color('0', 0x0B);
    vga_print_dec(min, 0x0B);
    vga_putc_color(':', 0x07);
    if (sec < 10) vga_putc_color('0', 0x0B);
    vga_print_dec(sec, 0x0B);
    vga_puts(" UTC\n", 0x07);
}

// --- 9. Системные утилиты lspci и sysinfo ---
static volatile uint32_t timer_ticks = 0;
static volatile int num_tasks = 1;

static const char *pci_class_name(uint8_t class_code) {
    switch (class_code) {
        case 0x01: return "Storage Controller";
        case 0x02: return "Network Controller";
        case 0x03: return "Display Controller";
        case 0x04: return "Multimedia Controller";
        case 0x06: return "Bridge Device";
        case 0x0C: return "Serial Bus Controller";
        default:   return "Peripheral Controller";
    }
}

static void cmd_lspci(void) {
    pci_scan();
    vga_puts("B:D.F  Class Name                  Vendor:Device\n", 0x0E);
    vga_puts("------------------------------------------------\n", 0x07);
    if (pci_dev_count == 0) {
        vga_puts("No PCI devices discovered.\n", 0x08);
        return;
    }
    for (int i = 0; i < pci_dev_count; i++) {
        vga_print_hex8(pci_devices[i].bus, 0x0B);
        vga_putc_color(':', 0x07);
        vga_print_hex8(pci_devices[i].slot, 0x0B);
        vga_putc_color('.', 0x07);
        vga_print_dec(pci_devices[i].func, 0x0B);
        vga_puts("  ", 0x07);

        const char *cname = pci_class_name(pci_devices[i].class_code);
        vga_puts(cname, 0x0A);
        int len = 0; while (cname[len]) len++;
        for (int s = 0; s < (28 - len); s++) vga_putc_color(' ', 0x07);

        vga_print_hex8((uint8_t)(pci_devices[i].vendor_id >> 8), 0x0F);
        vga_print_hex8((uint8_t)(pci_devices[i].vendor_id & 0xFF), 0x0F);
        vga_putc_color(':', 0x07);
        vga_print_hex8((uint8_t)(pci_devices[i].device_id >> 8), 0x0E);
        vga_print_hex8((uint8_t)(pci_devices[i].device_id & 0xFF), 0x0E);
        vga_putc_color('\n', 0x07);
    }
}

static void cmd_sysinfo(void) {
    pci_scan();
    uint32_t seconds = timer_ticks / 100;
    vga_puts("=== Aporia OS v1.0.0 Workstation Diagnostics ===\n", 0x0E);
    vga_puts("  Kernel Uptime   : ", 0x07); vga_print_dec(seconds, 0x0A); vga_puts(" seconds (PIT 100 Hz)\n", 0x07);
    vga_puts("  Display Engine  : VESA VBE 1024x768x32bpp LFB Active\n", 0x07);
    vga_puts("  Root Filesystem : ext2 VFS Volume (LBA 140)\n", 0x07);
    vga_puts("  PCI Bus Status  : ", 0x07); vga_print_dec(pci_dev_count, 0x0B); vga_puts(" hardware devices active\n", 0x07);
    vga_puts("  Heap Memory     : 2048 KB Monolithic Pool (0x100000)\n", 0x07);
    vga_puts("  Process Slots   : ", 0x07); vga_print_dec(num_tasks, 0x0D); vga_puts(" registered tasks\n", 0x07);
}

// --- 10. Пайпы IPC ---
#define PIPE_CAPACITY 256
static char pipe_buffer[PIPE_CAPACITY];
static uint32_t pipe_head = 0;
static uint32_t pipe_tail = 0;
static uint32_t pipe_count = 0;

int pipe_write(const char *src, uint32_t len) {
    uint32_t written = 0;
    while (written < len && pipe_count < PIPE_CAPACITY) {
        pipe_buffer[pipe_head] = src[written];
        pipe_head = (pipe_head + 1) % PIPE_CAPACITY;
        pipe_count++;
        written++;
    }
    return written;
}

int pipe_read(char *dst, uint32_t max_len) {
    uint32_t read_bytes = 0;
    while (read_bytes < max_len && pipe_count > 0) {
        dst[read_bytes] = pipe_buffer[pipe_tail];
        pipe_tail = (pipe_tail + 1) % PIPE_CAPACITY;
        pipe_count--;
        read_bytes++;
    }
    return read_bytes;
}

// --- 11. Текстовый редактор Nano ---
static char editor_buffer[512];
static int editor_len = 0;
static char editor_filename[16];

static void nano_render(void) {
    vga_clear();
    vga_puts_at(0, 0, "[ Nano Editor | File: ", 0x70);
    vga_puts_at(22, 0, editor_filename, 0x70);
    vga_puts_at(38, 0, "| Ctrl+S: Save | Ctrl+X: Exit ]", 0x70);

    cursor_x = 0;
    cursor_y = 1;
    for (int i = 0; i < editor_len; i++) {
        vga_putc_color(editor_buffer[i], 0x0F);
    }
    vga_puts_at(0, 21, "[ Editor Active: Write code or text ]", 0x1F);
}

static void nano_open(const char *filename) {
    for (int i = 0; i < 15 && filename[i]; i++) editor_filename[i] = filename[i];
    editor_filename[15] = '\0';

    uint32_t len = 0;
    if (ext2_read_file(editor_filename, (uint8_t *)editor_buffer, &len) == 0) {
        editor_len = len;
    } else {
        editor_len = 0;
    }
    editor_active = 1;
    nano_render();
}

static void nano_save(void) {
    vga_puts_at(0, 21, "[ File buffer saved to memory (read-only ext2) ]", 0x2F);
}

static void nano_exit(void) {
    editor_active = 0;
    cursor_x = 0;
    cursor_y = 1;
    vga_clear();
    vga_puts("=== Aporia OS Console ===\n", VGA_ATTR_BANNER);
    vga_puts("Exited nano.\n\n", 0x08);
    vga_puts("gamer@os> ", VGA_ATTR_PROMPT);
}

// --- 12. Микро-ассемблер ---
static void cmd_asm(const char *src_file, const char *dst_file) {
    static uint8_t src_buf[512];
    uint32_t src_len = 0;
    if (ext2_read_file(src_file, src_buf, &src_len) != 0) {
        vga_puts("Assembler Error: Source file not found.\n", 0x0C);
        return;
    }
    src_buf[src_len] = '\0';

    static uint8_t bin_buf[512];
    int bin_idx = 0;
    char *ptr = (char *)src_buf;

    vga_puts("[ASM] Assembling '", 0x0A); vga_puts(src_file, 0x0E); vga_puts("'...\n", 0x0A);

    while (*ptr && bin_idx < 480) {
        while (*ptr == ' ' || *ptr == '\n' || *ptr == '\r' || *ptr == '\t') ptr++;
        if (*ptr == '\0') break;

        if (strncmp(ptr, "mov eax,", 8) == 0) {
            ptr += 8; while (*ptr == ' ') ptr++;
            uint32_t val = parse_dec(ptr);
            bin_buf[bin_idx++] = 0xB8;
            bin_buf[bin_idx++] = val & 0xFF;
            bin_buf[bin_idx++] = (val >> 8) & 0xFF;
            bin_buf[bin_idx++] = (val >> 16) & 0xFF;
            bin_buf[bin_idx++] = (val >> 24) & 0xFF;
            while (*ptr >= '0' && *ptr <= '9') ptr++;
        }
        else if (strncmp(ptr, "mov ebx,", 8) == 0) {
            ptr += 8; while (*ptr == ' ') ptr++;
            uint32_t val = parse_dec(ptr);
            bin_buf[bin_idx++] = 0xBB;
            bin_buf[bin_idx++] = val & 0xFF;
            bin_buf[bin_idx++] = (val >> 8) & 0xFF;
            bin_buf[bin_idx++] = (val >> 16) & 0xFF;
            bin_buf[bin_idx++] = (val >> 24) & 0xFF;
            while (*ptr >= '0' && *ptr <= '9') ptr++;
        }
        else if (strncmp(ptr, "int 0x80", 8) == 0) {
            ptr += 8; bin_buf[bin_idx++] = 0xCD; bin_buf[bin_idx++] = 0x80;
        }
        else if (strncmp(ptr, "ret", 3) == 0) {
            ptr += 3; bin_buf[bin_idx++] = 0xC3;
        }
        else {
            while (*ptr && *ptr != '\n') ptr++;
        }
    }

    if (bin_idx == 0 || bin_buf[bin_idx - 1] != 0xC3) {
        bin_buf[bin_idx++] = 0xB8; bin_buf[bin_idx++] = 2; bin_buf[bin_idx++] = 0; bin_buf[bin_idx++] = 0; bin_buf[bin_idx++] = 0;
        bin_buf[bin_idx++] = 0xBB; bin_buf[bin_idx++] = 55; bin_buf[bin_idx++] = 0; bin_buf[bin_idx++] = 0; bin_buf[bin_idx++] = 0;
        bin_buf[bin_idx++] = 0xCD; bin_buf[bin_idx++] = 0x80;
    }

    vga_puts("[ASM] Compiled ", 0x0A);
    vga_print_dec(bin_idx, 0x0B);
    vga_puts(" bytes into buffer (ready to exec)\n", 0x0A);
}

// --- 13. GDT & TSS ---
struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

struct tss_entry {
    uint32_t prev_tss;
    uint32_t esp0;
    uint32_t ss0;
    uint32_t esp1, ss1, esp2, ss2;
    uint32_t cr3;
    uint32_t eip, eflags, eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap, iomap_base;
} __attribute__((packed));

static struct gdt_entry gdt[6];
static struct gdt_ptr   gp;
static struct tss_entry tss;
static uint8_t kernel_tss_stack[4096];

extern void gdt_flush(uint32_t);
extern void tss_flush(void);

static void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[num].base_low    = (base & 0xFFFF);
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].base_high   = (base >> 24) & 0xFF;
    gdt[num].limit_low   = (limit & 0xFFFF);
    gdt[num].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[num].access      = access;
}

static void gdt_init(void) {
    gp.limit = (sizeof(struct gdt_entry) * 6) - 1;
    gp.base  = (uint32_t)&gdt;

    gdt_set_gate(0, 0, 0, 0, 0);
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);

    uint32_t tss_base = (uint32_t)&tss;
    uint32_t tss_limit = sizeof(struct tss_entry) - 1;
    gdt_set_gate(5, tss_base, tss_limit, 0xE9, 0x00);

    for (uint32_t i = 0; i < sizeof(struct tss_entry); i++) ((uint8_t *)&tss)[i] = 0;
    tss.ss0  = 0x10;
    tss.esp0 = (uint32_t)&kernel_tss_stack[4096];
    tss.iomap_base = sizeof(struct tss_entry);

    gdt_flush((uint32_t)&gp);
    tss_flush();
}

// --- 14. Аллокатор кучи ---
struct heap_block {
    uint32_t size;
    uint32_t is_free;
    struct heap_block *next;
};

static struct heap_block *heap_head = (struct heap_block *)HEAP_START;

static void heap_init(void) {
    heap_head->size = HEAP_SIZE - sizeof(struct heap_block);
    heap_head->is_free = 1;
    heap_head->next = 0;
}

void *kmalloc(uint32_t size) {
    if (size == 0) return 0;
    size = (size + 3) & ~3;
    struct heap_block *curr = heap_head;
    while (curr) {
        if (curr->is_free && curr->size >= size) {
            if (curr->size >= size + sizeof(struct heap_block) + 4) {
                struct heap_block *new_b = (struct heap_block *)((uint32_t)curr + sizeof(struct heap_block) + size);
                new_b->size = curr->size - size - sizeof(struct heap_block);
                new_b->is_free = 1;
                new_b->next = curr->next;
                curr->size = size;
                curr->next = new_b;
            }
            curr->is_free = 0;
            return (void *)((uint32_t)curr + sizeof(struct heap_block));
        }
        curr = curr->next;
    }
    return 0;
}

// --- 15. Paging (MMU) + Маппинг VESA LFB ---
static uint32_t kernel_page_directory[1024] __attribute__((aligned(4096)));
static uint32_t kernel_page_table[1024] __attribute__((aligned(4096)));
static uint32_t fb_page_table[1024] __attribute__((aligned(4096)));

static void mmu_and_sse_init(void) {
    uint32_t cr0;
    asm volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1 << 2); cr0 |= (1 << 1);
    asm volatile ("mov %0, %%cr0" : : "r"(cr0));

    uint32_t cr4;
    asm volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (3 << 9);
    asm volatile ("mov %0, %%cr4" : : "r"(cr4));

    for (int i = 0; i < 1024; i++) kernel_page_table[i] = (i * 0x1000) | 3;
    kernel_page_directory[0] = ((uint32_t)kernel_page_table) | 3;
    for (int i = 1; i < 1024; i++) kernel_page_directory[i] = 0x00000002;

    uint32_t pde_idx = fb_phys >> 22;
    for (int i = 0; i < 1024; i++) {
        fb_page_table[i] = (fb_phys + (i * 0x1000)) | 7;
    }
    kernel_page_directory[pde_idx] = ((uint32_t)fb_page_table) | 7;

    asm volatile ("mov %0, %%cr3" : : "r"(kernel_page_directory));
    cr0 |= 0x80000000;
    asm volatile ("mov %0, %%cr0" : : "r"(cr0));
}

// --- 16. IDT структуры ---
struct idt_entry {
    uint16_t base_low;
    uint16_t sel;
    uint8_t  always0;
    uint8_t  flags;
    uint16_t base_high;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr   idtp;

extern void idt_load(uint32_t);

static void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_low = (base & 0xFFFF);
    idt[num].base_high = (base >> 16) & 0xFFFF;
    idt[num].sel = sel;
    idt[num].always0 = 0;
    idt[num].flags = flags;
}

extern void isr0(void);  extern void isr1(void);  extern void isr2(void);  extern void isr3(void);
extern void isr4(void);  extern void isr5(void);  extern void isr6(void);  extern void isr7(void);
extern void isr8(void);  extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void); extern void isr15(void);
extern void isr16(void); extern void isr17(void); extern void isr18(void); extern void isr19(void);
extern void isr20(void); extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void); extern void isr27(void);
extern void isr28(void); extern void isr29(void); extern void isr30(void); extern void isr31(void);
extern void isr128(void);

extern void irq0(void);  extern void irq1(void);  extern void irq2(void);  extern void irq3(void);
extern void irq4(void);  extern void irq5(void);  extern void irq6(void);  extern void irq7(void);
extern void irq8(void);  extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void); extern void irq15(void);

// --- 17. Структуры задач ---
struct task {
    uint32_t esp;
    uint32_t kstack_top;
    uint32_t cr3;
    uint32_t id;
    char     name[16];
    volatile uint32_t slices;
    uint32_t is_user;
    uint32_t state;
    uint32_t exit_code;
};

static struct task tasks[MAX_TASKS];
static volatile int current_task = 0;
static volatile int scheduler_active = 0;

uint32_t schedule(uint32_t current_esp);

struct registers {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, useresp, ss;
};

static void print_tasks_table(void) {
    vga_puts("PID  Name            Ring   Status   CR3 Base     Exit  Slices\n", 0x0E);
    vga_puts("--------------------------------------------------------------\n", 0x07);
    for (int k = 0; k < num_tasks; k++) {
        vga_print_dec(tasks[k].id, 0x0B);
        vga_puts("    ", 0x07);
        vga_puts(tasks[k].name, 0x0F);
        int len = 0; while (tasks[k].name[len]) len++;
        for (int s = 0; s < (16 - len); s++) vga_putc_color(' ', 0x07);

        if (tasks[k].is_user) vga_puts("Ring 3 ", 0x0E);
        else vga_puts("Ring 0 ", 0x0B);

        if (tasks[k].state == TASK_STATE_RUNNING) vga_puts("RUNNING  ", 0x0A);
        else vga_puts("DEAD     ", 0x0C);

        vga_print_hex32(tasks[k].cr3, 0x0E);
        vga_puts("   ", 0x07);
        if (tasks[k].state == TASK_STATE_DEAD) vga_print_dec(tasks[k].exit_code, 0x0C);
        else vga_puts("- ", 0x07);
        vga_puts("    ", 0x07);
        vga_print_dec(tasks[k].slices, 0x0A);
        vga_putc_color('\n', 0x07);
    }
}

// --- 18. Диспетчер системных вызовов ---
uint32_t syscall_dispatcher(struct registers *r) {
    if (r->eax == 1) {
        const char *msg = (const char *)r->ebx;
        int row = (tasks[current_task].id == 2) ? 23 : 24;
        vga_puts_at(0, row, "[PID ", 0x0E);
        vga_print_dec_at(5, row, tasks[current_task].id, 0x0E);
        vga_puts_at(6, row, "]: ", 0x0E);
        vga_puts_at(9, row, msg, 0x0A);
        vga_putc_at(42, row, (char)r->edx, 0x0F);
        vga_puts_at(44, row, "Iters: ", 0x07);
        vga_print_dec_at(51, row, r->ecx, 0x0B);
        return (uint32_t)r;
    } else if (r->eax == 2) {
        tasks[current_task].state = TASK_STATE_DEAD;
        tasks[current_task].exit_code = r->ebx;

        vga_puts("\n[EXIT] Process PID ", 0x0A);
        vga_print_dec(tasks[current_task].id, 0x0E);
        vga_puts(" ('", 0x0A);
        vga_puts(tasks[current_task].name, 0x0E);
        vga_puts("') called sys_exit with code ", 0x0A);
        vga_print_dec(r->ebx, 0x0B);
        vga_puts(".\n", 0x0A);

        int row = (tasks[current_task].id == 2) ? 23 : 24;
        vga_puts_at(0, row, "[Process ]: EXITED (Clean sys_exit)                     ", 0x02);
        vga_puts("gamer@os> ", VGA_ATTR_PROMPT);

        return schedule((uint32_t)r);
    } else if (r->eax == 3) {
        int child = -1;
        for (int k = 1; k < MAX_TASKS; k++) {
            if (k >= num_tasks || tasks[k].state == TASK_STATE_DEAD) {
                child = k; break;
            }
        }
        if (child == -1) {
            r->eax = (uint32_t)-1; return (uint32_t)r;
        }

        int parent = current_task;
        uint32_t child_base = USER_FRAMES_BASE + (child * 0x10000);
        uint32_t *child_pdir   = (uint32_t *)(child_base + 0x0000);
        uint32_t *child_ptable = (uint32_t *)(child_base + 0x1000);
        uint8_t  *child_code   = (uint8_t  *)(child_base + 0x2000);
        uint8_t  *child_ustack = (uint8_t  *)(child_base + 0x3000);

        for (int k = 0; k < 1024; k++) {
            child_pdir[k] = 0x00000002;
            child_ptable[k] = 0x00000002;
        }

        child_pdir[0] = ((uint32_t)kernel_page_table) | 3;
        child_pdir[fb_phys >> 22] = ((uint32_t)fb_page_table) | 7;

        uint32_t parent_base = USER_FRAMES_BASE + (parent * 0x10000);
        uint8_t *parent_code = (uint8_t *)(parent_base + 0x2000);
        uint8_t *parent_ustack = (uint8_t *)(parent_base + 0x3000);

        for (int k = 0; k < 4096; k++) {
            child_code[k] = parent_code[k];
            child_ustack[k] = parent_ustack[k];
        }

        child_ptable[0] = ((uint32_t)child_code) | 7;
        child_ptable[1] = ((uint32_t)child_ustack) | 7;
        child_pdir[1]   = ((uint32_t)child_ptable) | 7;

        if (tasks[child].kstack_top == 0) {
            tasks[child].kstack_top = (uint32_t)kmalloc(STACK_SIZE) + STACK_SIZE;
        }
        struct registers *child_r = (struct registers *)(tasks[child].kstack_top - sizeof(struct registers));
        *child_r = *r;
        child_r->eax = 0;

        tasks[child].esp = (uint32_t)child_r;
        tasks[child].cr3 = (uint32_t)child_pdir;
        tasks[child].id = child;

        int nlen = 0;
        while (tasks[parent].name[nlen] && nlen < 12) {
            tasks[child].name[nlen] = tasks[parent].name[nlen]; nlen++;
        }
        tasks[child].name[nlen++] = '_';
        tasks[child].name[nlen++] = 'c';
        tasks[child].name[nlen] = '\0';

        tasks[child].slices = 0;
        tasks[child].is_user = 1;
        tasks[child].state = TASK_STATE_RUNNING;
        tasks[child].exit_code = 0;
        if (child >= num_tasks) num_tasks = child + 1;

        vga_puts("\n[FORK] Parent PID ", 0x0A);
        vga_print_dec(parent, 0x0E);
        vga_puts(" forked Child PID ", 0x0A);
        vga_print_dec(child, 0x0E);
        vga_puts("\n", 0x0A);
        vga_puts("gamer@os> ", VGA_ATTR_PROMPT);

        r->eax = child;
        return (uint32_t)r;
    } else if (r->eax == 4) {
        r->eax = pipe_write((const char *)r->ebx, r->ecx);
        return (uint32_t)r;
    } else if (r->eax == 5) {
        r->eax = pipe_read((char *)r->ebx, r->ecx);
        return (uint32_t)r;
    }
    return (uint32_t)r;
}

// --- 19. Загрузчик исполняемых файлов fs_exec (напрямую из ext2 VFS) ---
int fs_exec(const char *cmdline) {
    int slot = -1;
    for (int k = 2; k < MAX_TASKS; k++) {
        if (k >= num_tasks || tasks[k].state == TASK_STATE_DEAD) {
            slot = k; break;
        }
    }
    if (slot == -1) {
        vga_puts("Error: Process table full.\n", 0x0C);
        return -1;
    }

    char filename[16];
    int i = 0;
    while (cmdline[i] && cmdline[i] != ' ' && i < 15) {
        filename[i] = cmdline[i]; i++;
    }
    filename[i] = '\0';

    static uint8_t raw_file_buf[8192] __attribute__((aligned(4)));
    uint32_t file_size = 0;
    if (ext2_read_file(filename, raw_file_buf, &file_size) != 0) {
        vga_puts("Error: File not found in ext2.\n", 0x0C);
        return -1;
    }

    uint32_t proc_base = USER_FRAMES_BASE + (slot * 0x10000);
    uint32_t *pdir   = (uint32_t *)(proc_base + 0x0000);
    uint32_t *ptable = (uint32_t *)(proc_base + 0x1000);
    uint8_t  *code   = (uint8_t  *)(proc_base + 0x2000);
    uint8_t  *ustack = (uint8_t  *)(proc_base + 0x3000);

    for (int k = 0; k < 1024; k++) {
        pdir[k] = 0x00000002; ptable[k] = 0x00000002;
    }
    for (int k = 0; k < 4096; k++) { code[k] = 0; ustack[k] = 0; }

    pdir[0] = ((uint32_t)kernel_page_table) | 3;
    pdir[fb_phys >> 22] = ((uint32_t)fb_page_table) | 7;
    ptable[0] = ((uint32_t)code) | 7;
    ptable[1] = ((uint32_t)ustack) | 7;
    pdir[1]   = ((uint32_t)ptable) | 7;

    uint32_t entry_point = 0x400000;
    uint8_t *ident = raw_file_buf;

    if (ident[0] == 0x7F && ident[1] == 'E' && ident[2] == 'L' && ident[3] == 'F') {
        uint32_t e_entry = *(uint32_t *)(raw_file_buf + 24);
        uint32_t e_phoff = *(uint32_t *)(raw_file_buf + 28);
        uint16_t e_phnum = *(uint16_t *)(raw_file_buf + 44);

        vga_puts("[ELF32] ext2 executable verified. Loading segments...\n", 0x0E);
        for (int p = 0; p < e_phnum; p++) {
            uint8_t *phdr = raw_file_buf + e_phoff + (p * 32);
            uint32_t p_type   = *(uint32_t *)(phdr + 0);
            uint32_t p_offset = *(uint32_t *)(phdr + 4);
            uint32_t p_vaddr  = *(uint32_t *)(phdr + 8);
            uint32_t p_filesz = *(uint32_t *)(phdr + 16);

            if (p_type == 1) {
                if (p_vaddr >= 0x400000 && p_vaddr < 0x401000) {
                    uint32_t dest_off = p_vaddr - 0x400000;
                    for (uint32_t b = 0; b < p_filesz && (dest_off + b) < 4096; b++) {
                        code[dest_off + b] = raw_file_buf[p_offset + b];
                    }
                } else if (p_vaddr < 0x400000 && (p_vaddr + p_filesz) > 0x400000) {
                    uint32_t skip = 0x400000 - p_vaddr;
                    uint32_t bytes = p_filesz - skip;
                    for (uint32_t b = 0; b < bytes && b < 4096; b++) {
                        code[b] = raw_file_buf[p_offset + skip + b];
                    }
                }
            }
        }
        entry_point = e_entry;
    } else {
        for (uint32_t k = 0; k < file_size && k < 4096; k++) {
            code[k] = raw_file_buf[k];
        }
    }

    if (tasks[slot].kstack_top == 0) {
        tasks[slot].kstack_top = (uint32_t)kmalloc(STACK_SIZE) + STACK_SIZE;
    }
    uint32_t *kstack = (uint32_t *)tasks[slot].kstack_top;

    *(--kstack) = 0x23;
    *(--kstack) = 0x401FF0; // 16-байтное выравнивание
    *(--kstack) = 0x00000202;
    *(--kstack) = 0x1B;
    *(--kstack) = entry_point;
    *(--kstack) = 0;
    *(--kstack) = 32;
    *(--kstack) = 0; *(--kstack) = 0; *(--kstack) = 0; *(--kstack) = 0;
    *(--kstack) = 0; *(--kstack) = 0; *(--kstack) = 0; *(--kstack) = 0;
    *(--kstack) = 0x23; *(--kstack) = 0x23; *(--kstack) = 0x23; *(--kstack) = 0x23;

    tasks[slot].esp = (uint32_t)kstack;
    tasks[slot].cr3 = (uint32_t)pdir;
    tasks[slot].id = slot;

    for (int k = 0; k < 15 && filename[k]; k++) tasks[slot].name[k] = filename[k];
    tasks[slot].name[15] = '\0';

    tasks[slot].slices = 0;
    tasks[slot].is_user = 1;
    tasks[slot].state = TASK_STATE_RUNNING;
    tasks[slot].exit_code = 0;
    if (slot >= num_tasks) num_tasks = slot + 1;

    vga_puts("[EXEC] PID ", 0x0A);
    vga_print_dec(slot, 0x0B);
    vga_puts(" '", 0x0A); vga_puts(filename, 0x0E);
    vga_puts("' active from ext2 VFS (Entry: ", 0x0A); vga_print_hex32(entry_point, 0x0E);
    vga_puts(")\n", 0x0A);

    return slot;
}

// --- 20. Планировщик ---
uint32_t schedule(uint32_t current_esp) {
    if (!scheduler_active || num_tasks <= 1) return current_esp;

    if (tasks[current_task].state == TASK_STATE_RUNNING) {
        tasks[current_task].esp = current_esp;
        tasks[current_task].slices++;
    }

    int tries = 0;
    do {
        current_task = (current_task + 1) % num_tasks;
        tries++;
    } while (tasks[current_task].state != TASK_STATE_RUNNING && tries < num_tasks * 2);

    tss.esp0 = tasks[current_task].kstack_top;

    uint32_t next_cr3 = tasks[current_task].cr3;
    uint32_t cur_cr3;
    asm volatile ("mov %%cr3, %0" : "=r"(cur_cr3));
    if (cur_cr3 != next_cr3) {
        asm volatile ("mov %0, %%cr3" : : "r"(next_cr3));
    }

    return tasks[current_task].esp;
}

static void kernel_worker(void) {
    uint32_t count = 0;
    static const char spin_chars[] = {'|', '/', '-', '\\'};
    int spin_idx = 0;
    while (1) {
        vga_puts_at(0, 22, "[Ring 0 Kernel Task  ]: Active ", 0x0B);
        vga_putc_at(31, 22, spin_chars[(spin_idx++) & 3], 0x0F);
        vga_puts_at(33, 22, "Iters: ", 0x07);
        vga_print_dec_at(40, 22, ++count, 0x0D);
        for (volatile int i = 0; i < 20000; i++);
    }
}

// --- 21. Интерактивные кнопки TUI (Строка 0) ---
static void render_header_menu(void) {
    vga_puts_at(0, 0,  "[TIME]", 0x30);
    vga_putc_at(6, 0, ' ', 0x07);
    vga_puts_at(7, 0,  "[TASKS]", 0x20);
    vga_putc_at(14, 0, ' ', 0x07);
    vga_puts_at(15, 0, "[CLEAR]", 0x40);
    vga_putc_at(22, 0, ' ', 0x07);
    vga_puts_at(23, 0, "[EXEC]", 0x60);
    vga_puts_at(29, 0, " <- GUI Buttons", 0x08);
}

// --- 22. Пиксельная Мышь PS/2 с оптимизированным Drag & Drop ---
static int mouse_px = 512;
static int mouse_py = 384;
static int prev_mouse_px = 512;
static int prev_mouse_py = 384;
static uint32_t cursor_saved[18][12];
static uint8_t mouse_cycle = 0;
static int8_t mouse_bytes[3];
static uint8_t mouse_btn_left = 0;
static uint8_t mouse_btn_right = 0;
static uint8_t prev_btn_left = 0;

static int is_dragging = 0;
static int drag_off_x = 0;
static int drag_off_y = 0;

static const uint16_t mouse_arrow[18] = {
    0b100000000000,
    0b110000000000,
    0b111000000000,
    0b111100000000,
    0b111110000000,
    0b111111000000,
    0b111111100000,
    0b111111110000,
    0b111111111000,
    0b111111111100,
    0b111111100000,
    0b110111100000,
    0b100011110000,
    0b000011110000,
    0b000001111000,
    0b000001111000,
    0b000000110000,
    0b000000000000
};

static void draw_mouse_cursor(int px, int py, int restore) {
    if (restore) {
        for (int y = 0; y < 18; y++) {
            for (int x = 0; x < 12; x++) {
                put_pixel(prev_mouse_px + x, prev_mouse_py + y, cursor_saved[y][x]);
            }
        }
    } else {
        for (int y = 0; y < 18; y++) {
            for (int x = 0; x < 12; x++) {
                cursor_saved[y][x] = get_pixel(px + x, py + y);
                if (mouse_arrow[y] & (1 << (11 - x))) {
                    put_pixel(px + x, py + y, mouse_btn_left ? 0xFF0000 : 0xFFFFFF);
                }
            }
        }
        prev_mouse_px = px;
        prev_mouse_py = py;
    }
}

static inline void mouse_wait_write(void) {
    uint32_t timeout = 100000;
    while ((inb(0x64) & 0x02) && --timeout);
}

static inline void mouse_wait_read(void) {
    uint32_t timeout = 100000;
    while (!(inb(0x64) & 0x01) && --timeout);
}

static void mouse_write(uint8_t write) {
    mouse_wait_write();
    outb(0x64, 0xD4);
    mouse_wait_write();
    outb(0x60, write);
}

static uint8_t mouse_read(void) {
    mouse_wait_read();
    return inb(0x60);
}

static void check_mouse_gui_click(void) {
    if (mouse_btn_left && !prev_btn_left) {
        if (mouse_px >= win_x && mouse_px < win_x + 640 &&
            mouse_py >= win_y - 24 && mouse_py < win_y) {
            is_dragging = 1;
            drag_off_x = mouse_px - win_x;
            drag_off_y = mouse_py - win_y;
        } else {
            int term_rel_x = mouse_px - win_x;
            int term_rel_y = mouse_py - win_y;

            if (term_rel_y >= 0 && term_rel_y < 16) {
                int cell_x = term_rel_x / 8;
                if (cell_x >= 0 && cell_x <= 5) {
                    vga_putc_color('\n', VGA_ATTR_DEFAULT);
                    cmd_time();
                    vga_puts("gamer@os> ", VGA_ATTR_PROMPT);
                } else if (cell_x >= 7 && cell_x <= 13) {
                    vga_putc_color('\n', VGA_ATTR_DEFAULT);
                    print_tasks_table();
                    vga_puts("gamer@os> ", VGA_ATTR_PROMPT);
                } else if (cell_x >= 15 && cell_x <= 21) {
                    vga_clear();
                    vga_puts("gamer@os> ", VGA_ATTR_PROMPT);
                } else if (cell_x >= 23 && cell_x <= 28) {
                    vga_putc_color('\n', VGA_ATTR_DEFAULT);
                    fs_exec("elfapp");
                    vga_puts("gamer@os> ", VGA_ATTR_PROMPT);
                }
            }
        }
    }

    if (!mouse_btn_left && prev_btn_left) {
        if (is_dragging) {
            int new_x = mouse_px - drag_off_x;
            int new_y = mouse_py - drag_off_y;
            if (new_x < 4) new_x = 4;
            if (new_x + 640 > fb_width - 4) new_x = fb_width - 640 - 4;
            if (new_y < 24) new_y = 24;
            if (new_y + 400 > fb_height - 4) new_y = fb_height - 400 - 4;

            if (new_x != win_x || new_y != win_y) {
                clear_exposed_rects(win_x - 4, win_y - 24, new_x - 4, new_y - 24, 648, 428, 0x141923);
                win_x = new_x;
                win_y = new_y;
                render_window_frame(win_x, win_y);
                for (int y = 0; y < VGA_ROWS; y++) {
                    for (int x = 0; x < VGA_COLS; x++) {
                        vga_sync_cell(x, y);
                    }
                }
            }
            is_dragging = 0;
        }
    }

    if (mouse_btn_left && is_dragging) {
        int new_x = mouse_px - drag_off_x;
        int new_y = mouse_py - drag_off_y;

        if (new_x < 4) new_x = 4;
        if (new_x + 640 > fb_width - 4) new_x = fb_width - 640 - 4;
        if (new_y < 24) new_y = 24;
        if (new_y + 400 > fb_height - 4) new_y = fb_height - 400 - 4;

        int d_x = new_x - win_x; if (d_x < 0) d_x = -d_x;
        int d_y = new_y - win_y; if (d_y < 0) d_y = -d_y;

        if (d_x >= 2 || d_y >= 2) {
            clear_exposed_rects(win_x - 4, win_y - 24, new_x - 4, new_y - 24, 648, 428, 0x141923);
            win_x = new_x;
            win_y = new_y;
            render_window_frame(win_x, win_y);
            for (int y = 0; y < VGA_ROWS; y++) {
                for (int x = 0; x < VGA_COLS; x++) {
                    vga_sync_cell(x, y);
                }
            }
        }
    }

    prev_btn_left = mouse_btn_left;
}

static void render_mouse(void) {
    draw_mouse_cursor(0, 0, 1);
    check_mouse_gui_click();
    draw_mouse_cursor(mouse_px, mouse_py, 0);

    int cell_x = (mouse_px - win_x) / 8;
    int cell_y = (mouse_py - win_y) / 16;
    if (cell_x < 0) cell_x = 0; if (cell_x >= VGA_COLS) cell_x = VGA_COLS - 1;
    if (cell_y < 0) cell_y = 0; if (cell_y >= VGA_ROWS) cell_y = VGA_ROWS - 1;

    char mbuf[22];
    mbuf[0] = '['; mbuf[1] = 'M'; mbuf[2] = ':'; mbuf[3] = ' ';
    mbuf[4] = '0' + (cell_x / 10); mbuf[5] = '0' + (cell_x % 10);
    mbuf[6] = ',';
    mbuf[7] = '0' + (cell_y / 10); mbuf[8] = '0' + (cell_y % 10);
    mbuf[9] = ' '; mbuf[10] = '|'; mbuf[11] = ' ';
    mbuf[12] = 'L'; mbuf[13] = ':'; mbuf[14] = mouse_btn_left ? '1' : '0';
    mbuf[15] = ' ';
    mbuf[16] = 'R'; mbuf[17] = ':'; mbuf[18] = mouse_btn_right ? '1' : '0';
    mbuf[19] = ']'; mbuf[20] = '\0';
    vga_puts_at(52, 0, mbuf, 0x70);
}

static void mouse_init(void) {
    mouse_wait_write();
    outb(0x64, 0xA8);

    mouse_wait_write();
    outb(0x64, 0x20);
    mouse_wait_read();
    uint8_t status = inb(0x60);
    status |= 0x02;
    status &= ~0x20;

    mouse_wait_write();
    outb(0x64, 0x60);
    mouse_wait_write();
    outb(0x60, status);

    mouse_write(0xF6);
    mouse_read();

    mouse_write(0xF4);
    mouse_read();

    draw_mouse_cursor(mouse_px, mouse_py, 0);
    render_header_menu();
}

// --- 23. PIC & PIT ---
static void pic_init(void) {
    outb(0x20, 0x11); io_wait();
    outb(0xA0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait();
    outb(0xA1, 0x28); io_wait();
    outb(0x21, 0x04); io_wait();
    outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait();
    outb(0xA1, 0x01); io_wait();

    outb(0x21, 0xF8);
    outb(0xA1, 0xEF);
}

static void pit_init(uint32_t hz) {
    uint32_t divisor = 1193182 / hz;
    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}

// --- 24. Клавиатурные таблицы скан-кодов ---
static const char kbd_us[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
  '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,  'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',   0,
  '*',   0, ' '
};

static const char kbd_us_shifted[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
  '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,  'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '\"', '~',
    0,  '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',   0,
  '*',   0, ' '
};

static volatile int shift_active = 0;
static volatile int ctrl_active = 0;
static char input_buffer[64];
static int input_pos = 0;

static void execute_command(void) {
    input_buffer[input_pos] = '\0';
    vga_putc_color('\n', VGA_ATTR_DEFAULT);

    if (input_pos == 0) {
    } else if (strcmp(input_buffer, "help") == 0) {
        vga_puts("Aporia OS Commands:\n", 0x0E);
        vga_puts("  lspci              - Scan and enumerate PCI hardware bus\n", 0x0A);
        vga_puts("  sysinfo            - System uptime and kernel diagnostics\n", 0x0A);
        vga_puts("  tasks              - List processes and reused memory slots\n", 0x07);
        vga_puts("  ls / ext2_ls       - List files in ext2 root directory\n", 0x0A);
        vga_puts("  cat <file>         - Display file contents from ext2\n", 0x0A);
        vga_puts("  exec <file>        - Run ELF32 binary from ext2 in Ring 3\n", 0x0A);
        vga_puts("  nano <file>        - Interactive text editor (Ctrl+S, Ctrl+X)\n", 0x07);
        vga_puts("  asm <src> <dst>    - In-kernel x86 assembler\n", 0x07);
        vga_puts("  xxd <lba>          - Safe 32-byte disk dump\n", 0x07);
        vga_puts("  time               - Hardware CMOS RTC clock\n", 0x07);
        vga_puts("  clear              - Wipe screen\n", 0x07);
    } else if (strcmp(input_buffer, "clear") == 0) {
        vga_clear();
    } else if (strcmp(input_buffer, "time") == 0) {
        cmd_time();
    } else if (strcmp(input_buffer, "lspci") == 0) {
        cmd_lspci();
    } else if (strcmp(input_buffer, "sysinfo") == 0) {
        cmd_sysinfo();
    } else if (strcmp(input_buffer, "ls") == 0 || strcmp(input_buffer, "ext2_ls") == 0) {
        cmd_ext2_ls();
    } else if (strncmp(input_buffer, "cat ", 4) == 0) {
        cmd_ext2_cat(&input_buffer[4]);
    } else if (strncmp(input_buffer, "ext2_cat ", 9) == 0) {
        cmd_ext2_cat(&input_buffer[9]);
    } else if (strncmp(input_buffer, "nano ", 5) == 0) {
        char target_fname[16]; int k = 0;
        while (input_buffer[5 + k] && k < 15) { target_fname[k] = input_buffer[5 + k]; k++; }
        target_fname[k] = '\0';
        input_pos = 0; input_buffer[0] = '\0';
        nano_open(target_fname);
        return;
    } else if (strncmp(input_buffer, "asm ", 4) == 0) {
        char *src = &input_buffer[4]; char *dst = src;
        while (*dst && *dst != ' ') dst++;
        if (*dst == ' ') { *dst = '\0'; dst++; cmd_asm(src, dst); }
    } else if (strncmp(input_buffer, "xxd ", 4) == 0) {
        uint32_t lba = parse_dec(&input_buffer[4]);
        cmd_xxd(lba);
    } else if (strcmp(input_buffer, "tasks") == 0) {
        print_tasks_table();
    } else if (strncmp(input_buffer, "exec ", 5) == 0) {
        fs_exec(&input_buffer[5]);
    } else {
        vga_puts("Unknown command: ", 0x0C);
        vga_puts(input_buffer, 0x0C);
        vga_putc_color('\n', VGA_ATTR_DEFAULT);
    }

    input_pos = 0;
    input_buffer[0] = '\0';
    vga_puts("gamer@os> ", VGA_ATTR_PROMPT);
}

// --- 25. Диспетчеры исключений и аппаратных прерываний ---
uint32_t isr_dispatch(struct registers *r) {
    if ((r->cs & 3) == 3) {
        vga_puts("\n[SECURITY] Process PID ", 0x0C);
        vga_print_dec(tasks[current_task].id, 0x0E);
        vga_puts(" killed: Vector ", 0x0C);
        vga_print_dec(r->int_no, 0x0E);
        vga_puts(".\n", 0x0C);

        tasks[current_task].state = TASK_STATE_DEAD;
        tasks[current_task].exit_code = 128 + r->int_no;
        vga_puts("gamer@os> ", VGA_ATTR_PROMPT);
        return schedule((uint32_t)r);
    }

    vga_puts("\n\n================ [ FATAL KERNEL PANIC ] ================\n", VGA_ATTR_PANIC);
    vga_puts("Vector     : ", VGA_ATTR_PANIC); vga_print_dec(r->int_no, 0x4E);
    vga_puts("\nError Code : ", VGA_ATTR_PANIC); vga_print_hex32(r->err_code, 0x4E);
    vga_puts("\nEIP        : ", VGA_ATTR_PANIC); vga_print_hex32(r->eip, 0x4E);
    vga_puts("\n=========================================================\n", VGA_ATTR_PANIC);
    while (1) { asm volatile ("cli; hlt"); }
}

void irq_dispatch(struct registers *r) {
    if (r->int_no >= 40) outb(0xA0, 0x20);
    outb(0x20, 0x20);
    if (r->int_no == 32) timer_ticks++;

    if (r->int_no == 33) {
        uint8_t scancode = inb(0x60);
        if (scancode == 0x2A || scancode == 0x36) shift_active = 1;
        else if (scancode == 0xAA || scancode == 0xB6) shift_active = 0;
        else if (scancode == 0x1D) ctrl_active = 1;
        else if (scancode == 0x9D) ctrl_active = 0;
        else if (!(scancode & 0x80)) {
            char ch = shift_active ? kbd_us_shifted[scancode] : kbd_us[scancode];

            if (ctrl_active && (ch == 'c' || ch == 'C')) {
                int killed = 0;
                for (int k = 2; k < num_tasks; k++) {
                    if (tasks[k].is_user && tasks[k].state == TASK_STATE_RUNNING) {
                        tasks[k].state = TASK_STATE_DEAD;
                        tasks[k].exit_code = 130;
                        killed++;
                        int row = (tasks[k].id == 2) ? 23 : 24;
                        vga_puts_at(0, row, "[Signal ]: KILLED by SIGINT (Ctrl+C)                    ", 0x0C);
                    }
                }
                vga_puts("\n^C\n", 0x0C);
                if (killed > 0) vga_puts("[SIGINT] Killed active user process.\n", 0x0A);
                vga_puts("gamer@os> ", VGA_ATTR_PROMPT);
                return;
            }

            if (editor_active) {
                if (ctrl_active && (ch == 's' || ch == 'S')) nano_save();
                else if (ctrl_active && (ch == 'x' || ch == 'X')) nano_exit();
                else if (ch == '\b') {
                    if (editor_len > 0) { editor_len--; nano_render(); }
                } else if (ch && editor_len < 510) {
                    editor_buffer[editor_len++] = ch; nano_render();
                }
                return;
            }

            if (ch == '\n') execute_command();
            else if (ch == '\b') {
                if (input_pos > 0) {
                    input_pos--; vga_putc_color('\b', VGA_ATTR_DEFAULT);
                }
            } else if (ch && input_pos < 63) {
                input_buffer[input_pos++] = ch;
                vga_putc_color(ch, VGA_ATTR_DEFAULT);
            }
        }
    }
    else if (r->int_no == 44) {
        uint8_t status = inb(0x64);
        if (status & 0x01) {
            uint8_t data = inb(0x60);
            if (mouse_cycle == 0) {
                if (data & 0x08) { mouse_bytes[0] = data; mouse_cycle++; }
            } else if (mouse_cycle == 1) {
                mouse_bytes[1] = data; mouse_cycle++;
            } else if (mouse_cycle == 2) {
                mouse_bytes[2] = data; mouse_cycle = 0;

                uint8_t flags = mouse_bytes[0];
                int dx = mouse_bytes[1];
                int dy = mouse_bytes[2];

                if (flags & 0x10) dx |= 0xFFFFFF00;
                if (flags & 0x20) dy |= 0xFFFFFF00;

                mouse_btn_left  = (flags & 0x01) ? 1 : 0;
                mouse_btn_right = (flags & 0x02) ? 1 : 0;

                mouse_px += dx;
                mouse_py -= dy;

                if (mouse_px < 0) mouse_px = 0;
                if (mouse_px >= fb_width - 12) mouse_px = fb_width - 13;
                if (mouse_py < 0) mouse_py = 0;
                if (mouse_py >= fb_height - 18) mouse_py = fb_height - 19;

                render_mouse();
            }
        }
    }
}

// --- 26. Точка входа kernel_main ---
void kernel_main(void) {
    fb_pitch  = *(uint16_t *)(0x7E00 + 16);
    fb_width  = *(uint16_t *)(0x7E00 + 18);
    fb_height = *(uint16_t *)(0x7E00 + 20);
    fb_bpp    = *(uint8_t  *)(0x7E00 + 25);
    fb_phys   = *(uint32_t *)(0x7E00 + 40);
    fb_base   = (volatile uint8_t *)fb_phys;

    gdt_init();

    idtp.limit = (sizeof(struct idt_entry) * 256) - 1;
    idtp.base  = (uint32_t)&idt;
    for (int i = 0; i < 256; i++) idt_set_gate(i, 0, 0, 0);

    #define SET_GATE(n, fn) idt_set_gate(n, (uint32_t)fn, 0x08, 0x8E)
    SET_GATE(0, isr0);   SET_GATE(1, isr1);   SET_GATE(2, isr2);   SET_GATE(3, isr3);
    SET_GATE(4, isr4);   SET_GATE(5, isr5);   SET_GATE(6, isr6);   SET_GATE(7, isr7);
    SET_GATE(8, isr8);   SET_GATE(9, isr9);   SET_GATE(10, isr10); SET_GATE(11, isr11);
    SET_GATE(12, isr12); SET_GATE(13, isr13); SET_GATE(14, isr14); SET_GATE(15, isr15);
    SET_GATE(16, isr16); SET_GATE(17, isr17); SET_GATE(18, isr18); SET_GATE(19, isr19);
    SET_GATE(20, isr20); SET_GATE(21, isr21); SET_GATE(22, isr22); SET_GATE(23, isr23);
    SET_GATE(24, isr24); SET_GATE(25, isr25); SET_GATE(26, isr26); SET_GATE(27, isr27);
    SET_GATE(28, isr28); SET_GATE(29, isr29); SET_GATE(30, isr30); SET_GATE(31, isr31);

    SET_GATE(32, irq0);  SET_GATE(33, irq1);  SET_GATE(34, irq2);  SET_GATE(35, irq3);
    SET_GATE(36, irq4);  SET_GATE(37, irq5);  SET_GATE(38, irq6);  SET_GATE(39, irq7);
    SET_GATE(40, irq8);  SET_GATE(41, irq9);  SET_GATE(42, irq10); SET_GATE(43, irq11);
    SET_GATE(44, irq12); SET_GATE(45, irq13); SET_GATE(46, irq14); SET_GATE(47, irq15);

    idt_set_gate(128, (uint32_t)isr128, 0x08, 0xEE);

    idt_load((uint32_t)&idtp);
    pic_init();
    pit_init(100);

    mmu_and_sse_init();
    heap_init();

    render_desktop();
    vga_clear();
    mouse_init();
    pci_scan();

    tasks[0].id = 0;
    for (int k = 0; k < 15; k++) tasks[0].name[k] = "Kernel-Shell"[k];
    tasks[0].name[12] = '\0';
    tasks[0].slices = 0;
    tasks[0].is_user = 0;
    tasks[0].state = TASK_STATE_RUNNING;
    tasks[0].kstack_top = 0x90000;
    tasks[0].cr3 = (uint32_t)kernel_page_directory;
    tasks[0].exit_code = 0;
    num_tasks = 1;

    uint32_t kstack_top = (uint32_t)kmalloc(STACK_SIZE) + STACK_SIZE;
    uint32_t *stack = (uint32_t *)kstack_top;
    *(--stack) = 0x00000202;
    *(--stack) = 0x00000008;
    *(--stack) = (uint32_t)kernel_worker;
    *(--stack) = 0;
    *(--stack) = 32;
    *(--stack) = 0; *(--stack) = 0; *(--stack) = 0; *(--stack) = 0;
    *(--stack) = 0; *(--stack) = 0; *(--stack) = 0; *(--stack) = 0;
    *(--stack) = 0x10; *(--stack) = 0x10; *(--stack) = 0x10; *(--stack) = 0x10;

    tasks[1].esp = (uint32_t)stack;
    tasks[1].kstack_top = kstack_top;
    tasks[1].cr3 = (uint32_t)kernel_page_directory;
    tasks[1].id = 1;
    for (int k = 0; k < 15; k++) tasks[1].name[k] = "KWorker-Ring0"[k];
    tasks[1].name[13] = '\0';
    tasks[1].slices = 0;
    tasks[1].is_user = 0;
    tasks[1].state = TASK_STATE_RUNNING;
    tasks[1].exit_code = 0;
    num_tasks = 2;

    scheduler_active = 1;
    asm volatile ("sti");

    vga_puts("=== Aporia OS v1.0.0 Workstation Online ===\n", VGA_ATTR_BANNER);
    vga_puts("VESA VBE: 1024x768 | PCI Bus Active | ext2 VFS Mounted\n", 0x0E);
    vga_puts("Try: 'lspci', 'sysinfo', 'ls', 'cat hello.txt', 'exec elfapp'\n\n", 0x07);
    vga_puts("gamer@os> ", VGA_ATTR_PROMPT);

    while (1) {
        asm volatile ("hlt");
    }
}
