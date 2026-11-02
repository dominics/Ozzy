#ifndef OzzyKextClient_h
#define OzzyKextClient_h

// Helpers for the user-space clients of OzzyKext (OzzyHAL, OzzyMIDI)

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <string.h>

/* A name field of the shared memory (@src, terminated or not: the driver's side writes it) as
 * a C string in @dst, cut to fit; @fallback where it is empty. Returns the string to use. */
template <size_t N, size_t M>
static inline const char* OzzyCopyName(char (&dst)[M], const char (&src)[N], const char* fallback) {
    static_assert(M > 0, "no room for the terminator");
    size_t n = strnlen(src, N < M - 1 ? N : M - 1);
    memcpy(dst, src, n);
    dst[n] = 0;
    return n ? dst : fallback;
}

/* The USB port of the device an OzzyKext instance drives (its provider's locationID).
 * Ploytec devices have no serial number, so this is what tells two of the same model apart. */
static inline uint32_t OzzyUSBLocationID(io_service_t service) {
    uint32_t locationID = 0;
    CFTypeRef value = IORegistryEntrySearchCFProperty(service, kIOServicePlane, CFSTR("locationID"),
        kCFAllocatorDefault, kIORegistryIterateRecursively | kIORegistryIterateParents);
    if (value) {
        if (CFGetTypeID(value) == CFNumberGetTypeID())
            CFNumberGetValue((CFNumberRef)value, kCFNumberSInt32Type, &locationID);
        CFRelease(value);
    }
    return locationID;
}

#endif
