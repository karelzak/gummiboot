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
	echo "Usage: $0 <KERNEL_VERSION> <KERNEL_IMAGE>" >&2
	exit 1
fi

KERNEL_VERSION="$1"
KERNEL_IMAGE="$2"

if ! [[ -f $KERNEL_IMAGE ]]; then
	echo "Can't find file $KERNEL_IMAGE" >&2
	exit 1
fi

if [[ -d /boot/loader/entries ]]; then
	EFI_DIR="/boot"
elif [[ -f /boot/efi/loader/entries ]]; then
	EFI_DIR="/boot/efi"
fi

if ! [[ $EFI_DIR ]] ; then
	echo "Can't install new kernel for gummiboot: no "loader/loader.conf" found!" >&2
	exit 1
fi

if [[ -f ${KERNEL_IMAGE/vmlinuz/initrd} ]]; then
	INITRD_IMAGE=${KERNEL_IMAGE/vmlinuz/initrd}
elif [[ -f ${KERNEL_IMAGE/vmlinuz/initrd}.img ]]; then
	INITRD_IMAGE=${KERNEL_IMAGE/vmlinuz/initrd}.img
elif [[ -f ${KERNEL_IMAGE/vmlinuz/initramfs}.img ]]; then
	INITRD_IMAGE=${KERNEL_IMAGE/vmlinuz/initramfs}.img
fi

if [[ -f /etc/kernel-cmdline ]]; then
	while read line; do
		BOOT_OPTIONS+="$line "
	done < /etc/kernel-cmdline
fi

if ! [[ $BOOT_OPTIONS ]]; then
	echo "Can't load default kernel command line parameters from /etc/kernel-cmdline!" >&2
fi

[[ -f /etc/os-release ]] && . /etc/os-release

if ! [[ $ID ]]; then
	echo "Can't determine the ID of your distribution. Fix /etc/os-release!" >&2
	exit 1
fi

[[ -f /etc/machine-id ]] && read MACHINE_ID < /etc/machine-id

if ! [[ $MACHINE_ID ]]; then
	echo "Can't determine your machine id. Fix /etc/machine-id!" >&2
fi

ROOT_DEV=$(while read a a a a mp a a a dev a; do
    if [[ $mp = "/" ]]; then
        echo $dev
        break
    fi
    done < /proc/self/mountinfo)

if [[ $ROOT_DEV ]]; then
	ROOT_LABEL=$(blkid -p -o udev -u filesystem $ROOT_DEV |
            while read line; do
                if [[ $line == ID_FS_LABEL* ]]; then
                    echo ${line##ID_FS_LABEL=}
                    break
                fi
                done)
fi

mkdir -p "${EFI_DIR}/${ID}/${MACHINE_ID}"

cp --preserve "$KERNEL_IMAGE" "${EFI_DIR}/${ID}/${MACHINE_ID}/"
[[ $INITRD_IMAGE ]] && cp --preserve "$INITRD_IMAGE" "${EFI_DIR}/${ID}/${MACHINE_ID}/"

{
	echo "title $NAME $VERSION_ID ($KERNEL_VERSION) $ROOT_LABEL ${ROOT_DEV##/dev/} ${MACHINE_ID:0:8}"

	echo "options $BOOT_OPTIONS"

	echo "linux /$ID/$MACHINE_ID/${KERNEL_IMAGE##*/}"

	[[ $INITRD_IMAGE ]] && echo "initrd /${ID}/${MACHINE_ID}/${INITRD_IMAGE##*/}"

} > "${EFI_DIR}/loader/entries/${ID}-${KERNEL_VERSION}-${MACHINE_ID}.conf"

if ! [[ -f ${EFI_DIR}/loader/loader.conf ]]; then
    {
        echo "default *$ID*"
    } > "${EFI_DIR}/loader/loader.conf"
fi

# now cleanup the old entries and files, for which no /lib/modules/$KERNEL_VERSION exists
(
	cd ${EFI_DIR}/loader/entries
	for conf in ${ID}-*-${MACHINE_ID}.conf; do
		KERNEL_VERSION=${conf##$ID-}
		KERNEL_VERSION=${KERNEL_VERSION%%-$MACHINE_ID.conf}
		[[ $KERNEL_VERSION ]] || continue
		[[ -d /lib/modules/${KERNEL_VERSION}/kernel ]] && continue
		rm -f "$conf"
		rm -f "${EFI_DIR}/${ID}/${MACHINE_ID}/*${KERNEL_VERSION}*"
	done
)

exit 0
