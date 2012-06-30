VERSION=1

ARCH=$(shell $(CC) -dumpmachine | sed "s/\(-\).*$$//")
LIBDIR=$(shell echo $$(cd /usr/lib/$$(gcc -print-multi-os-directory); pwd))

CPPFLAGS = \
	-I. \
	-I/usr/include/efi \
	-I/usr/include/efi/$(ARCH)

CFLAGS = \
	-DVERSION=$(VERSION) \
	-Wall \
	-g -O0 \
	-fpic \
	-fshort-wchar \
	-ffreestanding \
	-DEFI_FUNCTION_WRAPPER

LDFLAGS = -T $(LIBDIR)/gnuefi/elf_$(ARCH)_efi.lds \
	-shared \
	-Bsymbolic \
	-nostdlib \
	-znocombreloc \
	-L $(LIBDIR) \
	$(LIBDIR)/gnuefi/crt0-efi-$(ARCH).o

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

gummiboot.efi: gummiboot.so
	objcopy -j .text -j .sdata -j .data -j .dynamic \
	  -j .dynsym -j .rel -j .rela -j .reloc \
	  --target=efi-app-$(ARCH) $< $@

gummiboot.so: gummiboot.o
	$(LD) $(LDFLAGS) gummiboot.o -o $@ -lefi -lgnuefi

clean:
	rm -f gummiboot.o gummiboot.so gummiboot.efi

test: gummiboot.efi
	@# UUID=677B-ECF2 /boot2 vfat noauto,x-systemd.automount,x-gvfs-hide 1 3
	cp -v gummiboot.efi /boot/EFI/gummiboot/
	@# unmount to sync EFI partition to disk
	sync
	umount /boot
	echo 3 > /proc/sys/vm/drop_caches
	@# run QEMU with UEFI firmware
	qemu-kvm -m 512 -L /usr/lib/qemu-bios -snapshot /dev/sda
