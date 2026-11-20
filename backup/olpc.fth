\ OLPC NAND backup
\ The line above MUST stay first: Open Firmware treats a boot file as Forth text
\ only if it starts with "\ " (ofw/core/ofwcore.fth, execute-buffer).
\
\ Boot stick for the NAND backup of an XO-1 (Geode LX, Q2F firmware).  It boots the backup
\ kernel exactly like the normal image; Linux then asks for a second USB stick (FAT32, label
\ OLPCNAND) and copies the internal NAND onto it, read-only (the NAND is never written).
\ Result, in OLPCNAND:\nandbackup-NNN\ :
\   nand-raw-oob.bin    every page, data+OOB, no ECC (read twice and compared)
\   nand-data-ecc.bin   ECC-corrected data of all 8192 erase blocks, positions preserved
\   NANDCOPY.DAT        Open Firmware restore input (what save-nand would have produced)
\   MANIFEST.txt, SHA256SUMS
\
\ RESTORE without Linux (only if the NAND is ever damaged; this stick never writes the NAND):
\   1. Copy NANDCOPY.DAT to the ROOT of a FAT USB stick that Open Firmware reads reliably
\      (OFW sees 8.3 names only, so not from inside the nandbackup-NNN folder).
\   2. At the "ok" prompt, stick inserted:      nd-copy-nand u:\NANDCOPY.DAT
\      It erases every good NAND block, writes the file's blocks one after the other into
\      the next good blocks and puts JFFS2 cleanmarkers on the rest.  No .crc file is used.
\   Read MANIFEST.txt first: it warns if the restore file would not fit the good blocks.

\ Define a helper that unfreezes the console and makes the display visible
: (visible)  " unfreeze visible" evaluate  ;
\ Run the helper under catch so an older firmware lacking these words cannot abort the script
' (visible) catch  drop  forget (visible)

\ Kernel image: "last:" is the alias for the device this script was loaded from (the boot stick)
" last:\boot\bzImage" to boot-device

\ Kernel command line, same as the normal boot plus:
\   mtdparts=cafe_nand:-(nand)ro  : the NAND shows up as ONE read-only MTD partition
\   olpcboy.nandbackup            : marks the backup image
" console=tty0 fbcon=font:TER16x32 loglevel=1 printk.time=1 panic=0 reboot=pci rootwait olpcboy.debug=1 vt.global_cursor_default=0 consoleblank=0 usbcore.autosuspend=-1 i8042.dumbkbd=1 mtdparts=cafe_nand:-(nand)ro olpcboy.nandbackup" to boot-file

\ Load the kernel from boot-device with boot-file as its command line and run it
boot
