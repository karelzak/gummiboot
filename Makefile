VERSION=19

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
	$(LIBEFIDIR)/crt0-efi-$(ARCH).o

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

all: gummiboot$(MACHINE_TYPE_NAME).efi gummiboot-setup

gummiboot$(MACHINE_TYPE_NAME).efi: gummiboot.so
	objcopy -j .text -j .sdata -j .data -j .dynamic \
	  -j .dynsym -j .rel -j .rela -j .reloc -j .eh_frame \
	  --target=efi-app-$(ARCH) $< $@

gummiboot.so: gummiboot.o
	$(LD) $(LDFLAGS) gummiboot.o -o $@ -lefi -lgnuefi \
	$(shell $(CC) -print-libgcc-file-name)

gummiboot.o: gummiboot.c Makefile

gummiboot-setup: setup.c
	$(CC) -O0 -g -Wall -Wextra -D_GNU_SOURCE `pkg-config --cflags --libs blkid` $^ -o $@

clean:
	rm -f gummiboot.o gummiboot.so gummiboot$(MACHINE_TYPE_NAME).efi

install:
	mkdir -p $(DESTDIR)/usr/bin/
	cp gummiboot-setup $(DESTDIR)/usr/bin
	mkdir -p $(DESTDIR)/usr/lib/gummiboot/
	cp gummiboot$(MACHINE_TYPE_NAME).efi $(DESTDIR)/usr/lib/gummiboot/

tar:
	git archive --format=tar --prefix=gummiboot-$(VERSION)/ $(VERSION) | xz > gummiboot-$(VERSION).tar.xz

test-disk: gummiboot$(MACHINE_TYPE_NAME).efi test-create-disk.sh
	./test-create-disk.sh

test: test-disk
	qemu-kvm -m 256 -L /usr/lib/qemu-bios -snapshot test-disk
