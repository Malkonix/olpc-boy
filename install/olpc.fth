\ OLPC boot script (NAND installer)
\ The line above MUST stay first: Open Firmware treats a boot file as Forth text
\ only if it starts with "\ " (ofw/core/ofwcore.fth, execute-buffer).
\
\ Boot stick that installs olpc-boy on the internal NAND of an XO-1 (precheck against the backup, erase, write the JFFS2 image, verify).
\ The kernel command line exposes the NAND as mtd0 "olpcboy" (erase blocks 0..8187, writeable) and
\ mtd1 "bbt" (the 4 bad block table blocks, read-only); the tool refuses to write unless sysfs
\ confirms exactly that.  Results: logs\boot-NNNN\INSTALL.TXT on this stick.

\ Define a helper that unfreezes the console and makes the display visible
: (visible)  " unfreeze visible" evaluate  ;
\ Run the helper under catch so an older firmware lacking these words cannot abort the script
' (visible) catch  drop  forget (visible)

\ Kernel image: "last:" is the alias for the device this script was loaded from (the boot stick)
" last:\boot\bzImage" to boot-device

" console=tty0 fbcon=font:TER16x32 loglevel=1 printk.time=1 panic=0 reboot=pci rootwait olpcboy.debug=1 vt.global_cursor_default=0 consoleblank=0 usbcore.autosuspend=-1 i8042.dumbkbd=1 mtdparts=cafe_nand:1048064k(olpcboy),-(bbt)ro olpcboy.install" to boot-file

\ Load the kernel from boot-device with boot-file as its command line and run it
boot
