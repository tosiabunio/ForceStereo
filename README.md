# ForceStereo

A virtual 2-channel output device for macOS that plays on channels 1/2 of a real audio interface.

Some interfaces (for example the SSL 2+) present themselves as 4-channel devices even when only two outputs are used. ForceStereo gives the system a plain stereo device to play into and passes that audio, unchanged, to the first two channels of the interface.

## How it works

There are two parts:

- **`ForceStereo.driver`** — a Core Audio HAL plug-in that shows up as a stereo output device named "ForceStereo". It has its own volume and mute controls, so the keyboard volume keys work.
- **`forcestereo`** — a background helper that takes the audio played into ForceStereo and plays it on the real device.

The helper only ever opens the real device for output, so macOS does not ask for microphone access and does not show the recording indicator.

Audio is not resampled. The helper keeps both devices at the same sample rate and steers the virtual device's clock to follow the hardware, so samples reach the interface as they were written.

## Requirements

- macOS 12 or later
- Xcode command line tools (`clang`, `make`, `codesign`)

## Install

```bash
./install.sh
```

The script:

1. builds the driver and the helper,
2. copies the driver to `/Library/Audio/Plug-Ins/HAL` (asks for your sudo password),
3. restarts `coreaudiod` — all audio drops for about a second,
4. installs the helper in `~/Library/Application Support/ForceStereo` and a LaunchAgent that starts it at login.

Then choose **ForceStereo** as the sound output in System Settings.

## Uninstall

```bash
./uninstall.sh
```

## Behaviour

- **Target device:** the first connected USB, Thunderbolt, FireWire, PCI or AVB interface. If none is connected, the built-in output is used.
- **Default output:** when the target appears and macOS makes it the default output, the helper switches the default to ForceStereo. If you select the interface by hand later, the helper leaves that alone.
- **Sample rate:** ForceStereo follows the interface. If you change the rate on ForceStereo, the helper applies it to the interface when the interface supports it.
- **Idle:** the helper stops the real device a few seconds after nothing is playing, so the Mac can still go to sleep.
- **Latency:** estimated at 30–40 ms end to end; it has not been measured.

## Helper options

The LaunchAgent runs the helper with no options. To change them, edit `ProgramArguments` in `~/Library/LaunchAgents/local.forcestereo.helper.plist` and reload the agent.

| Option | Meaning |
| --- | --- |
| `--target <uid\|name>` | Pin the output device by UID or by part of its name |
| `--channels <L,R>` | Device channels to play on (default `1,2`) |
| `--latency <ms>` | Safety buffer kept in the helper (default `10`) |
| `--buffer <frames>` | IO buffer size on the output device (default 256 at 44.1/48 kHz) |
| `--no-default` | Never change the system default output |
| `--null` | Play nothing; only drain the device and print statistics |
| `-v` | Print statistics every 5 seconds |

To see the output devices and which one would be used:

```bash
./build/forcestereo list
```

## Troubleshooting

The helper logs to `~/Library/Logs/ForceStereo.log`.

For live statistics, stop the agent and run the helper by hand:

```bash
launchctl bootout gui/$(id -u)/local.forcestereo.helper
```

```bash
./build/forcestereo -v
```

Each line shows the buffered frames, the clock correction in ppm, and counts of underruns, skipped frames and resyncs. Start the agent again with:

```bash
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/local.forcestereo.helper.plist
```

## Layout

| Path | Contents |
| --- | --- |
| `driver/` | HAL plug-in source and `Info.plist` |
| `helper/` | Forwarding helper |
| `shared/` | Definitions used by both |
| `install.sh`, `uninstall.sh` | Install and removal |
