# RC14 native mouse-wheel test on CaffeineOS

Save a known-working kernel8.img before replacing it with RC14's
BOOT/kernel8.img. No changes to the Amiga RDB or filesystem.

1. Boot Workbench/CaffeineOS on the real Pi 3 using the USB wheel mouse
   plugged into an ARM-controlled port (directly or a working hub).
2. Confirm mouse movement and buttons still work as before.
3. Open the native OSD with LeftCtrl+LeftAlt+F12.
4. Select MONITOR -> ON, close OSD. Watch its top-right HUD W counter.
5. Rotate wheel upward and downward. The **W** count should increase
   for either direction, regardless of CaffeineOS wheel configuration.
6. Test Directory Opus or an Amiga window with scrollable contents.
   NewMouse events appear as raw codes 0x7A (up) and 0x7B (down).
7. If W advances but the window doesn't scroll, inspect the installed
   wheel commodity (FreeWheel/NewMouse) and its configured handlers.
   Some software requires FreeWheel even when it receives valid
   NewMouse input events.
8. If W stays at 0, inspect the USB mouse report. The 3-byte HID
   boot protocol has NO wheel byte; a compatible report-protocol
   mouse provides a wheel byte in a longer report. The RC14 report
   ID parser covers common report-ID mice, but not every bitfield
   layout. Share the VID:PID, interface and raw report length.

A long scroll value is clamped to 8 events per USB report to
protect the Amiga keyboard queue from overflow. Very fast wheel
spinning should not impair normal Ctrl/Alt/F12 shortcuts.

Architecture: ARM-side USB -> virtual Amiga CIA keyboard queue ->
input.device -> CaffeineOS wheel software, NO Poseidon involved.

This is a physical hardware TEST BUILD, not a proven working wheel
implementation yet. The OSD MONITOR counter is the first checkpoint.
