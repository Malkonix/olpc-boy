\ OLPC NAND write test
\ The line above MUST stay first: Open Firmware treats a boot file as Forth text
\ only if it starts with "\ " (ofw/core/ofwcore.fth, execute-buffer).
\
\ Boot stick for the NAND write test of an XO-1.  It boots the test kernel exactly like the
\ normal image; Linux then runs non-destructive write tests on a small free window of the
\ internal NAND (8 MiB at 934400 KiB, erase blocks 7300..7363), restores it, and compares the
\ whole NAND with the earlier backup.  Results: logs\boot-NNNN\NANDTEST.TXT on this stick.
\ The rest of the NAND is exposed read-only by the kernel command line.

\ Define a helper that unfreezes the console and makes the display visible
: (visible)  " unfreeze visible" evaluate  ;
\ Run the helper under catch so an older firmware lacking these words cannot abort the script
' (visible) catch  drop  forget (visible)

\ Kernel image: "last:" is the alias for the device this script was loaded from (the boot stick)
" last:\boot\bzImage" to boot-device

\ Kernel command line, same as the normal boot plus:
\   mtdparts=cafe_nand:934400k(pre)ro,8192k(test),-(post)ro : only the middle partition is writeable
\   olpcboy.nandtest                                        : marks the test image
" console=tty0 fbcon=font:TER16x32 loglevel=1 printk.time=1 panic=0 reboot=pci rootwait olpcboy.debug=1 vt.global_cursor_default=0 consoleblank=0 usbcore.autosuspend=-1 i8042.dumbkbd=1 mtdparts=cafe_nand:934400k(pre)ro,8192k(test),-(post)ro olpcboy.nandtest" to boot-file

\ Load the kernel from boot-device with boot-file as its command line and run it
boot
