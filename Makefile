CC = gcc
LD = ld
NASM = nasm

CFLAGS = -m32 -ffreestanding -fno-pie -fno-stack-protector -fno-builtin \
         -nostdinc -mno-sse -mno-sse2 -mno-mmx -mpreferred-stack-boundary=2 \
         -Wall -Wextra -O2

LDFLAGS_KERNEL = -m elf_i386 -Ttext 0x1000 --oformat binary
LDFLAGS_USER   = -m elf_i386 -T src/user/user_elf.ld

BUILD_DIR = build
IMAGE = $(BUILD_DIR)/os-image.bin

all: $(IMAGE)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/boot.bin: src/boot/boot.asm | $(BUILD_DIR)$(NASM) -f bin $< -o$@

$(BUILD_DIR)/kernel_entry.o: src/kernel/kernel_entry.asm | $(BUILD_DIR)$(NASM) -f elf32 $< -o$@

$(BUILD_DIR)/isr.o: src/kernel/isr.asm | $(BUILD_DIR)$(NASM) -f elf32 $< -o$@

$(BUILD_DIR)/kernel.o: src/kernel/kernel.c \vert{}$(BUILD_DIR)
	$(CC)$(CFLAGS) -c $< -o$@

$(BUILD_DIR)/kernel.bin:$(BUILD_DIR)/kernel_entry.o $(BUILD_DIR)/isr.o $(BUILD_DIR)/kernel.o
	$(LD)$(LDFLAGS_KERNEL) $^ -o$@

$(BUILD_DIR)/user_elf.o: src/user/user_elf.c \vert{}$(BUILD_DIR)
	$(CC)$(CFLAGS) -c $< -o$@

$(BUILD_DIR)/user_elf:$(BUILD_DIR)/user_elf.o src/user/user_elf.ld
	$(LD)$(LDFLAGS_USER) $< -o$@

$(BUILD_DIR)/injector: tools/injector.c \vert{}$(BUILD_DIR)
	gcc -O2 $< -o$@

$(IMAGE): $(BUILD_DIR)/boot.bin $(BUILD_DIR)/kernel.bin $(BUILD_DIR)/user_elf$(BUILD_DIR)/injector
	dd if=/dev/zero of=$(IMAGE) bs=512 count=2880 status=none
	dd if=$(BUILD_DIR)/boot.bin of=$(IMAGE) conv=notrunc status=none
	dd if=$(BUILD_DIR)/kernel.bin of=$(IMAGE) seek=1 conv=notrunc status=none
	./$(BUILD_DIR)/injector $(IMAGE)$(BUILD_DIR)/user_elf elfapp

run: $(IMAGE)
	qemu-system-i386 -drive file=$(IMAGE),format=raw,index=0,media=disk

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all run clean
