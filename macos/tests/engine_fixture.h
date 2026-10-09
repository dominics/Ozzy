/* SPDX-License-Identifier: MIT */
#pragma once
#include <memory>
#include "../../common/devices/ploytec/ploytec_protocol.h"
#include "../Devices/Dynacord/DynacordEngine.h"
#include "../Shared/OzzySharedData.h"
#include "FakeBus.h"

/* A DynacordEngine on a FakeBus whose responder behaves like the mixer:
 * firmware bytes, status 0x32, and GET_CUR returning the last SET_CUR. */
struct EngineFixture {
	FakeBus bus;
	std::unique_ptr<OzzySharedMemory> shm { new OzzySharedMemory() };
	DynacordEngine engine { 8 };
	uint8_t rate[3];

	explicit EngineFixture(uint32_t r = 48000)
	{
		ploytec_encode_rate(44100, rate);
		bus.responder = [this](const FakeBus::Control& c, uint8_t* data) {
			if (c.type == 0xC0 && c.req == PLOYTEC_CMD_FIRMWARE) {
				const uint8_t fw[15] = { 0x02, 0x01, 0x05 };
				std::memcpy(data, fw, 15);
			} else if (c.type == 0xC0 && c.req == PLOYTEC_CMD_STATUS) {
				data[0] = 0x32;
			} else if (c.type == PLOYTEC_CMD_SET_RATE_TYPE) {
				std::memcpy(rate, c.out.data(), 3);
			} else if (c.type == PLOYTEC_CMD_GET_RATE_TYPE) {
				std::memcpy(data, rate, 3);
			}
			return true;
		};
		shm->magic = kOzzyMagic;
		shm->version = kOzzyLayoutFrames;
		engine.SetRate(r);
		engine.Init(&bus, shm.get());
		bus.SetEngine(&engine);
	}
};
