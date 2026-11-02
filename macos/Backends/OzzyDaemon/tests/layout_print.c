/* SPDX-License-Identifier: MIT */
/* The shared memory's size and the trailer's offset as this language sees them. Built as C with
 * the engine's types (the daemon) and as C++ with std::atomic (OzzyHAL, OzzyMIDI); make check
 * compares the two. */

#include "../../../../engine/shared/ozzy_shared_data.h"
#include "../../../Shared/OzzyDaemonShared.h"
#include <stddef.h>
#include <stdio.h>

int main(void)
{
	printf("shared memory %zu\n", sizeof(struct ozzy_shared_memory));
	printf("audio at %zu\n", offsetof(struct ozzy_shared_memory, audio));
	printf("trailer at %llu\n", (unsigned long long)OZZY_DAEMON_TRAILER_OFFSET(sizeof(struct ozzy_shared_memory)));
	printf("trailer %zu, mailbox at %zu\n", sizeof(struct ozzy_daemon_trailer), offsetof(struct ozzy_daemon_trailer, mailbox));
	printf("segment %llu\n", (unsigned long long)ozzy_daemon_segment_size(sizeof(struct ozzy_shared_memory)));
	return 0;
}
