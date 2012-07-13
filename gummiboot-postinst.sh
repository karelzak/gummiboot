#!/bin/bash

# This is a script to be called automatically by new-kernel-pkg as
# /etc/kernel/postinst.d/gummiboot-postinst.sh

# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation; either version 2.1 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# Lesser General Public License for more details.
#
# Copyright (C) 2012 Kay Sievers <kay.sievers@vrfy.org>
# Copyright (C) 2012 Harald Hoyer <harald@redhat.com>

if (( $# != 2 )); then
	echo "Usage: $0 <version> <vmlinuz>" >&2
	exit 1
fi

version="$1"
vmlinuz="$2"

if ! [[ -f $vmlinuz ]]; then
	echo "Can't find file $vmlinuz" >&2
	exit 1
fi

if [[ -f /boot/loader/loader.conf ]]; then
	efidir="/boot"
elif [[ -f /boot/efi/loader/loader.conf ]]; then
	efidir="/boot/efi"
fi

if ! [[ $efidir ]] ; then
	echo "Can't install new kernel for gummiboot: no "loader/loader.conf" found!" >&2
	exit 1
fi

if [[ -f ${vmlinuz/vmlinuz/initrd} ]]; then
	initrd=${vmlinuz/vmlinuz/initrd}
elif [[ -f ${vmlinuz/vmlinuz/initrd}.img ]]; then
	initrd=${vmlinuz/vmlinuz/initrd}.img
elif [[ -f ${vmlinuz/vmlinuz/initramfs}.img ]]; then
	initrd=${vmlinuz/vmlinuz/initramfs}.img
fi

if [[ -f /etc/kernel-cmdline ]]; then
	while read line; do
		options+="$line "
	done < /etc/kernel-cmdline
fi

if ! [[ $options ]]; then
	echo "Can't load default kernel command line parameters from /etc/kernel-cmdline!" >&2
fi

[[ -f /etc/os-release ]] && . /etc/os-release

if ! [[ $ID ]]; then
	echo "Can't determine the ID of your distribution. Fix /etc/os-release!" >&2
	exit 1
fi

if ! [[ $PRETTY_NAME ]]; then
	PRETTY_NAME="$NAME $VERSION"
fi

[[ -f /etc/machine-id ]] && read MACHINE_ID < /etc/machine-id

if ! [[ $MACHINE_ID ]]; then
	echo "Can't determine your machine id. Fix /etc/machine-id!" >&2
fi

rootdev=$(while read a a a a mp a a a dev a; do if [[ $mp = "/" ]]; then echo $dev;break;fi;done < /proc/self/mountinfo)
if [[ $rootdev ]]; then
	rootlabel=$(blkid -p -o udev -u filesystem $rootdev | while read line; do if [[ $line == ID_FS_LABEL* ]]; then echo ${line##ID_FS_LABEL=}; break; fi; done)
fi
mkdir -p "${efidir}/$ID/$MACHINE_ID"

# --reflink=auto does COW, so if it is on the same filesystem, data is only kept once
# until someone modifies the file
cp --reflink=auto --preserve "$vmlinuz" "${efidir}/$ID/$MACHINE_ID/"
[[ $initrd ]] && cp --reflink=auto --preserve "$initrd" "${efidir}/$ID/$MACHINE_ID/"

{
	echo "title $PRETTY_NAME ($version) $rootlabel $rootdev ${MACHINE_ID:0:8}"

	echo "options $options"

	echo "linux /$ID/$MACHINE_ID/${vmlinuz##*/}"

	[[ $initrd ]] && echo "initrd /$ID/$MACHINE_ID/${initrd##*/}"

} > "${efidir}/loader/entries/$ID-$version-$MACHINE_ID.conf"

# now cleanup the old entries and files, for which no /lib/modules/$version exists
(
	cd ${efidir}/loader/entries
	for conf in $ID-*-$MACHINE_ID.conf; do
		version=${conf##$ID-}
		version=${version%%-$MACHINE_ID.conf}
		[[ $version ]] || continue
		[[ -d /lib/modules/$version/kernel ]] && continue
		rm -f "$conf"
		rm -f "${efidir}/$ID/$MACHINE_ID/*$version*"
	done
)

exit 0
