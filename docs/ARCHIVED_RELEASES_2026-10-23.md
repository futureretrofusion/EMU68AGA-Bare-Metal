# Archived EMU68AGA test releases — 23 October 2026

## Current release

**[v0.10.40a — hardware-verified USB endian fix](https://github.com/futureretrofusion/EMU68AGA-Bare-Metal/releases/tag/v0.10.40a)** is the only published GitHub release following the cleanup on 23 October 2026.

## Historical experimental releases

The seven published test releases below were **unpublished as drafts** on 23 October 2026, because they predate the hardware-verified R40a fix. Their existing release records, binary assets and Git tags were **not deleted**. Draft release records and binary attachments are visible only to authorized repository maintainers. Git tags remain accessible for code-history reconstruction.

| Original release | Git tag | Status |
| --- | --- | --- |
| R18 USB packet capture | `pi3-tinyusb-r18-experimental` | Draft (archived) |
| R19 HID gamepad to Amiga | `pi3-tinyusb-r19-hid-amiga-experimental` | Draft (archived) |
| R20 experimental USB gamepad | `pi3-tinyusb-r20-observed-24b-experimental` | Draft (archived) |
| R21 gamepad HID identification | `pi3-tinyusb-r21-correct-7e05-0920-experimental` | Draft (archived) |
| R22 live input diagnostic | `pi3-tinyusb-r22-live-guest-input-diagnostic` | Draft (archived) |
| R23 keyboard/gamepad isolation | `pi3-tinyusb-r23-gamepad-keyboard-isolation` | Draft (archived) |
| R23 hardware test | `r23-hardware-test-20261022` | Draft (archived) |

The earlier `v0.10.25` release was already an unpublished draft and was left untouched. The early `v0.1-alpha` Git tag and legacy source documentation are retained.

## Restoration and history

Repository maintainers can inspect archived release drafts through the GitHub releases interface or `gh release list --limit 30`. They can restore an individual release by editing its draft status, but should avoid republishing obsolete test kernels unless required for historical reproduction.

The v0.10.40a release corrects the USB device descriptor endian interpretation responsible for erroneous controller identification in R18–R23. Experimental R-series binaries should not be recommended as working downloads.

This housekeeping change archives release *visibility*, not source history: no Git branches, commits, tags or historical binary assets were removed.
