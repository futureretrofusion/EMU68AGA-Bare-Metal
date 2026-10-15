import unittest, sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from prepare_dos_mount_stability import rewrite_startup,rewrite_user

class PatcherTests(unittest.TestCase):
    def test_removes_legacy_scsi_and_mount_sweep(self):
        sample='''; Loads a patched scsi.device with large partition support if WHDLoad: drive
If WARN
  BMX scsi_hw_select
  Reboot
Endif
; Loads PeterK's icon library
  KeepThis
 ; Installs the pistorm shared drive dos driver
If $PiStorm EQ "1"
  Copy PC0 DEVS:
EndIf
C:Mount >NIL: DEVS:DOSDrivers/~(#?.info)
IF EXISTS DEVS:Monitors
  Valid
EndIf
c:mount workbench:system/ami-share
LoadWB
EndCLI
'''
        actual=rewrite_startup(sample)
        self.assertNotIn('scsi_hw_select',actual)
        self.assertNotIn('Mount >NIL: DEVS:DOSDrivers/~',actual)
        self.assertIn('C:Mount >NIL: DEVS:DOSDrivers/PIPE',actual)
        self.assertIn('C:Mount >NIL: DEVS:DOSDrivers/AUDIO',actual)
        self.assertIn('LoadWB\nEndCLI',actual)
        self.assertIn(';c:mount workbench:system/ami-share',actual)

    def test_ags_guard(self):
        a=';BEGIN Amiga Game Selector\n Ex S:AGS-Stuff\n;END Amiga Game Selector'
        b=rewrite_user(a)
        self.assertIn('If EXISTS WHDLoad:AGS2',b)
        self.assertIn('Ex S:AGS-Stuff',b)
        self.assertIn('EndIf',b)

    def test_fail_closed_when_unexpected_layout(self):
        with self.assertRaises(ValueError):
            rewrite_startup('Unknown startup layout')
        with self.assertRaises(ValueError):
            rewrite_user('Unknown startup layout')

if __name__=='__main__': unittest.main()
