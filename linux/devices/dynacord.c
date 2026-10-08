// SPDX-License-Identifier: MIT
/*
 * Dynacord CMS 600-3
 *
 * A Ploytec-based mixer that shares the Xone's vendor handshake but not its
 * streaming: playback is plain S24_3LE, 4 channels, on isochronous endpoint
 * 0x02, one packet per microframe. The host paces it from a feedback stream
 * on iso endpoint 0x81 that reports the frames consumed in each millisecond.
 * The core's bulk/interrupt URB model can't express that, so this device
 * runs its own URBs through the start_urbs/stop_urbs ops.
 *
 * Capture is bulk 0x86 in the Xone's 64-byte frame format, of which only
 * channels 1-4 carry data.
 *
 * MIDI is two ports, "USB-MIDI" (the DIN sockets) and "SystemCtrl" (FX
 * control), multiplexed in 9-byte packets on bulk 0x04 out and 0x83 in (see
 * dynacord_midi.h). The firmware only moves MIDI while audio streams, so the
 * MIDI URBs live alongside the PCM ones. The mixer echoes SystemCtrl
 * messages back on SystemCtrl.
 */

#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/delay.h>

#include "../ozzy.h"
#include "../ozzy_log.h"
#include "../ozzy_pcm.h"
#include <sound/rawmidi.h>
#include "dynacord.h"
#include "../../common/devices/ploytec/ploytec_defs.h"
#include "../../common/devices/ploytec/ploytec_protocol.h"
#include "../../common/devices/ploytec/ploytec_codec.h"
#include "../../common/devices/ploytec/dynacord_pacer.h"
#include "../../common/devices/ploytec/dynacord_feedback.h"
#include "../../common/devices/ploytec/dynacord_midi.h"

/*
 * The firmware stops streaming for good if its OUT buffer runs dry, so keep
 * ~96 ms queued: 32 URBs of 24 packets (3 ms) each.
 */
#define CMS_OUT_URBS     32
#define CMS_OUT_PKTS     24
#define CMS_FB_URBS      8
#define CMS_FB_PKTS      5
#define CMS_IN_URBS      4
#define CMS_MIDI_EP_OUT  0x04
#define CMS_MIDI_EP_IN   0x83
#define CMS_MIDI_IN_URBS 4
#define CMS_MIDI_IN_MAX  512    /* wMaxPacketSize; the device sends 9 */
#define CMS_IN_URB_MAX   18432  /* 3 ms at 96 kHz, a multiple of 512 */

/* Bytes 0x1b and 0x3b of every capture frame are always this */
#define CMS_IN_MARKER    0xce

/* No valid feedback for this long means the device has stopped streaming */
#define CMS_FB_TIMEOUT_MS 50

/*
 * struct dynacord_private - CMS 600-3 runtime data.
 */
struct dynacord_private {
	unsigned char xfer_buf[16];   /* DMA-safe buffer for vendor requests */

	spinlock_t pacer_lock;        /* pacer is fed and read from two handlers */
	struct pacer pacer;

	unsigned long last_fb;        /* jiffies of the last valid feedback reading */
	bool fb_seen;                 /* any valid feedback since start_urbs */
	bool fb_lost;                 /* feedback went silent; reported once */
	bool stopping;                /* handlers must not resubmit */
	bool running;                 /* URBs allocated and submitted */
	bool misaligned;              /* capture frames lost alignment; reported once */

	unsigned int out_stride;      /* wMaxPacketSize of the PCM out endpoint */
	unsigned int fb_stride;       /* wMaxPacketSize of the feedback endpoint */

	struct urb *out[CMS_OUT_URBS];
	struct urb *fb[CMS_FB_URBS];
	struct urb *in[CMS_IN_URBS];

	struct snd_rawmidi *rmidi;
	/*
	 * Triggered substreams, NULL when not. Accessed without a lock: ALSA
	 * may call trigger(0) under the substream lock that transmit takes.
	 */
	struct snd_rawmidi_substream *midi_in_sub[DYNACORD_MIDI_PORTS];
	struct snd_rawmidi_substream *midi_out_sub[DYNACORD_MIDI_PORTS];
	spinlock_t midi_lock;         /* guards midi_out and midi_out_busy */
	struct urb *midi_out;         /* bulk 0x04, one in flight at a time */
	bool midi_out_busy;
	struct urb *midi_in[CMS_MIDI_IN_URBS];
};

static const unsigned int dynacord_rates[] = { 44100, 48000, 96000 };

static int dynacord_start_urbs(struct ozzy_chip *chip);
static void dynacord_stop_urbs(struct ozzy_chip *chip);
static int dynacord_midi_new(struct ozzy_chip *chip);

/*
 * dynacord_release - Detach the private data from the chip. Frees it
 * unless the rawmidi device owns it (see dynacord_midi_free).
 */
static void dynacord_release(struct ozzy_chip *chip)
{
	struct dynacord_private *priv = chip->private_data;

	chip->private_data = NULL;
	if (priv && !priv->rmidi)
		kfree(priv);
}

/* ========================================================================
 * Vendor Handshake
 * ======================================================================== */

static int dynacord_ctrl_in(struct ozzy_chip *chip, u8 type, u8 req, u16 val,
			    u16 idx, u16 len)
{
	struct dynacord_private *priv = chip->private_data;

	return usb_control_msg(chip->dev, usb_rcvctrlpipe(chip->dev, 0), req, type,
			       val, idx, priv->xfer_buf, len, 2000);
}

static int dynacord_ctrl_out(struct ozzy_chip *chip, u8 type, u8 req, u16 val,
			     u16 idx, u16 len)
{
	struct dynacord_private *priv = chip->private_data;

	return usb_control_msg(chip->dev, usb_sndctrlpipe(chip->dev, 0), req, type,
			       val, idx, len ? priv->xfer_buf : NULL, len, 2000);
}

/*
 * dynacord_handshake - Run the Windows driver's init sequence at a rate.
 *
 * Firmware read, status read, GET_CUR rate, five SET_CURs alternating
 * between the input (0x86) and output (0x02) endpoints, verify, then the
 * status read-modify-write that arms streaming.
 */
static int dynacord_handshake(struct ozzy_chip *chip, unsigned int rate_index)
{
	struct dynacord_private *priv = chip->private_data;
	struct ploytec_firmware_version fw;
	unsigned int rate = chip->info->rates[rate_index];
	unsigned int got;
	int i, ret;

	ret = dynacord_ctrl_in(chip, 0xC0, PLOYTEC_CMD_FIRMWARE, 0, 0, 15);
	if (ret < 0)
		return ret;
	fw = ploytec_parse_firmware(priv->xfer_buf);
	dynacord_log(&chip->dev->dev, "firmware: v1.%d.%d (chip ID: 0x%02X)\n",
		     fw.major, fw.minor, fw.chip_id);

	ret = dynacord_ctrl_in(chip, 0xC0, PLOYTEC_CMD_STATUS, 0, 0, 1);
	if (ret < 0)
		return ret;

	ret = dynacord_ctrl_in(chip, PLOYTEC_CMD_GET_RATE_TYPE,
			       PLOYTEC_CMD_GET_RATE_REQ, 0x0100, 0, 3);
	if (ret < 0)
		return ret;
	dynacord_log(&chip->dev->dev, "current rate: %u Hz, setting %u Hz\n",
		     ploytec_decode_rate(priv->xfer_buf), rate);

	for (i = 0; i < 5; i++) {
		u16 ep = i % 2 == 0 ? PLOYTEC_EP_RATE_IN : DYNACORD_EP_RATE_OUT;

		ploytec_encode_rate(rate, priv->xfer_buf);
		ret = dynacord_ctrl_out(chip, PLOYTEC_CMD_SET_RATE_TYPE,
					PLOYTEC_CMD_SET_RATE_REQ, 0x0100, ep, 3);
		if (ret < 0)
			return ret;
		usleep_range(10000, 11000);
	}

	ret = dynacord_ctrl_in(chip, PLOYTEC_CMD_GET_RATE_TYPE,
			       PLOYTEC_CMD_GET_RATE_REQ, 0x0100,
			       PLOYTEC_EP_RATE_IN, 3);
	if (ret < 0)
		return ret;
	got = ploytec_decode_rate(priv->xfer_buf);
	if (got != rate) {
		dynacord_err(&chip->dev->dev,
			     "device reports %u Hz after setting %u Hz\n", got, rate);
		return -EIO;
	}

	ret = dynacord_ctrl_in(chip, 0xC0, PLOYTEC_CMD_STATUS, 0, 0, 1);
	if (ret < 0)
		return ret;
	ret = dynacord_ctrl_out(chip, 0x40, PLOYTEC_CMD_STATUS,
				ploytec_confirm_wvalue(priv->xfer_buf[0]), 0, 0);
	if (ret < 0)
		return ret;

	chip->current_rate = rate_index;
	return 0;
}

/* ========================================================================
 * Device Ops
 * ======================================================================== */

/*
 * dynacord_init - Allocate private data, run the handshake and, the first
 * time, create the rawmidi device. Also called from post_reset, when
 * private data already exists.
 */
static int dynacord_init(struct ozzy_chip *chip)
{
	struct dynacord_private *priv = chip->private_data;
	int ret;

	if (!priv) {
		priv = kzalloc(sizeof(*priv), GFP_KERNEL);
		if (!priv)
			return -ENOMEM;
		spin_lock_init(&priv->pacer_lock);
		spin_lock_init(&priv->midi_lock);
		chip->private_data = priv;
	}

	ret = dynacord_handshake(chip, chip->requested_rate);
	if (!ret && !priv->rmidi)
		ret = dynacord_midi_new(chip);
	if (ret < 0) {
		dynacord_err(&chip->dev->dev, "init failed (ret=%d)\n", ret);
		dynacord_release(chip);
	}
	return ret;
}

static void dynacord_free(struct ozzy_chip *chip)
{
	dynacord_stop_urbs(chip);
	dynacord_release(chip);
}

/*
 * dynacord_set_rate - Change rate without a USB reset.
 *
 * A reset risks the CMS's enumeration quirk (it may need a power cycle to
 * come back), so stop streaming, reselect the alt settings, rerun the
 * handshake at the new rate, and restart. Restarts at the old rate if the
 * handshake fails.
 */
static int dynacord_set_rate(struct ozzy_chip *chip, unsigned int rate_index)
{
	struct dynacord_private *priv = chip->private_data;
	bool was_running;
	int ret, start_ret = 0;

	/* NULL if a post-reset handshake failed */
	if (!priv)
		return -ENODEV;
	if (rate_index >= chip->info->num_rates)
		return -EINVAL;
	was_running = priv->running;

	dynacord_stop_urbs(chip);

	/*
	 * Output stays silent after a rate change unless both interfaces are
	 * reselected first, as they are at probe.
	 */
	ret = usb_set_interface(chip->dev, 0, chip->info->alt_setting);
	if (!ret)
		ret = usb_set_interface(chip->dev, 1, chip->info->alt_setting);
	if (!ret)
		ret = dynacord_handshake(chip, rate_index);
	if (was_running)
		start_ret = dynacord_start_urbs(chip);

	return ret < 0 ? ret : start_ret;
}

/* ========================================================================
 * Streaming
 * ======================================================================== */

/*
 * dynacord_copy_out - Copy `bytes` from the ALSA ring at *off into dst,
 * wrapping at the end of the ring, and advance *off.
 */
static void dynacord_copy_out(u8 *dst, const u8 *ring, unsigned int ring_size,
			      unsigned int *off, unsigned int bytes)
{
	unsigned int first = min(bytes, ring_size - *off);

	memcpy(dst, ring + *off, first);
	memcpy(dst + first, ring, bytes - first);
	*off += bytes;
	if (*off >= ring_size)
		*off -= ring_size;
}

/*
 * dynacord_fill_out - Size each packet from the pacer and fill it with
 * audio from the ALSA ring, or silence when playback isn't running.
 */
static void dynacord_fill_out(struct ozzy_chip *chip, struct urb *urb)
{
	struct dynacord_private *priv = chip->private_data;
	struct pcm_substream *sub = &chip->pcm->playback;
	struct snd_pcm_substream *elapsed = NULL;
	u8 *buf = urb->transfer_buffer;
	int frames[CMS_OUT_PKTS];
	unsigned int total = 0;
	unsigned long flags;
	int i;

	spin_lock_irqsave(&priv->pacer_lock, flags);
	for (i = 0; i < CMS_OUT_PKTS; i++)
		frames[i] = pacer_next(&priv->pacer);
	spin_unlock_irqrestore(&priv->pacer_lock, flags);

	for (i = 0; i < CMS_OUT_PKTS; i++) {
		urb->iso_frame_desc[i].offset = i * priv->out_stride;
		urb->iso_frame_desc[i].length = frames[i] * DYNACORD_OUT_FRAME_SIZE;
		total += urb->iso_frame_desc[i].length;
	}

	spin_lock_irqsave(&sub->lock, flags);
	if (sub->active) {
		struct snd_pcm_runtime *alsa_rt = sub->instance->runtime;
		unsigned int ring_size = snd_pcm_lib_buffer_bytes(sub->instance);
		unsigned int off = sub->dma_off;

		for (i = 0; i < CMS_OUT_PKTS; i++)
			dynacord_copy_out(buf + urb->iso_frame_desc[i].offset,
					  alsa_rt->dma_area, ring_size, &off,
					  urb->iso_frame_desc[i].length);

		if (ozzy_pcm_advance(sub, total))
			elapsed = sub->instance;
	} else {
		memset(buf, 0, urb->transfer_buffer_length);
	}
	spin_unlock_irqrestore(&sub->lock, flags);

	if (elapsed)
		snd_pcm_period_elapsed(elapsed);
}

/*
 * dynacord_urb_dead - True if a completed URB must not be resubmitted.
 */
static bool dynacord_urb_dead(struct ozzy_chip *chip, struct urb *urb)
{
	struct dynacord_private *priv = chip->private_data;

	if (READ_ONCE(priv->stopping) || chip->pcm->panic)
		return true;

	switch (urb->status) {
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
	case -ENODEV:
		return true;
	default:
		return false;
	}
}

static void dynacord_resubmit(struct ozzy_chip *chip, struct urb *urb)
{
	int ret = usb_submit_urb(urb, GFP_ATOMIC);

	if (ret < 0 && !READ_ONCE(((struct dynacord_private *)chip->private_data)->stopping)) {
		dynacord_err(&chip->dev->dev, "resubmit on ep %02x failed (%d)\n",
			     usb_pipeendpoint(urb->pipe), ret);
		chip->pcm->panic = true;
	}
}

/* ========================================================================
 * MIDI
 * ======================================================================== */

/*
 * dynacord_midi_send - Send the next packet of pending rawmidi output.
 *
 * Takes up to 4 bytes from each triggered output port straight into its
 * slot. Called from the output trigger, chained from the 0x04 completion
 * while there is more, and every 3 ms from the PCM out handler.
 */
static void dynacord_midi_send(struct dynacord_private *priv)
{
	unsigned long flags;
	bool any = false;
	u8 *pkt;
	int port;

	spin_lock_irqsave(&priv->midi_lock, flags);
	if (!priv->midi_out || priv->midi_out_busy)
		goto out;

	pkt = priv->midi_out->transfer_buffer;
	dynacord_midi_out_init(pkt);
	for (port = 0; port < DYNACORD_MIDI_PORTS; port++) {
		struct snd_rawmidi_substream *sub = READ_ONCE(priv->midi_out_sub[port]);

		if (sub && snd_rawmidi_transmit(sub, dynacord_midi_slot(pkt, port),
						DYNACORD_MIDI_SLOT_LEN) > 0)
			any = true;
	}
	if (!any)
		goto out;

	priv->midi_out_busy = true;
	if (usb_submit_urb(priv->midi_out, GFP_ATOMIC) < 0)
		priv->midi_out_busy = false;
out:
	spin_unlock_irqrestore(&priv->midi_lock, flags);
}

static void dynacord_midi_out_complete(struct urb *urb)
{
	struct ozzy_chip *chip = urb->context;
	struct dynacord_private *priv = chip->private_data;
	unsigned long flags;

	spin_lock_irqsave(&priv->midi_lock, flags);
	priv->midi_out_busy = false;
	spin_unlock_irqrestore(&priv->midi_lock, flags);

	if (!dynacord_urb_dead(chip, urb))
		dynacord_midi_send(priv);
}

/*
 * dynacord_midi_in_complete - Hand each port's bytes to its input
 * substream, if open.
 */
static void dynacord_midi_in_complete(struct urb *urb)
{
	struct ozzy_chip *chip = urb->context;
	struct dynacord_private *priv = chip->private_data;
	const u8 *buf = urb->transfer_buffer;
	unsigned int off;
	int port;

	if (dynacord_urb_dead(chip, urb))
		return;

	if (!urb->status) {
		for (off = 0; off + DYNACORD_MIDI_PKT_LEN <= urb->actual_length;
		     off += DYNACORD_MIDI_PKT_LEN) {
			for (port = 0; port < DYNACORD_MIDI_PORTS; port++) {
				struct snd_rawmidi_substream *sub = READ_ONCE(priv->midi_in_sub[port]);
				const u8 *data;
				unsigned int n = dynacord_midi_in_slot(buf + off, DYNACORD_MIDI_PKT_LEN,
								       port, &data);

				if (n && sub)
					snd_rawmidi_receive(sub, data, n);
			}
		}
	}

	dynacord_resubmit(chip, urb);
}

static int dynacord_midi_open(struct snd_rawmidi_substream *sub)
{
	return 0;
}

static int dynacord_midi_close(struct snd_rawmidi_substream *sub)
{
	return 0;
}

static void dynacord_midi_in_trigger(struct snd_rawmidi_substream *sub, int up)
{
	struct dynacord_private *priv = sub->rmidi->private_data;

	WRITE_ONCE(priv->midi_in_sub[sub->number], up ? sub : NULL);
}

static void dynacord_midi_out_trigger(struct snd_rawmidi_substream *sub, int up)
{
	struct dynacord_private *priv = sub->rmidi->private_data;

	WRITE_ONCE(priv->midi_out_sub[sub->number], up ? sub : NULL);
	if (up)
		dynacord_midi_send(priv);
}

static const struct snd_rawmidi_ops dynacord_midi_out_ops = {
	.open    = dynacord_midi_open,
	.close   = dynacord_midi_close,
	.trigger = dynacord_midi_out_trigger,
};

static const struct snd_rawmidi_ops dynacord_midi_in_ops = {
	.open    = dynacord_midi_open,
	.close   = dynacord_midi_close,
	.trigger = dynacord_midi_in_trigger,
};

/*
 * The rawmidi device can call in until the card is freed, which may be
 * after disconnect (an open file still closes), so it owns the private
 * data from creation on.
 */
static void dynacord_midi_free(struct snd_rawmidi *rmidi)
{
	kfree(rmidi->private_data);
}

/*
 * dynacord_midi_new - Create the rawmidi device: one subdevice per port,
 * in the order of enum dynacord_midi_port.
 */
static int dynacord_midi_new(struct ozzy_chip *chip)
{
	static const char * const names[DYNACORD_MIDI_PORTS] = {
		[DYNACORD_MIDI_PORT_USB]     = "USB-MIDI",
		[DYNACORD_MIDI_PORT_SYSCTRL] = "SystemCtrl",
	};
	struct dynacord_private *priv = chip->private_data;
	struct snd_rawmidi_substream *sub;
	struct snd_rawmidi *rmidi;
	int dir, ret;

	ret = snd_rawmidi_new(chip->card, "CMS 600-3 MIDI", 0, DYNACORD_MIDI_PORTS,
			      DYNACORD_MIDI_PORTS, &rmidi);
	if (ret < 0)
		return ret;

	strscpy(rmidi->name, "CMS 600-3 MIDI", sizeof(rmidi->name));
	rmidi->info_flags = SNDRV_RAWMIDI_INFO_OUTPUT | SNDRV_RAWMIDI_INFO_INPUT |
			    SNDRV_RAWMIDI_INFO_DUPLEX;
	rmidi->private_data = priv;
	rmidi->private_free = dynacord_midi_free;
	snd_rawmidi_set_ops(rmidi, SNDRV_RAWMIDI_STREAM_OUTPUT, &dynacord_midi_out_ops);
	snd_rawmidi_set_ops(rmidi, SNDRV_RAWMIDI_STREAM_INPUT, &dynacord_midi_in_ops);

	for (dir = SNDRV_RAWMIDI_STREAM_OUTPUT; dir <= SNDRV_RAWMIDI_STREAM_INPUT; dir++)
		list_for_each_entry(sub, &rmidi->streams[dir].substreams, list)
			strscpy(sub->name, names[sub->number], sizeof(sub->name));

	priv->rmidi = rmidi;
	return 0;
}

static void dynacord_out_complete(struct urb *urb)
{
	struct ozzy_chip *chip = urb->context;
	struct dynacord_private *priv = chip->private_data;

	if (dynacord_urb_dead(chip, urb))
		return;

	if (READ_ONCE(priv->fb_seen) &&
	    time_after(jiffies, READ_ONCE(priv->last_fb) +
				msecs_to_jiffies(CMS_FB_TIMEOUT_MS))) {
		if (!xchg(&priv->fb_lost, true))
			dynacord_err(&chip->dev->dev,
				     "no feedback for %d ms: device has stopped streaming\n",
				     CMS_FB_TIMEOUT_MS);
		chip->pcm->panic = true;
		return;
	}

	dynacord_fill_out(chip, urb);
	dynacord_midi_send(priv);
	dynacord_resubmit(chip, urb);
}

static void dynacord_fb_complete(struct urb *urb)
{
	struct ozzy_chip *chip = urb->context;
	struct dynacord_private *priv = chip->private_data;
	const u8 *buf = urb->transfer_buffer;
	unsigned long flags;
	bool valid = false;
	int i;

	if (dynacord_urb_dead(chip, urb))
		return;

	spin_lock_irqsave(&priv->pacer_lock, flags);
	for (i = 0; i < urb->number_of_packets; i++) {
		struct usb_iso_packet_descriptor *d = &urb->iso_frame_desc[i];
		unsigned long long invalid = priv->pacer.invalid;
		int v;

		if (d->status)
			continue;
		v = feedback_parse(buf + d->offset, d->actual_length);
		if (v < 0)
			continue;
		pacer_feedback(&priv->pacer, v);
		if (priv->pacer.invalid == invalid)
			valid = true;
	}
	spin_unlock_irqrestore(&priv->pacer_lock, flags);

	if (valid) {
		WRITE_ONCE(priv->last_fb, jiffies);
		WRITE_ONCE(priv->fb_seen, true);
	}

	dynacord_resubmit(chip, urb);
}

/*
 * dynacord_in_complete - Decode capture frames into the ALSA ring.
 *
 * ploytec_decode_frame() gives 8 channels; the CMS fills only the first 4,
 * so copy those. Frames whose marker bytes are wrong are dropped, since
 * decoding them would give noise.
 */
static void dynacord_in_complete(struct urb *urb)
{
	struct ozzy_chip *chip = urb->context;
	struct dynacord_private *priv = chip->private_data;
	struct pcm_substream *sub = &chip->pcm->capture;
	struct snd_pcm_substream *elapsed = NULL;
	const u8 *buf = urb->transfer_buffer;
	unsigned int frames = urb->actual_length / PLOYTEC_IN_FRAME_SIZE;
	unsigned long flags;
	u8 decoded[PLOYTEC_CHANNELS * 3];
	unsigned int f;

	if (dynacord_urb_dead(chip, urb))
		return;

	spin_lock_irqsave(&sub->lock, flags);
	if (sub->active && !urb->status) {
		u8 *ring = sub->instance->runtime->dma_area;
		unsigned int ring_size = snd_pcm_lib_buffer_bytes(sub->instance);
		unsigned int off = sub->dma_off;
		unsigned int written = 0;

		for (f = 0; f < frames; f++) {
			const u8 *src = buf + f * PLOYTEC_IN_FRAME_SIZE;

			if (src[0x1b] != CMS_IN_MARKER || src[0x3b] != CMS_IN_MARKER) {
				if (!xchg(&priv->misaligned, true))
					dynacord_err(&chip->dev->dev,
						     "capture frame without 0xce markers; dropping\n");
				continue;
			}
			ploytec_decode_frame(decoded, src);
			/* the ring holds whole frames, so one never wraps */
			memcpy(ring + off, decoded, DYNACORD_OUT_FRAME_SIZE);
			off += DYNACORD_OUT_FRAME_SIZE;
			if (off >= ring_size)
				off = 0;
			written += DYNACORD_OUT_FRAME_SIZE;
		}
		if (ozzy_pcm_advance(sub, written))
			elapsed = sub->instance;
	}
	spin_unlock_irqrestore(&sub->lock, flags);

	if (elapsed)
		snd_pcm_period_elapsed(elapsed);

	dynacord_resubmit(chip, urb);
}

static struct urb *dynacord_alloc_iso(struct ozzy_chip *chip, unsigned int pipe,
				      unsigned int pkts, unsigned int stride,
				      unsigned int interval, usb_complete_t complete)
{
	struct urb *urb = usb_alloc_urb(pkts, GFP_KERNEL);
	unsigned int i;

	if (!urb)
		return NULL;
	urb->transfer_buffer = kzalloc(pkts * stride, GFP_KERNEL);
	if (!urb->transfer_buffer) {
		usb_free_urb(urb);
		return NULL;
	}
	urb->dev = chip->dev;
	urb->pipe = pipe;
	urb->transfer_flags = URB_ISO_ASAP;
	urb->transfer_buffer_length = pkts * stride;
	urb->number_of_packets = pkts;
	urb->interval = interval;
	urb->complete = complete;
	urb->context = chip;
	for (i = 0; i < pkts; i++) {
		urb->iso_frame_desc[i].offset = i * stride;
		urb->iso_frame_desc[i].length = stride;
	}
	return urb;
}

static void dynacord_free_urb(struct urb *urb)
{
	if (!urb)
		return;
	kfree(urb->transfer_buffer);
	usb_free_urb(urb);
}

/*
 * dynacord_start_urbs - Allocate and submit feedback, capture and
 * playback URBs, in that order, with the pacer reset to the current rate.
 */
static int dynacord_start_urbs(struct ozzy_chip *chip)
{
	struct dynacord_private *priv = chip->private_data;
	struct usb_device *dev = chip->dev;
	struct usb_host_endpoint *ep_out = dev->ep_out[DYNACORD_EP_PCM_OUT & USB_ENDPOINT_NUMBER_MASK];
	struct usb_host_endpoint *ep_fb = dev->ep_in[DYNACORD_EP_FEEDBACK & USB_ENDPOINT_NUMBER_MASK];
	struct usb_host_endpoint *ep_in = dev->ep_in[DYNACORD_EP_PCM_IN & USB_ENDPOINT_NUMBER_MASK];
	struct urb *midi_out = NULL;
	unsigned long flags;
	unsigned int rate, in_size;
	int i, ret;

	if (!priv)
		return -ENODEV;
	if (dev->speed != USB_SPEED_HIGH || !ep_out || !ep_fb || !ep_in) {
		dynacord_err(&dev->dev, "not a high-speed device with eps 02/81/86\n");
		return -ENODEV;
	}

	priv->out_stride = usb_endpoint_maxp(&ep_out->desc);
	priv->fb_stride = usb_endpoint_maxp(&ep_fb->desc);
	dynacord_log(&dev->dev,
		     "eps: out 02 maxp %u bInterval %u, fb 81 maxp %u bInterval %u, in 86 maxp %u\n",
		     priv->out_stride, ep_out->desc.bInterval, priv->fb_stride,
		     ep_fb->desc.bInterval, usb_endpoint_maxp(&ep_in->desc));

	/* The pacer sends one packet per microframe; HS iso bInterval is 1..16 */
	if (ep_out->desc.bInterval != 1 ||
	    ep_fb->desc.bInterval < 1 || ep_fb->desc.bInterval > 16) {
		dynacord_err(&dev->dev, "unexpected iso bInterval (out %u, fb %u)\n",
			     ep_out->desc.bInterval, ep_fb->desc.bInterval);
		return -ENODEV;
	}
	if (priv->out_stride < DYNACORD_OUT_MAX_FRAMES_PER_PKT * DYNACORD_OUT_FRAME_SIZE) {
		dynacord_err(&dev->dev, "PCM out wMaxPacketSize %u too small\n",
			     priv->out_stride);
		return -ENODEV;
	}

	/* 3 ms of capture per URB, as the Windows driver does */
	rate = chip->info->rates[chip->current_rate];
	in_size = min_t(unsigned int, ((rate * 3 / 1000) & ~7u) * PLOYTEC_IN_FRAME_SIZE,
			CMS_IN_URB_MAX);

	WRITE_ONCE(priv->stopping, false);
	priv->misaligned = false;
	priv->fb_seen = false;
	priv->fb_lost = false;
	pacer_init(&priv->pacer, rate);

	/* High-speed iso intervals are 2^(bInterval-1) microframes */
	for (i = 0; i < CMS_FB_URBS; i++) {
		priv->fb[i] = dynacord_alloc_iso(chip, usb_rcvisocpipe(dev, DYNACORD_EP_FEEDBACK),
						 CMS_FB_PKTS, priv->fb_stride,
						 1 << (ep_fb->desc.bInterval - 1),
						 dynacord_fb_complete);
		if (!priv->fb[i])
			goto nomem;
	}
	for (i = 0; i < CMS_IN_URBS; i++) {
		void *buf = kmalloc(in_size, GFP_KERNEL);

		priv->in[i] = usb_alloc_urb(0, GFP_KERNEL);
		if (!priv->in[i] || !buf) {
			kfree(buf);
			goto nomem;
		}
		usb_fill_bulk_urb(priv->in[i], dev, usb_rcvbulkpipe(dev, DYNACORD_EP_PCM_IN),
				  buf, in_size, dynacord_in_complete, chip);
	}
	for (i = 0; i < CMS_OUT_URBS; i++) {
		priv->out[i] = dynacord_alloc_iso(chip, usb_sndisocpipe(dev, DYNACORD_EP_PCM_OUT),
						  CMS_OUT_PKTS, priv->out_stride,
						  1 << (ep_out->desc.bInterval - 1),
						  dynacord_out_complete);
		if (!priv->out[i])
			goto nomem;
		dynacord_fill_out(chip, priv->out[i]);
	}

	for (i = 0; i < CMS_MIDI_IN_URBS; i++) {
		void *buf = kmalloc(CMS_MIDI_IN_MAX, GFP_KERNEL);

		priv->midi_in[i] = usb_alloc_urb(0, GFP_KERNEL);
		if (!priv->midi_in[i] || !buf) {
			kfree(buf);
			goto nomem;
		}
		usb_fill_bulk_urb(priv->midi_in[i], dev, usb_rcvbulkpipe(dev, CMS_MIDI_EP_IN),
				  buf, CMS_MIDI_IN_MAX, dynacord_midi_in_complete, chip);
	}
	midi_out = usb_alloc_urb(0, GFP_KERNEL);
	if (!midi_out)
		goto nomem;
	usb_fill_bulk_urb(midi_out, dev, usb_sndbulkpipe(dev, CMS_MIDI_EP_OUT),
			  kmalloc(DYNACORD_MIDI_PKT_LEN, GFP_KERNEL), DYNACORD_MIDI_PKT_LEN,
			  dynacord_midi_out_complete, chip);
	if (!midi_out->transfer_buffer)
		goto nomem;

	priv->running = true;

	for (i = 0; i < CMS_FB_URBS; i++) {
		ret = usb_submit_urb(priv->fb[i], GFP_KERNEL);
		if (ret < 0)
			goto fail;
	}
	for (i = 0; i < CMS_IN_URBS; i++) {
		ret = usb_submit_urb(priv->in[i], GFP_KERNEL);
		if (ret < 0)
			goto fail;
	}
	for (i = 0; i < CMS_OUT_URBS; i++) {
		ret = usb_submit_urb(priv->out[i], GFP_KERNEL);
		if (ret < 0)
			goto fail;
	}
	for (i = 0; i < CMS_MIDI_IN_URBS; i++) {
		ret = usb_submit_urb(priv->midi_in[i], GFP_KERNEL);
		if (ret < 0)
			goto fail;
	}

	/* Publish last: the rawmidi trigger may send as soon as it sees it */
	spin_lock_irqsave(&priv->midi_lock, flags);
	priv->midi_out = midi_out;
	priv->midi_out_busy = false;
	spin_unlock_irqrestore(&priv->midi_lock, flags);
	dynacord_midi_send(priv);

	dynacord_log(&dev->dev, "streaming at %u Hz\n",
		     chip->info->rates[chip->current_rate]);
	return 0;

nomem:
	ret = -ENOMEM;
fail:
	dynacord_err(&dev->dev, "starting URBs failed (%d)\n", ret);
	dynacord_free_urb(midi_out);
	priv->running = true;
	dynacord_stop_urbs(chip);
	return ret;
}

/*
 * dynacord_stop_urbs - Kill and free every URB. Safe when nothing runs.
 */
static void dynacord_stop_urbs(struct ozzy_chip *chip)
{
	struct dynacord_private *priv = chip->private_data;
	struct urb *midi_out;
	unsigned long flags;
	int i;

	if (!priv || !priv->running)
		return;

	WRITE_ONCE(priv->stopping, true);

	/* Unpublish first so the rawmidi trigger stops submitting it */
	spin_lock_irqsave(&priv->midi_lock, flags);
	midi_out = priv->midi_out;
	priv->midi_out = NULL;
	spin_unlock_irqrestore(&priv->midi_lock, flags);
	if (midi_out)
		usb_kill_urb(midi_out);
	dynacord_free_urb(midi_out);
	for (i = 0; i < CMS_MIDI_IN_URBS; i++) {
		if (priv->midi_in[i])
			usb_kill_urb(priv->midi_in[i]);
		dynacord_free_urb(priv->midi_in[i]);
		priv->midi_in[i] = NULL;
	}

	for (i = 0; i < CMS_OUT_URBS; i++)
		if (priv->out[i])
			usb_kill_urb(priv->out[i]);
	for (i = 0; i < CMS_FB_URBS; i++)
		if (priv->fb[i])
			usb_kill_urb(priv->fb[i]);
	for (i = 0; i < CMS_IN_URBS; i++)
		if (priv->in[i])
			usb_kill_urb(priv->in[i]);

	for (i = 0; i < CMS_OUT_URBS; i++) {
		dynacord_free_urb(priv->out[i]);
		priv->out[i] = NULL;
	}
	for (i = 0; i < CMS_FB_URBS; i++) {
		dynacord_free_urb(priv->fb[i]);
		priv->fb[i] = NULL;
	}
	for (i = 0; i < CMS_IN_URBS; i++) {
		dynacord_free_urb(priv->in[i]);
		priv->in[i] = NULL;
	}

	priv->running = false;
}

/* ========================================================================
 * Exported Device Descriptor and Operations
 * ======================================================================== */

const struct ozzy_device_info dynacord_info = {
	.name                  = "Dynacord CMS 600-3",
	.playback_channels     = DYNACORD_CHANNELS,
	.capture_channels      = DYNACORD_CHANNELS,
	/* one URB at 48 kHz; sizes the ALSA period and buffer limits */
	.frames_per_out_packet = CMS_OUT_PKTS * 6,
	.out_ep                = DYNACORD_EP_PCM_OUT,
	.in_ep                 = DYNACORD_EP_PCM_IN & USB_ENDPOINT_NUMBER_MASK,
	.alsa_format           = SNDRV_PCM_FMTBIT_S24_3LE,
	.bytes_per_sample      = 3,
	/* MIDI is device-owned (dynacord_midi_new), so no core MIDI eps */
	.num_interfaces        = 2,
	.alt_setting           = 1,
	.rates                 = dynacord_rates,
	.num_rates             = ARRAY_SIZE(dynacord_rates),
	.rates_mask            = SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_96000,
	.rate_min              = 44100,
	.rate_max              = 96000,
};

const struct ozzy_device_ops dynacord_ops = {
	.init       = dynacord_init,
	.free       = dynacord_free,
	.set_rate   = dynacord_set_rate,
	.start_urbs = dynacord_start_urbs,
	.stop_urbs  = dynacord_stop_urbs,
};
