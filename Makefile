# Makefile

SHELL := /bin/bash

TARGET      ?= i686-elf
CC          := $(TARGET)-gcc
AS          := nasm
GRUB_FILE   := grub-file
GRUB_MKRESCUE := grub-mkrescue
QEMU        := qemu-system-i386
OBJCOPY     ?= $(TARGET)-objcopy

USER_PROGRAMS := demo counter shell fstest
USER_ELFS := $(USER_PROGRAMS:%=build/user/%.elf)
USER_BLOBS := $(USER_PROGRAMS:%=build/user/%_elf_blob.o)
USER_LIB_SRCS := user/lib/toyix.c
USER_LIB_OBJS := $(USER_LIB_SRCS:user/lib/%.c=build/user/lib/%.o)

CFLAGS := -std=gnu11 \
          -ffreestanding \
          -O2 \
          -Wall \
          -Wextra \
          -Werror \
          -m32 \
          -fno-stack-protector \
          -fno-pic \
          -fno-pie \
          -Iinclude \
          -I. \
          $(CFLAGS_EXTRA)

LDFLAGS := -T linker.ld \
           -ffreestanding \
           -O2 \
           -nostdlib \
           -lgcc

USER_CFLAGS := \
          -std=gnu11 \
          -ffreestanding \
          -fno-builtin \
          -fno-stack-protector \
          -fno-pic \
          -fno-pie \
          -fno-asynchronous-unwind-tables \
          -fno-unwind-tables \
          -m32 \
          -march=i686 \
          -O2 \
          -Wall \
          -Wextra \
          -Iuser/include

USER_LDFLAGS := \
           -nostdlib \
           -ffreestanding \
           -m32 \
           -Wl,-T,user/linker.ld \
           -Wl,--build-id=none

OBJS := \
    build/arch/x86/boot.o \
    build/arch/x86/gdt.o \
    build/arch/x86/gdt_flush.o \
    build/arch/x86/idt.o \
    build/arch/x86/interrupts.o \
    build/arch/x86/isr.o \
    build/arch/x86/irq.o \
    build/arch/x86/paging_asm.o \
    build/arch/x86/paging.o \
    build/arch/x86/pic.o \
    build/arch/x86/pit.o \
    build/arch/x86/sched_interrupt.o \
    build/arch/x86/syscall.o \
    build/arch/x86/user_enter.o \
    build/arch/x86/vmm.o \
    build/kernel/address_space.o \
    build/kernel/elf_loader.o \
    build/kernel/kmain.o \
    build/kernel/idle.o \
    build/kernel/console.o \
    build/kernel/heap.o \
    build/kernel/monitor.o \
    build/kernel/panic.o \
    build/kernel/pmm.o \
    build/kernel/process.o \
    build/kernel/program.o \
    build/kernel/sync.o \
    build/kernel/syscall.o \
    build/kernel/terminal.o \
    build/kernel/thread.o \
    build/kernel/usercopy.o \
    build/kernel/usermode.o \
    build/kernel/vfs.o \
    build/kernel/vmem.o \
    build/kernel/wait_queue.o \
    build/kernel/lib/mem.o \
    build/drivers/console/serial.o \
    build/drivers/console/vga_text.o \
    build/drivers/input/keyboard.o \
    $(USER_BLOBS)

.PHONY: all clean iso run test test-exception test-page-fault user-programs user-blobs list-user-programs readelf-user FORCE

all: build/kernel.elf

build/.cflags: FORCE
	@mkdir -p build
	@if [[ ! -f $@ ]] || [[ "$$(cat $@)" != "$(CFLAGS_EXTRA)" ]]; then \
		printf '%s\n' "$(CFLAGS_EXTRA)" > $@; \
	fi

build/arch/x86/paging.o: arch/x86/paging.c build/.cflags
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/user:
	@mkdir -p build/user

build/user/lib:
	@mkdir -p build/user/lib

build/%.o: %.asm
	@mkdir -p $(dir $@)
	$(AS) -f elf32 $< -o $@

build/%.o: %.c build/.cflags
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/user/crt0.o: user/crt0.S | build/user
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/lib/%.o: user/lib/%.c user/include/toyix.h user/include/toyix_syscall.h | build/user/lib
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/%.o: user/%.c user/include/toyix.h user/include/toyix_syscall.h | build/user
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/%.elf: build/user/crt0.o build/user/%.o $(USER_LIB_OBJS) user/linker.ld | build/user
	$(CC) $(USER_LDFLAGS) \
		-Wl,-Map,build/user/$*.map \
		build/user/crt0.o build/user/$*.o $(USER_LIB_OBJS) \
		-o $@

build/user/%_elf_blob.o: build/user/%.elf | build/user
	$(OBJCOPY) \
		-I binary \
		-O elf32-i386 \
		-B i386 \
		--rename-section .data=.rodata.user_$*,alloc,load,readonly,data,contents \
		--redefine-sym _binary_build_user_$*_elf_start=user_$*_elf_start \
		--redefine-sym _binary_build_user_$*_elf_end=user_$*_elf_end \
		$< $@

user-programs: $(USER_LIB_OBJS) $(USER_ELFS)

user-blobs: $(USER_BLOBS)

list-user-programs:
	@echo "$(USER_PROGRAMS)"

readelf-user: $(USER_ELFS)
	@for elf in $(USER_ELFS); do \
		echo "==== $$elf ===="; \
		i686-elf-readelf -h $$elf | grep -E "Class:|Data:|Type:|Machine:|Entry point"; \
		i686-elf-readelf -l $$elf | grep LOAD; \
	done

build/kernel.elf: $(OBJS) linker.ld
	$(CC) $(LDFLAGS) $(OBJS) -o $@

iso: build/kernel.elf grub.cfg
	@mkdir -p build/iso/boot/grub
	cp build/kernel.elf build/iso/boot/kernel.elf
	cp grub.cfg build/iso/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o build/toyix.iso build/iso

run: iso
	$(QEMU) -cdrom build/toyix.iso -serial stdio

test: iso $(USER_LIB_OBJS)
	@mkdir -p build
	@rm -f build/test.log
	@timeout 10s $(QEMU) \
		-boot d \
		-cdrom build/toyix.iso \
		-display none \
		-monitor none \
		-serial file:build/test.log \
		-no-reboot \
		2>/dev/null || true
	@echo "Captured normal boot log at build/test.log"

test-exception:
	$(MAKE) clean
	$(MAKE) iso CFLAGS_EXTRA=-DTOYIX_TRIGGER_TEST_EXCEPTION
	@mkdir -p build
	@rm -f build/exception.log
	@timeout 5s $(QEMU) \
		-boot d \
		-cdrom build/toyix.iso \
		-display none \
		-monitor none \
		-serial file:build/exception.log \
		-no-reboot \
		2>/dev/null || true
	@echo "Captured exception log at build/exception.log"

test-page-fault:
	$(MAKE) clean
	$(MAKE) iso CFLAGS_EXTRA=-DTOYIX_TRIGGER_PAGE_FAULT
	@mkdir -p build
	@rm -f build/pagefault.log
	@timeout 5s $(QEMU) \
		-boot d \
		-cdrom build/toyix.iso \
		-display none \
		-monitor none \
		-serial file:build/pagefault.log \
		-no-reboot \
		2>/dev/null || true
	@echo "Captured page-fault log at build/pagefault.log"

clean:
	rm -rf build
