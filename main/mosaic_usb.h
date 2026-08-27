// mosaic_usb.h — USB identity of the Septentrio mosaic-go receiver.
//
// Shared by usb_cdc_source.c (RTCM3 source) and nmea_source.c so the VID/PID
// live in one place instead of being defined in two.
//
// ⚠ These are per-individual: a different mosaic-go unit/firmware enumerates
// under a different PID and a different set of CDC interfaces. Values below are
// THIS client's receiver — the mosaic-go G5 P3H in docs/hardware-findings.md,
// which comes up as PID 0x8231 with two CDC COMs (itf {0,2}) and no mass-storage
// interface. (The tab5-caster bench unit was 0x85C0 with three COMs + MSC.)
#pragma once

#define MOSAIC_VID  0x152A
#define MOSAIC_PID  0x8231
