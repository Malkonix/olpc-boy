\ OLPC boot script (internal NAND)
\ The line above MUST stay first: Open Firmware treats a boot file as Forth text
\ only if it starts with "\ " (ofw/core/ofwcore.fth, execute-buffer).
\ Stored on the NAND as /boot/olpc.fth of the whole-device JFFS2 (Open Firmware reads
\ nand:\boot\olpc.fth when no USB stick / SD card carries a boot script first).
\ Same unattended boot as the USB image; the kernel finds its root on the NAND.

\ Define a helper that unfreezes the console and makes the display visible
: (visible)  " unfreeze visible" evaluate  ;
\ Run the helper under catch so an older firmware lacking these words cannot abort the script
' (visible) catch  drop  forget (visible)

\ Kernel and initrd, both read from the NAND's JFFS2.  The initrd only adds cafe_nand.ko
\ (the NAND driver is a module and must be loaded before the root can be mounted).
" nand:\boot\bzImage" to boot-device
" nand:\boot\initrd.img" to ramdisk

\ Kernel command line (as the USB image's, minus rootwait):
\   mtdparts=cafe_nand:...        : mtd0 "olpcboy" = erase blocks 0..8187 (the JFFS2 root, writeable),
\                                   mtd1 "bbt" = blocks 8188..8191 (the bad block tables, read-only)
\   olpcboy.nand                  : initramfs loads cafe_nand (allow_bbt_write=0) and mounts mtd0 as root;
\                                   boot logs go to /data/olpclog/logs/boot-NNNN on the NAND
" console=tty0 fbcon=font:TER16x32 loglevel=1 printk.time=1 panic=0 reboot=pci olpcboy.debug=1 vt.global_cursor_default=0 consoleblank=0 usbcore.autosuspend=-1 i8042.dumbkbd=1 mtdparts=cafe_nand:1048064k(olpcboy),-(bbt)ro olpcboy.nand" to boot-file

\ Load the kernel from boot-device with boot-file as its command line and run it
boot
