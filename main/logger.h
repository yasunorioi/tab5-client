// logger.h — microSD CSV work logger.
//
// Mounts the Tab5 microSD (SDMMC slot 0, 4-bit, powered by the P4 on-chip LDO)
// and writes one CSV row per second with the live fix + attitude + cut/fill so a
// pass can be reviewed / kept as a record. Pins + LDO channel from M5Stack's Tab5
// BSP (m5stack/M5Tab5-UserDemo). Target-only (SDMMC + FATFS).
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool     mounted;    // SD present + FAT mounted
    bool     logging;    // a log file is open and being written
    uint32_t rows;       // rows written to the current file
    char     path[40];   // current log file path ("" if none)
} logger_status_t;

// Mount the SD and start the 1 Hz writer task. Logging auto-starts if the card
// mounts. Returns false (does not abort) if no card / mount fails — the box runs
// fine without it. Safe to call once from app_main.
bool logger_init(void);

// Start (open a new file) or stop (close) a log session.
void logger_set(bool on);

void logger_status_get(logger_status_t *out);
