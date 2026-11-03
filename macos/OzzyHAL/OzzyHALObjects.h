/* SPDX-License-Identifier: MIT */
/*
 * The fork's objects in an OzzyHAL device's slot, after upstream's device (+0), streams (+1, +2)
 * and clock source control (+3) (OzzyHAL.h, OzzyHAL.mm's OzzyObjectKind): the output and input
 * volume and mute controls, then a device and a stream per mono input device (OzzyChannels.h), and the related
 * devices those make up. Object kinds are offsets in the slot. C, usable from C++ (OzzyHAL) and
 * the host tests (tests/test_hal_objects.c).
 */

#ifndef OZZY_HAL_OBJECTS_H
#define OZZY_HAL_OBJECTS_H

#include <stdbool.h>
#include <stdint.h>
#include "OzzyChannels.h"

enum ozzy_extra_object_kind {
	kOzzyObjectVolumeControl = 4,        /* output */
	kOzzyObjectMuteControl = 5,          /* output */
	kOzzyObjectInputVolumeControl = 6,
	kOzzyObjectInputMuteControl = 7,
	kOzzyObjectMonoDevice = 8,     /* mono input i: its device at 8 + 2i, its stream at 9 + 2i */
	kOzzyObjectKinds = kOzzyObjectMonoDevice + 2 * OZZY_MAX_MONO_INPUTS,   /* the objects a device has */
};

/* The mono input index of an object kind (its device's or its stream's), or -1 */
static inline int OzzyMonoIndex(uint32_t kind)
{
	return kind >= kOzzyObjectMonoDevice && kind < kOzzyObjectKinds ? (int)(kind - kOzzyObjectMonoDevice) / 2 : -1;
}

static inline bool OzzyIsMonoObject(uint32_t kind)
{
	return OzzyMonoIndex(kind) >= 0;
}

/* A mono input's device, not its stream */
static inline bool OzzyIsMonoDevice(uint32_t kind)
{
	return OzzyMonoIndex(kind) >= 0 && (kind - kOzzyObjectMonoDevice) % 2 == 0;
}

/* The object kind of mono input @i's device (its stream's is one more) */
static inline uint32_t OzzyMonoDeviceKind(unsigned int i)
{
	return kOzzyObjectMonoDevice + 2 * i;
}

static inline bool OzzyIsExtraControl(uint32_t kind)
{
	return kind >= kOzzyObjectVolumeControl && kind <= kOzzyObjectInputMuteControl;
}

/* A volume control, not a mute */
static inline bool OzzyIsVolumeControl(uint32_t kind)
{
	return kind == kOzzyObjectVolumeControl || kind == kOzzyObjectInputVolumeControl;
}

/* An input control, not an output one */
static inline bool OzzyIsInputControl(uint32_t kind)
{
	return kind == kOzzyObjectInputVolumeControl || kind == kOzzyObjectInputMuteControl;
}

/*
 * The related devices (kAudioDevicePropertyRelatedDevices) of the main device @device and of
 * each of its @mono_count mono devices: all of them, the main device first, into @out; returns
 * how many
 */
static inline unsigned int ozzy_related_devices(uint32_t device, unsigned int mono_count,
						uint32_t out[1 + OZZY_MAX_MONO_INPUTS])
{
	unsigned int i, n = 0;

	out[n++] = device;
	for (i = 0; i < mono_count && i < OZZY_MAX_MONO_INPUTS; i++)
		out[n++] = device + OzzyMonoDeviceKind(i);
	return n;
}

#endif
