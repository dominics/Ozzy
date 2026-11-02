/* SPDX-License-Identifier: MIT */
/*
 * OzzyHAL's object dispatch for the fork's objects (macos/OzzyHAL/OzzyHALObjects.h): which
 * object kinds are the volume and mute controls, the mono devices and their streams and which
 * mono input each belongs to, nothing for upstream's kinds or past the last mono input, and the
 * related devices of a CMS 600-3 (main, MON, AUX) and of a device without mono inputs.
 */

#include "macos/OzzyHAL/OzzyHALObjects.h"
#include "check.h"

int main(void)
{
	const struct ozzy_device *cms = ozzy_device_lookup(0x0562, 0x03EB);
	const struct ozzy_device *db4 = ozzy_device_lookup(0x0A4A, 0xFFDB);
	const struct ozzy_mono_input *mono[OZZY_MAX_MONO_INPUTS];
	uint32_t related[1 + OZZY_MAX_MONO_INPUTS], kind;
	unsigned int n;

	/* Upstream's device, streams and clock control are none of the fork's */
	for (kind = 0; kind < kOzzyObjectVolumeControl; kind++)
		CHECK(OzzyMonoIndex(kind) == -1 && !OzzyIsMonoObject(kind) && !OzzyIsExtraControl(kind), "kind %u", kind);
	CHECK(OzzyIsExtraControl(kOzzyObjectVolumeControl) && OzzyIsExtraControl(kOzzyObjectMuteControl), "controls");
	CHECK(!OzzyIsMonoObject(kOzzyObjectVolumeControl) && !OzzyIsMonoObject(kOzzyObjectMuteControl), "controls not mono");

	/* Mono input i: its device at 6 + 2i, its stream next to it */
	for (n = 0; n < OZZY_MAX_MONO_INPUTS; n++) {
		uint32_t dev = OzzyMonoDeviceKind(n);

		CHECK(OzzyMonoIndex(dev) == (int)n && OzzyMonoIndex(dev + 1) == (int)n, "mono %u index", n);
		CHECK(OzzyIsMonoDevice(dev) && !OzzyIsMonoDevice(dev + 1), "mono %u device vs stream", n);
		CHECK(OzzyIsMonoObject(dev + 1) && !OzzyIsExtraControl(dev), "mono %u objects", n);
	}
	CHECK(OzzyMonoIndex(kOzzyObjectKinds) == -1 && !OzzyIsMonoObject(kOzzyObjectKinds) && OzzyMonoIndex(0xFFFFFFFF) == -1,
	      "past the last mono input");
	CHECK(kOzzyObjectKinds == OzzyMonoDeviceKind(OZZY_MAX_MONO_INPUTS), "kinds end after the last mono stream");

	/* The CMS 600-3: main, MON (input 4) and AUX (input 3), related to each other */
	n = ozzy_mono_inputs_for(cms, mono);
	CHECK(n == 2, "CMS 600-3 mono inputs: %u", n);
	CHECK(ozzy_related_devices(100, n, related) == 3 && related[0] == 100 && related[1] == 106 && related[2] == 108,
	      "CMS 600-3 related devices");
	/* A device without mono inputs is related only to itself */
	n = ozzy_mono_inputs_for(db4, mono);
	CHECK(n == 0 && ozzy_related_devices(116, n, related) == 1 && related[0] == 116, "Xone:DB4 related devices");
	return CHECK_DONE("hal objects");
}
