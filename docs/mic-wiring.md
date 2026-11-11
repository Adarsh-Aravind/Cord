# Getting your PC microphone into the phone

This is the one feature that needs a cable. It is worth understanding why, so
you do not spend an evening looking for an app that cannot exist.

## Why software cannot do this

Android has no virtual microphone. An app can read the mic, and (since Android
10) it can read what other apps are *playing*, but nothing can *write* into the
microphone input that another app reads. There is no API for it, ADB cannot
provide one, and USB debugging does not help: the restriction is in the audio
framework, not in the permission model.

So your options are:

| Path | Works on | Trade-off |
|---|---|---|
| **Analog send** (this guide) | Any phone, no root | Needs ~£15 of adapters |
| Plug your mic into the phone | Any phone, no root | You lose your PC audio interface |
| Rooted virtual mic (Magisk) | Rooted phones only | Unlocked bootloader, breaks banking apps |

Cord implements the analog send, and can also capture the phone's own mic
as a source if you choose the second option instead (set **Phone audio source**
to *Phone microphone*).

## The signal chain

```
  PC audio output
        |
        |   line level, roughly 1-2 V       <-- far too hot for a mic input
        v
  [ line-to-mic attenuator ]                <-- drops it to ~10-30 mV
        |
        v
  [ USB-C audio adapter with a mic input ]  <-- must support TRRS / CTIA
        |
        v
  phone, which now hears a headset microphone
```

### Parts

1. **A USB-C (or Lightning) audio adapter with microphone support.** Critical:
   many adapters are output-only. Look for *TRRS*, *CTIA* or "headset" in the
   description, not just "headphone". A USB-C audio *interface* works too and
   usually sounds better.

2. **A line-to-mic attenuator cable** (sometimes sold as a "line level
   converter" or "-20 dB attenuator", 3.5 mm TRS to TRRS). This is not optional.
   A PC line output is roughly 100x hotter than a phone mic input expects.
   Without attenuation you get violent clipping, and teammates hear distortion
   rather than your voice.

   If you already own a mixer or audio interface with a mic-level or instrument
   output, use that instead and skip the attenuator.

3. **A 3.5 mm TRRS splitter** if you want to keep monitoring the phone's audio
   through the same adapter.

### Which PC output to use

Use a **dedicated output**, not your speakers or headphones. A cheap USB sound
card for a few pounds is ideal: it shows up as its own Windows device, so you
can send your microphone to it without sending anything else, and without the
Windows volume mixer getting in the way.

Then in the source properties:

- **Output wired to the phone** - that USB sound card
- **Microphone to send** - your real microphone
- **Send level** - start at 0 dB and adjust using the test described below

## Verifying it before you go live

1. Open a voice recorder app on the phone (not the game).
2. Enable **Send PC microphone to the phone** in the source properties.
3. Record a few seconds while talking.
4. Play it back on the phone.

If it is silent, the adapter probably has no mic input. If it is distorted or
crackling, the attenuator is missing or too weak. If it is very quiet, raise
**Send level** a few dB at a time - and prefer raising the level in Windows'
own device properties first, which is cleaner than digital gain here.

Once that works, test in a real match and ask a teammate. They are the only
real level meter you have.

## The echo trap

**Never set OBS's monitoring device to the output wired to the phone.**

If you do, OBS plays the phone's own audio back down the cable into the phone's
mic input. Teammates then hear themselves a fraction of a second late, which
builds into a howl. Cord checks for this when you enable the send and
writes a warning to the OBS log, but it cannot stop you.

The safe arrangement: OBS monitoring off (or pointed at your headphones), the
phone send device used by nothing except this bridge.

## Latency

The bridge runs on its own WASAPI path with 10 ms periods and Pro Audio thread
priority, independent of OBS's mixer, so it adds roughly 10-20 ms. That is
below the threshold where a conversation starts to feel awkward, and well under
what OBS monitoring would add.

The analog hop itself adds nothing measurable.
