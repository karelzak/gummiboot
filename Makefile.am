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
