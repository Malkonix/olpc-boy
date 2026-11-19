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
\   console=tty0                  : kernel/init console on the LCD framebuffer
\   fbcon=font:TER16x32           : large readable font on the 1200x900 LCD
\   loglevel=4                    : only warnings and worse are painted on the
\                                   framebuffer console (less repainting); the log
\                                   copier reads /dev/kmsg, which still gets everything
\   printk.time=1                 : timestamp every kernel message
\   panic=0                       : on panic stay on screen, do not reboot
\   reboot=pci                    : the reboot method the XO-1 needs
\   rootwait                      : USB stick may enumerate late
\   olpcboy.debug=1               : enable the initramfs/userland debug logging
\   vt.global_cursor_default=0    : no blinking text cursor (a constant repaint)
\   consoleblank=0                : the screen must never blank or power down
\ (no ramoops/memmap reservation: safe RAM addresses beside Open Firmware's own
\  top-of-memory area on the 256 MB XO-1 could not be verified)
\   usbcore.autosuspend=-1        : never autosuspend a USB device (the default here is
\                                   2 s): removes runtime suspend/resume of the Sony
\                                   game pad and of the boot stick as a variable; costs
\                                   a little power, nothing else
\   i8042.dumbkbd=1               : the keyboard module of this unit is unplugged,
\                                   so atkbd's probe gets no answer and no input
\                                   device appears; this makes atkbd bind without
\                                   probing, so D-pad / game-key scancodes sent by
\                                   the EC are delivered
" console=tty0 fbcon=font:TER16x32 loglevel=4 printk.time=1 panic=0 reboot=pci rootwait olpcboy.debug=1 vt.global_cursor_default=0 consoleblank=0 usbcore.autosuspend=-1 i8042.dumbkbd=1" to boot-file

\ Load the kernel from boot-device with boot-file as its command line and run it
boot
