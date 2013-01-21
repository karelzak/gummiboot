#!/bin/bash

# This script is called automatically by new-kernel-pkg as
# /etc/kernel/postinst.d/loader-postinst.sh (Fedora)

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
# Copyright (C) 2012 Harald Hoyer <harald@redhat.com>
# Copyright (C) 2012 Kay Sievers <kay.sievers@vrfy.org>

if (( $# != 2 )); then
        echo "Usage: $0 <KERNEL_VERSION> <KERNEL_IMAGE>" >&2
        exit 1
fi

KERNEL_VERSION="$1"
KERNEL_IMAGE="$2"

if ! [[ -f $KERNEL_IMAGE ]]; then
        echo "Can't find file $KERNEL_IMAGE!" >&2
        exit 1
fi

if [[ -d /boot/loader/entries ]]; then
        EFI_DIR="/boot"
elif [[ -d /boot/efi/loader/entries ]]; then
        EFI_DIR="/boot/efi"
fi

if ! [[ $EFI_DIR ]] ; then
        echo "Did not install new kernel and loader entry." >&2
        echo "Please create the directory 'loader/entries/' in your EFI system partition." >&2
        exit 0
fi

if [[ -f ${KERNEL_IMAGE/vmlinuz/initrd} ]]; then
        INITRD_IMAGE=${KERNEL_IMAGE/vmlinuz/initrd}
elif [[ -f ${KERNEL_IMAGE/vmlinuz/initrd}.img ]]; then
        INITRD_IMAGE=${KERNEL_IMAGE/vmlinuz/initrd}.img
elif [[ -f ${KERNEL_IMAGE/vmlinuz/initramfs}.img ]]; then
        INITRD_IMAGE=${KERNEL_IMAGE/vmlinuz/initramfs}.img
fi

if [[ -f /etc/kernel/cmdline ]]; then
        while read line; do
                BOOT_OPTIONS+="$line "
        done < /etc/kernel/cmdline
fi
if ! [[ $BOOT_OPTIONS ]]; then
        echo "Can't determine the kernel command line parameters." >&2
        echo "Please specify the kernel command line in /etc/kernel/cmdline!" >&2
        exit 1
fi

[[ -f /etc/os-release ]] && . /etc/os-release
if ! [[ $ID ]]; then
        echo "Can't determine the name of your distribution. Please create /etc/os-release." >&2
        echo "See http://www.freedesktop.org/software/systemd/man/os-release.html" >&2
        exit 1
fi

[[ -f /etc/machine-id ]] && read MACHINE_ID < /etc/machine-id
if ! [[ $MACHINE_ID ]]; then
        echo "Can't determine your machine id. Please create /etc/machine-id!" >&2
        echo "See http://www.freedesktop.org/software/systemd/man/machine-id.html" >&2
        exit 1
fi

mkdir -p "${EFI_DIR}/${ID}/${MACHINE_ID}"

cp --preserve "$KERNEL_IMAGE" "${EFI_DIR}/${ID}/${MACHINE_ID}/"
[[ $INITRD_IMAGE ]] && cp --preserve "$INITRD_IMAGE" "${EFI_DIR}/${ID}/${MACHINE_ID}/"

{
        echo "title      $PRETTY_NAME"
        echo "version    $KERNEL_VERSION"
        echo "machine-id $MACHINE_ID"
        echo "options    $BOOT_OPTIONS"
        echo "linux      /$ID/$MACHINE_ID/${KERNEL_IMAGE##*/}"
        [[ $INITRD_IMAGE ]] && echo "initrd     /${ID}/${MACHINE_ID}/${INITRD_IMAGE##*/}"
} > "${EFI_DIR}/loader/entries/${ID}-${KERNEL_VERSION}-${MACHINE_ID}.conf"

if ! [[ -f ${EFI_DIR}/loader/loader.conf ]]; then
        {
                echo "default $ID-*"
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
