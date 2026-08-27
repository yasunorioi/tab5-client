// mosaic_usb.h — USB identity of the Septentrio mosaic-go receiver.
//
// Shared by usb_cdc_source.c (RTCM3 source) and nmea_source.c so the VID/PID
// live in one place instead of being defined in two.
//
// ⚠ These are the values observed on ONE development unit. A different mosaic-go
// individual or firmware can enumerate under a different PID and a different set
// of CDC interfaces — e.g. a P3H unit that comes up as PID 0x8231 with only two
// CDC COMs (itf {0,2}) and no mass-storage interface. See docs/hardware-findings.md
// before assuming these are universal.
#pragma once

#define MOSAIC_VID  0x152A
#define MOSAIC_PID  0x85C0
