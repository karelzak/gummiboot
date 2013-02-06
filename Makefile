VERSION=17

ARCH=$(shell $(CC) -dumpmachine | sed "s/\(-\).*$$//")
LIBDIR=$(shell echo $$(cd /usr/lib/$$(gcc -print-multi-os-directory); pwd))
LIBEFIDIR=$(or $(wildcard $(LIBDIR)/gnuefi), $(LIBDIR))

ifeq ($(ARCH),i686)
	ARCH=ia32
	MACHINE_TYPE_NAME=ia32
endif

ifeq ($(ARCH),x86_64)
	MACHINE_TYPE_NAME=x64
	ARCH_CFLAGS= \
		-DEFI_FUNCTION_WRAPPER \
		-mno-red-zone
endif

CPPFLAGS = \
	-I. \
	-I/usr/include/efi \
	-I/usr/include/efi/$(ARCH)

CFLAGS = \
	-DVERSION=$(VERSION) \
	-Wall \
	-Wextra \
	-nostdinc \
	-ggdb -O0 \
	-fpic \
	-fshort-wchar \
	-nostdinc \
	-ffreestanding \
	-fno-strict-aliasing \
	-fno-stack-protector \
	-Wsign-compare \
	$(ARCH_CFLAGS)

LDFLAGS = -T $(LIBEFIDIR)/elf_$(ARCH)_efi.lds \
	-shared \
	-Bsymbolic \
	-nostdlib \
	-znocombreloc \
	-L $(LIBDIR) \
	$(LIBEFIDIR)/crt0-efi-$(ARCH).o \
	$(shell $(CC) -print-libgcc-file-name)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

gummiboot$(MACHINE_TYPE_NAME).efi: gummiboot.so
	objcopy -j .text -j .sdata -j .data -j .dynamic \
	  -j .dynsym -j .rel -j .rela -j .reloc -j .eh_frame \
	  --target=efi-app-$(ARCH) $< $@

gummiboot.so: gummiboot.o
	$(LD) $(LDFLAGS) gummiboot.o -o $@ -lefi -lgnuefi

gummiboot.o: gummiboot.c Makefile

clean:
	rm -f gummiboot.o gummiboot.so gummiboot$(MACHINE_TYPE_NAME).efi

tar:
	git archive --format=tar --prefix=gummiboot-$(VERSION)/ $(VERSION) | xz > gummiboot-$(VERSION).tar.xz

test: gummiboot$(MACHINE_TYPE_NAME).efi
	mkdir -p /boot/EFI/gummiboot/
	cp -v gummiboot$(MACHINE_TYPE_NAME).efi /boot/EFI/gummiboot/
	@# unmount to sync EFI partition to disk
	sync
	umount /boot
	echo 3 > /proc/sys/vm/drop_caches
	@# run QEMU with UEFI firmware
	qemu-kvm -m 512 -L /usr/lib/qemu-bios -snapshot /dev/sda
