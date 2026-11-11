# Latency: where it comes from and how it is kept down

The goal is that the picture in OBS is not visibly behind the phone. Since you
play *on the phone*, mirroring latency never affects your own gameplay - what it
affects is whether the stream looks right, and whether audio stays locked to
video. "Lag free" in practice means three separate things:

1. **Low delay** - the mirror is within a few frames of the phone.
2. **No stutter** - frames arrive evenly, nothing is dropped or repeated.
3. **No drift** - audio and video stay together over hours.

## The budget

| Stage | Cost | Controlled by |
|---|---|---|
| Phone screen -> encoder | ~1 frame (16 ms at 60fps) | Android |
| Hardware H.264 encode | 5-15 ms | `KEY_PRIORITY=0`, `KEY_LATENCY=1`, set by the device server |
| USB transfer | 1-3 ms | use a cable, not Wi-Fi |
| Socket read | <1 ms | `TCP_NODELAY` on both streams |
| Software decode | 3-8 ms | low-delay flags, slice-only threading |
| Hand to libobs | ~0 | no queue: decode happens on the reader thread |
| OBS compositing | 1 frame | OBS itself |

Realistic total: **30-60 ms** over USB at 1080p60. That is two to four frames -
below what a viewer can notice, and far below the 150-300 ms typical of
window-capturing a separate mirroring app.

## The four decisions that matter

Everything else is detail; these are the ones that would each cost whole frames
if chosen differently.

**1. `obs_source_set_async_unbuffered(source, true)`**

By default libobs buffers async frames to smooth out jittery sources like
webcams. That buffer is several frames of delay. A phone over USB delivers a
steady cadence and does not need it. This single call is the largest latency
win on the OBS side. See `src/source/phone-source.c`.

**2. Slice threading, not frame threading**

`FF_THREAD_FRAME` pipelines several frames through the decoder before emitting
the first, which is latency measured in whole frames. `FF_THREAD_SLICE` splits
each frame across cores instead. Combined with `AV_CODEC_FLAG_LOW_DELAY`, the
decoder never holds a frame back for reordering - safe here because the device
encodes with no B-frames. See `src/video/decoder.c`.

**3. Software decode, not D3D11VA**

Counter-intuitive, but libobs takes CPU-side frames, so a hardware decoder has
to copy every frame back from VRAM. That readback stalls the GPU pipeline and
costs more than it saves at 1080p60 H.264, which one modern core decodes several
times faster than real time. A zero-copy D3D11 texture path would be a genuine
improvement, but it belongs in the render callback, not in the decoder.

**4. No queues anywhere**

The socket reader thread demuxes, decodes, and calls
`obs_source_output_video()` itself. Each queue between those steps would be a
frame of latency bought for nothing - the work is fast enough that handing it to
another thread only adds a scheduling delay.

## Keeping it smooth

Delay is not the same as stutter, and stutter is more visible. In order of how
often each one is the cause:

- **Use USB.** Wi-Fi is the single most common source of stutter. It is offered
  because it is occasionally convenient, not because it is good for streaming.
- **Match frame rates.** Set the phone's frame rate limit to match the OBS
  canvas frame rate, or a clean multiple. 60 into 60 is smooth; 90 into 60 is
  not, and no amount of bitrate fixes it.
- **Give it bitrate.** 8 Mbps is fine for 1080p60 gameplay; below about 4 Mbps,
  fast-moving scenes smear. It costs nothing but USB bandwidth.
- **Watch for thermal throttling.** A phone that is hot and charging over the
  same cable will drop its encoder frame rate. If stutter appears 20 minutes in
  rather than at the start, this is why.

## Measuring it

Glass-to-glass, without instruments: point the phone's camera at the OBS preview
showing the phone's own screen, then photograph both. The difference between the
two visible clocks is the total latency. A stopwatch app on the phone makes the
numbers easy to read.

For stutter, use OBS's **Stats** dock - look at skipped frames due to encoding
lag and at the frame rate - and the plugin's own log lines, which report decoder
errors if packets are being rejected.
