# Cord

An OBS Studio plugin for streaming mobile games: pulls an Android phone's
screen and sound into OBS as a normal source, and sends your PC microphone back
to the phone so teammates can hear you.

**Nothing is installed on your phone.** No APK, no launcher icon, no account.
You turn on USB debugging once, plug in a cable, and add a source in OBS.

```
  Android phone                     Windows PC / OBS Studio
  ─────────────                     ───────────────────────
  screen  ──┐                    ┌──  "Phone (Cord)" source
            ├─ USB (ADB) ────────┤    video + phone audio
  audio   ──┘                    └──  assignable to OBS tracks
     ▲
     └── headset mic input ◄── analog cable ◄── your PC microphone
```

## What it does

| Feature | How |
|---|---|
| Phone screen into OBS | Hardware-encoded H.264/H.265/AV1 over USB, 30-60 ms, 1080p60 |
| Phone sound into OBS | The phone's whole output mix, as OBS source audio |
| Teammates' voices into OBS | Part of that same mix - see below |
| PC microphone into the phone | Dedicated low-latency WASAPI send over an analog cable |

### Two things worth knowing up front

**Teammate voice arrives mixed with the game.** It comes through - that is the
main reason this works where screen-recording apps do not - but it cannot be
split onto its own track. An in-game voice chat mixes teammate audio inside the
game process before the operating system ever sees it, so nothing outside that
process can separate them. [audio-routing.md](docs/audio-routing.md) explains
the options if you need them separate.

**The microphone send needs a cable.** Android provides no way for software to
act as your phone's microphone - no API, and root does not come into it unless
you want it to. The supported path is analog: PC output, through an attenuator,
into a USB-C adapter with a mic input. About £15 of parts, works on any phone.
[mic-wiring.md](docs/mic-wiring.md) has the parts list and the test procedure.

## Requirements

- Windows 10/11
- OBS Studio 30 or newer
- Android 11 or newer (Android 11+ is required for audio capture; video works
  further back)
- A USB cable. Wi-Fi is supported and is a bad idea for streaming.

## Setup

1. Install the plugin: run `install.bat` (it finds OBS Studio or Streamlabs
   Desktop and copies the plugin in; `/uninstall` removes it). Building it
   first: [building.md](docs/building.md).
2. On the phone: **Settings -> About -> tap Build number seven times**, then
   **Developer options -> USB debugging**.
3. Plug in over USB and tap **Allow** on the phone when prompted.
4. In OBS: **+ -> Phone (Cord)**, pick your phone, press OK.

Then read [audio-routing.md](docs/audio-routing.md) before your first stream -
mobile games are mastered loud, and the default balance will bury your voice.

## Documentation

- [docs/building.md](docs/building.md) - prerequisites, build, install
- [docs/latency.md](docs/latency.md) - the latency budget, and keeping it smooth
- [docs/audio-routing.md](docs/audio-routing.md) - track layout, levels, sync
- [docs/mic-wiring.md](docs/mic-wiring.md) - the PC-mic-to-phone cable

## How it works

The plugin bundles the [scrcpy](https://github.com/Genymobile/scrcpy) device
server. Over ADB it pushes a small `.jar` to `/data/local/tmp` and runs it with
`app_process` as the `shell` user - which is why nothing is installed, and why
it disappears on reboot.

Running as `shell` also means it holds `CAPTURE_AUDIO_OUTPUT`, so it can capture
the device's entire audio output mix. An installed app could only use
MediaProjection, which apps are allowed to opt out of - and many games and chat
apps do, which is why screen-recording apps so often give you silent teammate
voice.

On the PC side the plugin demuxes that stream, decodes it, and hands frames
straight to libobs on the thread that read them - no intermediate queues, async
buffering switched off. See [docs/latency.md](docs/latency.md).

## Project status

Early. The capture path, audio path, device management and microphone send are
implemented; a Qt dock panel with live statistics and a wiring wizard is not yet
written, and the forward-tunnel fallback for OEM ROMs that refuse reverse
tunnels has not been tested on hardware that needs it.

## Licence

GPLv3. OBS Studio is "GPLv2 or later", and v3 is what makes the bundled
Apache-2.0 scrcpy device server compatible - that server is a separate process
this plugin speaks a socket protocol to, never linked into the binary.

`adb.exe` and `scrcpy-server` are redistributed from the scrcpy Windows release
and are not in this repository; `scripts/fetch-deps.ps1` downloads them.
