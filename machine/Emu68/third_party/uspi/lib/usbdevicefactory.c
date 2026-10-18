//
// usbdevicefactory.c
//
// USPi - An USB driver for Raspberry Pi written in C
// Copyright (C) 2014-2018  R. Stange <rsta2@o2online.de>
// 
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#include <uspi/usbdevicefactory.h>
#include <uspios.h>
#include <uspi/assert.h>

// for factory
#include <uspi/usbstandardhub.h>
#include <uspi/usbmassdevice.h>
#include <uspi/usbkeyboard.h>
#include <uspi/usbmouse.h>
#include <uspi/usbgamepad.h>
#include <uspi/usbdevice.h>
#include <uspi/usbmidi.h>
#include <uspi/smsc951x.h>
#include <uspi/lan7800.h>

TUSBFunction *USBDeviceFactoryGetDevice (TUSBFunction *pParent, TString *pName)
{
	assert (pParent != 0);
	assert (pName != 0);
	
	TUSBFunction *pResult = 0;

	if (   StringCompare (pName, "int9-0-2") == 0
	    || StringCompare (pName, "int9-0-0") == 0)
	{
		TUSBStandardHub *pDevice = (TUSBStandardHub *) malloc (sizeof (TUSBStandardHub));
		assert (pDevice != 0);
		USBStandardHub (pDevice, pParent);
		pResult = (TUSBFunction *) pDevice;
	}
	/* FRF v0.10.44: MSC 08/06/50 with endian-correct BOT frames. */
	else if (StringCompare (pName, "int8-6-50") == 0)
	{
		extern volatile unsigned frf_um_msc_interface_seen;
		frf_um_msc_interface_seen++;
		TUSBBulkOnlyMassStorageDevice *pDevice = (TUSBBulkOnlyMassStorageDevice *) malloc (sizeof (TUSBBulkOnlyMassStorageDevice));
		assert (pDevice != 0);
		USBBulkOnlyMassStorageDevice (pDevice, pParent);
		pResult = (TUSBFunction *) pDevice;
	}
	else if (StringCompare (pName, "int3-1-1") == 0)
	{
		TUSBKeyboardDevice *pDevice = (TUSBKeyboardDevice *) malloc (sizeof (TUSBKeyboardDevice));
		assert (pDevice != 0);
		USBKeyboardDevice (pDevice, pParent);
		pResult = (TUSBFunction *) pDevice;
	}
	else if (StringCompare (pName, "int3-1-2") == 0)
	{
		TUSBMouseDevice *pDevice = (TUSBMouseDevice *) malloc (sizeof (TUSBMouseDevice));
		assert (pDevice != 0);
		USBMouseDevice (pDevice, pParent);
		pResult = (TUSBFunction *) pDevice;
	}
	#if 0 /* FRF input-only bring-up */
else if (StringCompare (pName, "ven424-ec00") == 0)
	{
		TSMSC951xDevice *pDevice = (TSMSC951xDevice *) malloc (sizeof (TSMSC951xDevice));
		assert (pDevice != 0);
		SMSC951xDevice (pDevice, pParent);
		pResult = (TUSBFunction *) pDevice;
	}
#endif
	#if 0 /* FRF input-only bring-up */
else if (StringCompare (pName, "ven424-7800") == 0)
	{
		TLAN7800Device *pDevice = (TLAN7800Device *) malloc (sizeof (TLAN7800Device));
		assert (pDevice != 0);
		LAN7800Device (pDevice, pParent);
		pResult = (TUSBFunction *) pDevice;
	}
#endif
    /* Original Microsoft Xbox 360 wired controller (045e:028e).
     * Vendor interface FF/5D/01, NOT USB HID. Bind only exact VID/PID
     * so unsupported vendor-specific devices are never captured. */
    else if (StringCompare (pName, "ven45e-28e") == 0)
    {
        TUSBGamePadDevice *pDevice = (TUSBGamePadDevice *) malloc (sizeof (TUSBGamePadDevice));
        assert (pDevice != 0);
        USBGamePadDevice (pDevice, pParent);
        pResult = (TUSBFunction *) pDevice;
    }
    /* Known XInput and Game Input Protocol devices. Match a controller
       class/interface, not arbitrary vendor products with unrelated USB roles.
       146b:0603 exposes XInput when attached to PC on supported firmware.
       2dc8 arcade sticks may report as 360 or One depending on mode. */
    else if ((StringCompare(pName,"ven146b-603")==0 &&
              USBFunctionGetInterfaceClass(pParent)==0xff &&
              USBFunctionGetInterfaceSubClass(pParent)==0x5d &&
              USBFunctionGetInterfaceProtocol(pParent)==0x01) ||
             (StringCompare(pName,"ven45e-2ea")==0 &&
              USBFunctionGetInterfaceClass(pParent)==0xff &&
              USBFunctionGetInterfaceSubClass(pParent)==0x47 &&
              USBFunctionGetInterfaceProtocol(pParent)==0xd0 &&
              USBFunctionGetInterfaceNumber(pParent)==0) ||
             (StringCompare(pName,"intff-5d-1")==0 &&
              (USBFunctionGetDevice(pParent)->m_pDeviceDesc->idVendor==0x146b ||
               USBFunctionGetDevice(pParent)->m_pDeviceDesc->idVendor==0x045e ||
               USBFunctionGetDevice(pParent)->m_pDeviceDesc->idVendor==0x2dc8)) ||
             (StringCompare(pName,"intff-47-d0")==0 &&
              USBFunctionGetInterfaceNumber(pParent)==0 &&
              (USBFunctionGetDevice(pParent)->m_pDeviceDesc->idVendor==0x045e ||
               USBFunctionGetDevice(pParent)->m_pDeviceDesc->idVendor==0x2dc8)))
    {
        TUSBGamePadDevice *pDevice=(TUSBGamePadDevice*)malloc(sizeof(TUSBGamePadDevice));
        assert(pDevice!=0);
        USBGamePadDevice(pDevice,pParent);
        pResult=(TUSBFunction*)pDevice;
    }
    else if (StringCompare (pName, "int3-0-0") == 0)
    {
        TUSBGamePadDevice *pDevice = (TUSBGamePadDevice *) malloc (sizeof (TUSBGamePadDevice));
        assert (pDevice != 0);
        USBGamePadDevice (pDevice, pParent);
        pResult = (TUSBFunction *) pDevice;
    }
	#if 0 /* FRF input-only bring-up */
else if (StringCompare (pName, "int1-3-0") == 0)
	{
		TUSBMIDIDevice *pDevice = (TUSBMIDIDevice *) malloc (sizeof (TUSBMIDIDevice));
		assert (pDevice != 0);
		USBMIDIDevice (pDevice, pParent);
		pResult = (TUSBFunction *)pDevice;
	}
#endif
	// new devices follow

	if (pResult != 0)
	{
		LogWrite ("usbdev", LOG_NOTICE, "Using device/interface %s", StringGet (pName));
	}
	
	_String (pName);
	free (pName);
	
	return pResult;
}
