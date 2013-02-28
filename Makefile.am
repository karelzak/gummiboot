#
#  This file is part of gummiboot
#
#  Copyright (C) 2013 Karel Zak <kzak@redhat.com>
#
#  gummiboot is free software; you can redistribute it and/or modify it
#  under the terms of the GNU Lesser General Public License as published by
#  the Free Software Foundation; either version 2.1 of the License, or
#  (at your option) any later version.
#
#  gummiboot is distributed in the hope that it will be useful, but
#  WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
#  Lesser General Public License for more details.
#
#  You should have received a copy of the GNU Lesser General Public License
#  along with systemd; If not, see <http://www.gnu.org/licenses/>.
#
ACLOCAL_AMFLAGS = -I m4 ${ACLOCAL_FLAGS}
AM_MAKEFLAGS =

gummibootlibdir = $(libexecdir)/gummiboot

AM_CPPFLAGS = -include config.h
AM_CFLAGS =
AM_LDFLAGS =

EXTRA_DIST = autogen.sh README LICENSE
CLEANFILES =

#
# gummiboot setup binary
#
bin_PROGRAMS = gummiboot
dist_man_MANS = src/setup/gummiboot.1

EXTRA_DIST += src/setup/gummiboot.xml
CLEANFILES += src/setup/gummiboot.1

gummiboot_SOURCES = \
	src/setup/setup.c \
	src/setup/efivars.c \
	src/setup/efivars.h

gummiboot_CFLAGS = \
	$(AM_CFLAGS) \
	-Wno-unused-parameter -D_GNU_SOURCE \
	-DMACHINE_TYPE_NAME=\"$(MACHINE_TYPE_NAME)\" \
	$(BLKID_CFLAGS)

gummiboot_LDADD = \
	$(BLKID_LIBS)

%.1: %.xml
	$(AM_V_GEN)xsltproc -o $@ --nonet \
	  --stringparam man.output.quietly 1 \
	  --stringparam man.th.extra1.suppress 1 \
	  --stringparam man.authors.section.enabled 0 \
	  --stringparam man.copyright.section.enabled 0 \
	  http://docbook.sourceforge.net/release/xsl/current/manpages/docbook.xsl $<

#
# EFI compilation
#
# This part of the build system uses custom make rules and bypass regular
# automake to provide absolute control on compiler and linker flags.
#
efi_loadername = gummiboot$(MACHINE_TYPE_NAME).efi
efi_sources = src/efi/gummiboot.c

efi_cppflags = \
	-I/usr/include/efi \
	-I/usr/include/efi/$(ARCH) \
	-DVERSION=$(VERSION)

efi_cflags = \
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
	-Wsign-compare

if ARCH_X86_64
efi_cflags += \
	-DEFI_FUNCTION_WRAPPER \
	-mno-red-zone
endif

efi_ldflags = \
	-T $(GNUEFI_LDS_DIR)/elf_$(ARCH)_efi.lds \
	-shared \
	-Bsymbolic \
	-nostdlib \
	-znocombreloc \
	$(GNUEFI_LIBS) \
	$(GNUEFI_LDS_DIR)/crt0-efi-$(ARCH).o

efi_objects = $(addprefix $(top_builddir)/,$(efi_sources:.c=.o))
efi_solib = $(top_builddir)/src/efi/gummiboot.so

# automake stuff
gummibootlib_DATA = $(efi_loadername)
CLEANFILES += $(efi_objects) $(efi_solib) $(efi_loadername)
EXTRA_DIST += $(efi_sources)

$(top_builddir)/src/efi/%.o: $(top_srcdir)/src/efi/%.c
	@$(MKDIR_P) $(top_builddir)/src/efi/
	$(AM_V_CC)$(GNUEFI_CC) $(efi_cppflags) $(efi_cflags) -c $< -o $@

$(efi_solib): $(efi_objects)
	$(AM_V_CCLD)$(LD) $(efi_ldflags) $(efi_objects) \
		-o $@ -lefi -lgnuefi $(shell $(CC) -print-libgcc-file-name); \
	nm -D -u $@ | grep ' U ' && exit 1 || :

$(efi_loadername): $(efi_solib)
	$(AM_V_GEN) objcopy -j .text -j .sdata -j .data -j .dynamic \
	  -j .dynsym -j .rel -j .rela -j .reloc -j .eh_frame \
	  --target=efi-app-$(ARCH) $< $@
