#!/usr/bin/env python3
"""
Switches Portal Zero (Skylanders portal emulator) into flash mode from the PC.
Afterwards the drive RPI-RP2 appears on the PC - just drag the new PortalZero.uf2 onto it.

Install once:
    pip3 install hidapi

Usage (board connected to the Mac/PC via USB, Portal Zero firmware running):
    python3 portalzero_flash.py
"""
import sys

try:
    import hid
except ImportError:
    print("Please install first:  pip3 install hidapi")
    sys.exit(1)

VID, PID = 0x1430, 0x0150                 # how Portal Zero identifies itself (like a real portal)
MAGIC = b"PORTALZERO-FLASH"

devices = hid.enumerate(VID, PID)
if not devices:
    print("Portal Zero not found. Is the board connected via USB and is the Portal Zero firmware running?")
    sys.exit(1)

dev = hid.device()
dev.open_path(devices[0]["path"])
report = MAGIC + bytes(32 - len(MAGIC))   # 32 bytes, like a portal command
written = dev.write(b"\x00" + report)     # 0x00 = no report ID
dev.close()

if written < 0:
    print("Sending failed.")
    sys.exit(1)
print("OK - the board is switching to flash mode. The drive RPI-RP2 will appear shortly.")
