#!/bin/bash -e

# create GPT table with EFI System Partition
rm -f test-disk
dd if=/dev/null of=test-disk bs=1M seek=64 count=1
parted --script test-disk "mklabel gpt" "mkpart ESP fat32 1M 64M" "set 1 boot on" "print"

# create FAT32 file system
LOOP=$(losetup --show -f -P test-disk)
mkfs.vfat -F32 $LOOP
mkdir -p mnt
mount $LOOP mnt

# install gummiboot
mkdir -p mnt/EFI/BOOT
cp gummibootx64.efi mnt/EFI/BOOT/BOOTX64.EFI

[ -e /boot/shellx64.efi ] && cp /boot/shellx64.efi mnt/

# install entries
mkdir -p mnt/loader/entries
echo -e "timeout 3\n" > mnt/loader/loader.conf
echo -e "title Test\nefi /test\n" > mnt/loader/entries/test.conf
echo -e "title Test2\nefi /test2\n" > mnt/loader/entries/test2.conf
echo -e "title Test3\nefi /test3\n" > mnt/loader/entries/test3.conf
echo -e "title Test4\nefi /test4\n" > mnt/loader/entries/test4.conf
echo -e "title Test5\nefi /test5\n" > mnt/loader/entries/test5.conf

sync
umount mnt
rmdir mnt
losetup -d $LOOP
