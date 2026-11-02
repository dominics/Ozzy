/* SPDX-License-Identifier: MIT */
/*
 * backend-client: OzzyHAL's and OzzyMIDI's daemon backend (macos/Shared/OzzyBackend.h) outside
 * coreaudiod and MIDIServer, against fake-daemon (make check). It opens the segment and keeps
 * the mapping for all its steps, as the plug-ins do, so a daemon that hangs, crashes or is
 * replaced meanwhile (check.sh does that while it runs) is seen the way they see it. Exit
 * status 0 when each step succeeded.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "macos/Shared/OzzyBackend.h"

static uint64_t NowMs() { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000u; }

/* Poll Alive() each 250 ms (the HAL's monitor loop) for @ms; true if it ever said gone */
static bool GoneWithin(OzzyBackend& b, unsigned int ms) {
    for (uint64_t end = NowMs() + ms; NowMs() < end; usleep(250000))
        if (!b.Alive()) return true;
    return !b.Alive();
}

/* The HAL's monitor thread during a call: Alive() each 250 ms until told to stop */
struct Watch {
    OzzyBackend* b;
    std::atomic<bool> stop { false };
    bool sawGone = false;
};

static void* WatchEntry(void* arg) {
    Watch* w = (Watch*)arg;
    while (!w->stop.load()) {
        if (!w->b->Alive()) w->sawGone = true;
        usleep(250000);
    }
    return nullptr;
}

static bool MidiLoop(OzzyBackend& b, const char* hex) {
    uint8_t out[64], in[64];
    uint32_t n = 0, got = 0;
    for (; hex[0] && hex[1] && n < sizeof(out); hex += 2) {
        char pair[3] = { hex[0], hex[1], 0 };
        out[n++] = (uint8_t)strtoul(pair, nullptr, 16);
    }
    if (ozzy_midi_ring_push(&b.shm->midi_out, out, n) != n) return false;
    b.MidiOutPending();
    for (uint64_t end = NowMs() + 1000; got < n && NowMs() < end; usleep(1000))
        got += ozzy_midi_ring_pop(&b.shm->midi_in, in + got, sizeof(in) - got);
    return got == n && memcmp(in, out, n) == 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: backend-client LOCATION [--open-fails] [--alive] [--gone MS] [--set-rate HZ]\n"
                        "       [--set-rate-watched HZ] [--midi-loop HEX] [--midi2 NAME]\n");
        return 2;
    }
    uint32_t location = (uint32_t)strtoul(argv[1], nullptr, 16);
    OzzyBackend b;
    bool ok = true;

    if (argc > 2 && !strcmp(argv[2], "--open-fails")) {
        ok = !b.OpenDaemon(location);
        printf("backend: open %s\n", ok ? "failed, as expected" : "succeeded");
        if (b.IsOpen()) b.Close();
        return ok ? 0 : 1;
    }
    if (!b.OpenDaemon(location)) {
        fprintf(stderr, "backend: no daemon segment for %08x\n", location);
        return 1;
    }
    printf("backend: open, session 0x%08x, %u Hz\n", b.shm->session_id, b.shm->audio.sample_rate.load());
    for (int i = 2; i < argc && ok; i++) {
        const char* a = argv[i];
        const char* p = i + 1 < argc ? argv[i + 1] : nullptr;
        if (!strcmp(a, "--alive")) {
            usleep(300000);   // past a tick
            ok = b.Alive() && b.shm->magic == OZZY_MAGIC;
            printf("backend: %s\n", ok ? "alive" : "not alive");
        } else if (p && !strcmp(a, "--gone")) {
            ok = GoneWithin(b, (unsigned int)atoi(p));
            printf("backend: %s within %s ms\n", ok ? "gone" : "still alive", p);
            i++;
        } else if (p && !strcmp(a, "--set-rate")) {
            uint64_t in[1] = { strtoull(p, nullptr, 10) };
            kern_return_t kr = b.Call(OZZY_METHOD_SET_RATE, in, 1);
            ok = kr == KERN_SUCCESS && b.shm->audio.sample_rate.load() == in[0];
            printf("backend: SET_RATE %s: 0x%x, %u Hz\n", p, kr, b.shm->audio.sample_rate.load());
            i++;
        } else if (p && !strcmp(a, "--set-rate-watched")) {
            // A slow restart (the daemon's handshake) mustn't look like a dead daemon
            Watch w;
            pthread_t t;
            w.b = &b;
            pthread_create(&t, nullptr, WatchEntry, &w);
            uint64_t in[1] = { strtoull(p, nullptr, 10) };
            uint64_t start = NowMs();
            kern_return_t kr = b.Call(OZZY_METHOD_SET_RATE, in, 1);
            uint64_t took = NowMs() - start;
            usleep(300000);
            w.stop.store(true);
            pthread_join(t, nullptr);
            ok = kr == KERN_SUCCESS && !w.sawGone && b.Alive();
            printf("backend: SET_RATE %s took %llu ms: 0x%x, %s\n", p, (unsigned long long)took, kr,
                   w.sawGone ? "seen as gone meanwhile" : "alive throughout");
            i++;
        } else if (p && !strcmp(a, "--midi2")) {
            // The second MIDI port as OzzyMIDI finds it
            struct ozzy_daemon_midi2* m2 = b.Midi2();
            ok = m2 && !strncmp(m2->name, p, sizeof(m2->name));
            printf("backend: second MIDI port %s\n", m2 ? m2->name : "(none)");
            i++;
        } else if (p && !strcmp(a, "--midi-loop")) {
            ok = MidiLoop(b, p);
            printf("backend: MIDI loop %s\n", ok ? "ok" : "failed");
            i++;
        } else {
            fprintf(stderr, "backend: bad step '%s'\n", a);
            ok = false;
        }
        fflush(stdout);
    }
    b.Close();
    return ok ? 0 : 1;
}
