// SPDX-License-Identifier: MIT
/*
 * Ozzy USB Audio Driver - rawmidi on the engine's MIDI rings
 *
 * The engine sends MIDI out from the shared memory's midi_out ring (embedded in every PCM
 * packet on the Xones, on its own endpoint elsewhere) and fills the midi_in ring. This side
 * moves bytes between those rings and rawmidi: ozzy_midi_service() runs after every completion
 * and timer tick, so the rings keep up with the device. Output gets its running status
 * expanded on the way in (common/midi_running_status.h).
 *
 * A device with a second MIDI port (OZZY_QUIRK_MIDI_PORT2, the CMS 600-3's "SystemCtrl") gets
 * a second substream per direction on a ring pair of its own, which ozzy_midi_attach() hands
 * the engine (engine/devices/ploytec/ploytec_midi2.h).
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "ozzy.h"
#include "../engine/devices/ploytec/ploytec_midi2.h"

static uint32_t ring_free(volatile struct ozzy_midi_buffer *ring)
{
	return (ozzy_load_acquire_u32(&ring->read_index) - ring->write_index - 1) & OZZY_MIDI_RING_MASK;
}

/* Rawmidi output into the port's out ring while it has room; expanded bytes that don't fit wait */
static bool fill_out(struct ozzy_midi_port *p)
{
	bool queued = false;
	u8 byte;

	for (;;) {
		if (p->pending_count) {
			if (ring_free(p->ring_out) < p->pending_count)
				break;
			ozzy_midi_ring_push(p->ring_out, p->pending, p->pending_count);
			p->pending_count = 0;
			queued = true;
		}
		if (!p->out || snd_rawmidi_transmit(p->out, &byte, 1) != 1)
			break;
		p->pending_count = midi_running_status_expand(&p->rs, byte, p->pending);
	}
	return queued;
}

/* The port's in ring into rawmidi input (dropped while nobody listens) */
static void drain_in(struct ozzy_midi_port *p)
{
	u8 bytes[64];
	uint32_t n;

	while ((n = ozzy_midi_ring_pop(p->ring_in, bytes, sizeof(bytes))) > 0)
		if (p->in)
			snd_rawmidi_receive(p->in, bytes, n);
}

/* Any context: catch the rings up with rawmidi */
void ozzy_midi_service(struct ozzy_chip *chip)
{
	struct ozzy_midi *m = &chip->midi;
	unsigned long flags;
	unsigned int i;
	bool queued = false;

	if (!m->rmidi)
		return;
	spin_lock_irqsave(&m->lock, flags);
	for (i = 0; i < m->ports; i++) {
		drain_in(&m->port[i]);
		queued |= fill_out(&m->port[i]);
	}
	spin_unlock_irqrestore(&m->lock, flags);
	if (queued)
		ozzy_engine_midi_out_pending(chip->engine);
}

/* The port a substream belongs to (its number) */
static struct ozzy_midi_port *substream_port(struct snd_rawmidi_substream *substream)
{
	struct ozzy_chip *chip = substream->rmidi->private_data;

	return &chip->midi.port[substream->number < chip->midi.ports ? substream->number : 0];
}

static int ozzy_midi_open(struct snd_rawmidi_substream *substream)
{
	return 0;
}

static int ozzy_midi_close(struct snd_rawmidi_substream *substream)
{
	return 0;
}

static void ozzy_midi_in_trigger(struct snd_rawmidi_substream *substream, int up)
{
	struct ozzy_chip *chip = substream->rmidi->private_data;
	struct ozzy_midi_port *p = substream_port(substream);
	unsigned long flags;

	spin_lock_irqsave(&chip->midi.lock, flags);
	p->in = up ? substream : NULL;
	spin_unlock_irqrestore(&chip->midi.lock, flags);
}

/* While triggered, ozzy_midi_service keeps pulling from the substream */
static void ozzy_midi_out_trigger(struct snd_rawmidi_substream *substream, int up)
{
	struct ozzy_chip *chip = substream->rmidi->private_data;
	struct ozzy_midi_port *p = substream_port(substream);
	unsigned long flags;

	spin_lock_irqsave(&chip->midi.lock, flags);
	if (up)
		p->out = substream;
	else if (p->out == substream)
		p->out = NULL;
	spin_unlock_irqrestore(&chip->midi.lock, flags);
	if (up)
		ozzy_midi_service(chip);
}

static const struct snd_rawmidi_ops ozzy_midi_out_ops = {
	.open    = ozzy_midi_open,
	.close   = ozzy_midi_close,
	.trigger = ozzy_midi_out_trigger,
};

static const struct snd_rawmidi_ops ozzy_midi_in_ops = {
	.open    = ozzy_midi_open,
	.close   = ozzy_midi_close,
	.trigger = ozzy_midi_in_trigger,
};

/*
 * ozzy_midi_attach - The ports' rings, after ozzy_engine_attach and before the engine starts:
 * the shared memory's pair, and the second port's own pair if the engine takes it.
 */
void ozzy_midi_attach(struct ozzy_chip *chip)
{
	struct ozzy_midi *m = &chip->midi;

	m->port[0].ring_in = &chip->shm->midi_in;
	m->port[0].ring_out = &chip->shm->midi_out;
	m->port[1].ring_in = &m->midi2_in;
	m->port[1].ring_out = &m->midi2_out;
	m->ports = ozzy_engine_set_midi2(chip->engine, &m->midi2_in, &m->midi2_out) ? 2 : 1;
}

/* With two ports, each substream gets its port's name after the device's */
static void name_ports(struct snd_rawmidi *rmidi)
{
	static const char *const names[OZZY_MIDI_PORTS] = {
		DYNACORD_MIDI_PORT_USB_NAME, DYNACORD_MIDI_PORT_SYSCTRL_NAME,
	};
	struct snd_rawmidi_substream *sub;
	int dir;

	for (dir = SNDRV_RAWMIDI_STREAM_OUTPUT; dir <= SNDRV_RAWMIDI_STREAM_INPUT; dir++)
		list_for_each_entry(sub, &rmidi->streams[dir].substreams, list)
			if (sub->number < OZZY_MIDI_PORTS)
				snprintf(sub->name, sizeof(sub->name), "%s %s", rmidi->name, names[sub->number]);
}

int ozzy_midi_init(struct ozzy_chip *chip)
{
	const struct ozzy_device_profile *p = &chip->engine->profile;
	unsigned int ports = chip->midi.ports;
	struct snd_rawmidi *rmidi;
	int ret;

	/* MIDI endpoints, or MIDI in every PCM packet (the Xones) */
	if (!p->pipes[OZZY_PIPE_MIDI_IN].address && !p->pipes[OZZY_PIPE_MIDI_IN].types &&
	    !p->pipes[OZZY_PIPE_MIDI_OUT].address && !p->pipes[OZZY_PIPE_MIDI_OUT].types &&
	    chip->model->family != OZZY_FAMILY_PLOYTEC_XONE)
		return 0;

	ret = snd_rawmidi_new(chip->card, chip->card->shortname, 0, ports, ports, &rmidi);
	if (ret < 0)
		return ret;
	rmidi->private_data = chip;
	strscpy(rmidi->name, chip->dev->product ?: chip->model->name, sizeof(rmidi->name));
	rmidi->info_flags = SNDRV_RAWMIDI_INFO_OUTPUT | SNDRV_RAWMIDI_INFO_INPUT | SNDRV_RAWMIDI_INFO_DUPLEX;
	snd_rawmidi_set_ops(rmidi, SNDRV_RAWMIDI_STREAM_OUTPUT, &ozzy_midi_out_ops);
	snd_rawmidi_set_ops(rmidi, SNDRV_RAWMIDI_STREAM_INPUT, &ozzy_midi_in_ops);
	if (ports > 1)
		name_ports(rmidi);
	chip->midi.rmidi = rmidi;
	return 0;
}
