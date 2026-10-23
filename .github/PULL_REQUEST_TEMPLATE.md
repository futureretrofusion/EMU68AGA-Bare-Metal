## Summary

Describe exactly what this change fixes or adds.

## Why this change is needed

Link the related issue if one exists:

Fixes #

## Base

- EMU68AGA branch/tag/commit:
- Hardware target:

## What changed

List the files/subsystems changed and why.

## Testing performed

### Software tests

- [ ] Build passes
- [ ] Existing regression tests pass
- [ ] New regression test added where appropriate

### Real hardware tests

- [ ] Tested on physical Raspberry Pi hardware
- [ ] Amiga environment boots
- [ ] Keyboard works
- [ ] Mouse works
- [ ] USB joypad/controller works
- [ ] No obvious regression in previously working behaviour

Describe the exact hardware used and the observed result.

## USB / input changes

If this touches USB, HID, keyboard, mouse or joypad handling, include:

- USB VID:PID where relevant
- report descriptor/report length details
- button/axis mapping
- whether keyboard isolation was checked
- whether more than one controller was tested

## Compatibility impact

List any games, demos or applications tested.

## Evidence

Attach logs, screenshots, serial output, hashes, test output or other evidence.

## Notes for maintainers

Do not treat software-only success as proof of hardware correctness. EMU68AGA changes affecting timing, USB, video, storage, reset behaviour or Amiga chipset emulation should be physically verified before release.
