# Audio routing for a mobile-game stream

## What the phone actually gives you

One stream, containing **everything the phone plays** - game sound effects,
music, and your teammates' voices, already mixed together.

They cannot be separated, and this is not a limitation of Cord. An in-game
voice chat (PUBG Mobile, CoD Mobile, Free Fire) mixes teammate audio into the
game's own output *inside the game process*, before it reaches the operating
system at all. Nothing running outside that process - on any platform, with any
API - can pull them apart again.

If you genuinely need teammate voice on its own track, the voice chat has to
happen in a *separate app*. Then:

- Set **Phone audio source** to *Per-app capture (Android 13+)*, which can
  isolate that app - but only if the app permits capture. Discord permits it;
  many do not, and there is no way to tell except by trying.
- Or have the voice call on the PC instead, where OBS can capture it directly.

For in-game voice, accept the single mixed track. It is what every mobile
streamer is actually using.

## Recommended track layout

Set this up in OBS under **Settings > Output > Recording** (set Recording
Format to `mkv` and Audio Track to several tracks), then assign per source in
**Advanced Audio Properties** (right-click the mixer).

| Track | Contents | Why |
|---|---|---|
| 1 | Everything | What goes to YouTube |
| 2 | Phone (Cord) only | Lets you rebalance game vs. voice in the VOD |
| 3 | Your microphone only | Lets you re-record or noise-reduce afterwards |
| 4 | PC desktop audio only | Music, alerts, Discord on the PC |

Track 1 is the live mix; the rest cost almost nothing and save a recording when
the game turns out to have been 10 dB too loud.

## Levels

Mobile games are mastered loud. Expect to pull the phone source down
**6-12 dB** relative to your microphone, and use a compressor on your mic
rather than pushing its gain up to compete.

A practical starting point:

- Phone (Cord): -8 dB
- Your microphone: 0 dB, with a compressor (3:1, threshold around -18 dB)
- PC desktop audio: -15 dB or lower

## Sync

The plugin anchors both streams to one shared clock, derived from the device's
own timestamps, so the phone's audio stays locked to the phone's picture
(`src/util/cord-clock.h` explains the mechanism).

You should not need a sync offset. If you do see drift, it is almost always one
of:

- **Wi-Fi instead of USB.** Wi-Fi jitter is the usual culprit. Use the cable.
- **A sync offset left over** from a previous capture-card setup, still set in
  Advanced Audio Properties.
- **OBS buffering re-enabled.** The plugin disables async buffering
  deliberately; a filter or a setting that re-adds it costs frames.

## The echo trap

If you are also using **Send PC microphone to the phone**, do not enable OBS
monitoring on any source while the output device wired to the phone is OBS's
monitoring device. See [mic-wiring.md](mic-wiring.md#the-echo-trap).
