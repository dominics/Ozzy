/* SPDX-License-Identifier: MIT */
#pragma once
#include <cstdint>

/* AudioObjectIDs the plug-in publishes. */
enum OzzyObjectID : uint32_t {
	kOzzyDeviceID = 100,
	kOzzyMonDeviceID = 110,
	kOzzyAuxDeviceID = 120,
	kOzzyInputStreamID = 150,
	kOzzyMonStreamID = 160,
	kOzzyAuxStreamID = 170,
	kOzzyOutputStreamID = 200,
	kOzzyVolumeControlID = 300,
	kOzzyMuteControlID = 301,
};

/* One CoreAudio device over the shared engine. The main device carries every
 * channel both ways; the CMS 600-3 adds a 1-channel input device for each of
 * its AUX and MON buses, so apps that only record channels 1-2 can use them. */
struct OzzyDeviceInfo {
	OzzyObjectID device;
	OzzyObjectID inputStream;
	OzzyObjectID outputStream;	/* kOzzyDeviceID's only; 0 on input-only devices */
	const char* name;		/* nullptr: the product name from the engine */
	const char* uidSuffix;		/* appended to the main device's UID and model UID */
	uint32_t inputChannel;		/* 1-based source channel; 0 for all of them */
};

static constexpr OzzyDeviceInfo kOzzyDevices[] = {
	{ kOzzyDeviceID, kOzzyInputStreamID, kOzzyOutputStreamID, nullptr, "", 0 },
	{ kOzzyMonDeviceID, kOzzyMonStreamID, OzzyObjectID(0), "CMS 600-3 MON", ":MON", 4 },
	{ kOzzyAuxDeviceID, kOzzyAuxStreamID, OzzyObjectID(0), "CMS 600-3 AUX", ":AUX", 3 },
};

static constexpr uint32_t kOzzyMaxDevices = sizeof(kOzzyDevices) / sizeof(kOzzyDevices[0]);

/* How many of kOzzyDevices are published: none while disconnected, only the
 * main device for a Xone. */
inline uint32_t OzzyDeviceCount(bool connected, bool dynacord)
{
	return !connected ? 0 : dynacord ? kOzzyMaxDevices : 1;
}

/* Index into kOzzyDevices of a published device or one of its streams, or -1. */
inline int OzzyDeviceIndex(uint32_t objectID, bool connected, bool dynacord)
{
	for (uint32_t i = 0; i < OzzyDeviceCount(connected, dynacord); i++) {
		const OzzyDeviceInfo& d = kOzzyDevices[i];
		if (objectID == d.device || objectID == d.inputStream || (d.outputStream && objectID == d.outputStream))
			return (int)i;
	}
	return -1;
}

/* True if objectID is one of the device's streams rather than the device. */
inline bool OzzyIsStream(uint32_t objectID)
{
	for (const OzzyDeviceInfo& d : kOzzyDevices)
		if (objectID == d.inputStream || (d.outputStream && objectID == d.outputStream))
			return true;
	return false;
}
