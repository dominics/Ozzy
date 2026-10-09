#pragma once

// Centralized logging macros for consistent prefix usage across the entire Ozzy codebase

#if defined(KERNEL) || defined(__KERNEL__)
// ============================================================================
// KERNEL (KEXT) LOGGING
// ============================================================================
#include <IOKit/IOLib.h>

// OzzyKext logging (device-agnostic kext code)
#define LogOzzyKext(fmt, ...) IOLog("[OzzyKext] " fmt "\n", ##__VA_ARGS__)

// Ploytec logging (device-specific code in kext)
#define LogPloytec(fmt, ...) IOLog("[Ploytec] " fmt "\n", ##__VA_ARGS__)

#include <os/log.h>
// Dynacord engine logging (shared format strings with userspace, so os_log)
#define LogDynacord(fmt, ...) os_log(OS_LOG_DEFAULT, "[Dynacord] " fmt, ##__VA_ARGS__)
#define LogDynacordError(fmt, ...) os_log_error(OS_LOG_DEFAULT, "[Dynacord] " fmt, ##__VA_ARGS__)

#else
// ============================================================================
// USERSPACE LOGGING (HAL, MIDI, Daemon)
// ============================================================================
#include <os/log.h>

// Helper to get subsystem logs
static inline os_log_t GetOzzyHALLog() {
    static os_log_t log = NULL;
    if (!log) log = os_log_create("OzzyHAL", "plugin");
    return log;
}

static inline os_log_t GetOzzyMIDILog() {
    static os_log_t log = NULL;
    if (!log) log = os_log_create("OzzyMIDI", "driver");
    return log;
}

static inline os_log_t GetOzzyDaemonLog() {
    static os_log_t log = NULL;
    if (!log) log = os_log_create("OzzyDaemon", "daemon");
    return log;
}

static inline os_log_t GetDynacordLog() {
    static os_log_t log = NULL;
    if (!log) log = os_log_create("Ozzy", "dynacord");
    return log;
}

static inline os_log_t GetPloytecLog() {
    static os_log_t log = NULL;
    if (!log) log = os_log_create("Ozzy", "ploytec");
    return log;
}

// Logging macros for HAL
#define LogOzzyHAL(fmt, ...) os_log(GetOzzyHALLog(), "[OzzyHAL] " fmt, ##__VA_ARGS__)
#define LogOzzyHALError(fmt, ...) os_log_error(GetOzzyHALLog(), "[OzzyHAL] " fmt, ##__VA_ARGS__)

// Logging macros for MIDI
#define LogOzzyMIDI(fmt, ...) os_log_info(GetOzzyMIDILog(), "[OzzyMIDI] " fmt, ##__VA_ARGS__)
#define LogOzzyMIDIError(fmt, ...) os_log_error(GetOzzyMIDILog(), "[OzzyMIDI] " fmt, ##__VA_ARGS__)

// Ploytec (Xone) engine logging when built outside the kernel
#define LogPloytec(fmt, ...) os_log(GetPloytecLog(), "[Ploytec] " fmt, ##__VA_ARGS__)

// Logging macros for the userspace daemon and its Dynacord engine
#define LogOzzyDaemon(fmt, ...) os_log(GetOzzyDaemonLog(), "[OzzyDaemon] " fmt, ##__VA_ARGS__)
#define LogOzzyDaemonError(fmt, ...) os_log_error(GetOzzyDaemonLog(), "[OzzyDaemon] " fmt, ##__VA_ARGS__)
#define LogDynacord(fmt, ...) os_log(GetDynacordLog(), "[Dynacord] " fmt, ##__VA_ARGS__)
#define LogDynacordError(fmt, ...) os_log_error(GetDynacordLog(), "[Dynacord] " fmt, ##__VA_ARGS__)

#endif
