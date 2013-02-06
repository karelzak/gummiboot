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

test-disk:
	./test-create-disk.sh

test: gummiboot$(MACHINE_TYPE_NAME).efi test-disk
	qemu-kvm -m 256 -L /usr/lib/qemu-bios -snapshot test-disk
