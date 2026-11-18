\ OLPC boot script
\ The line above MUST stay first: Open Firmware treats a boot file as Forth text
\ only if it starts with "\ " (ofw/core/ofwcore.fth, execute-buffer).
\ Unattended boot for an XO-1 (Geode LX, Q2F firmware): no typing at "ok" needed.

\ Define a helper that unfreezes the console and makes the display visible
: (visible)  " unfreeze visible" evaluate  ;
\ Run the helper under catch so an older firmware lacking these words cannot abort the script
' (visible) catch  drop  forget (visible)

\ Kernel image: "last:" is the alias for the device this script was loaded from (the USB stick)
" last:\boot\bzImage" to boot-device

\ Kernel command line, no root= : the embedded initramfs finds partitions by label
\   console=tty0 + fbcon TER16x32 : readable text on the 1200x900 LCD
\   ignore_loglevel printk.time=1 : full timestamped kernel log
\   panic=0                       : on panic stay on screen, do not reboot
\   reboot=pci                    : the reboot method the XO-1 needs
\   rootwait                      : USB stick may enumerate late
\   olpcboy.debug=1               : enable the initramfs/userland debug logging
\ (no ramoops/memmap reservation: safe RAM addresses beside Open Firmware's own
\  top-of-memory area on the 256 MB XO-1 could not be verified)
" console=tty0 fbcon=font:TER16x32 ignore_loglevel printk.time=1 panic=0 reboot=pci rootwait olpcboy.debug=1" to boot-file

\ Load the kernel from boot-device with boot-file as its command line and run it
boot
