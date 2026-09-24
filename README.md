# razer-pod-bridge

[More Drumandbytes projects](https://drumandbytes.com/projects/)

Use a Razer Wireless Control Pod as a plug-and-play USB media controller for macOS without Razer Synapse.

The firmware runs on a Seeed Studio XIAO nRF52840 Sense. It connects to the pod over Bluetooth Low Energy, translates its consumer-control reports, and exposes a standard USB HID device to the Mac. Pairing is stored in flash, so the bridge reconnects after losing power.

## Controls

| Pod input | macOS action |
| --- | --- |
| Turn left/right | Volume down/up |
| Single tap | Mute after 0.5 seconds |
| Single tap, then turn within 0.5 seconds | Brightness down/up |
| Double tap | Play/pause after 0.5 seconds |
| Double tap, then turn within 0.5 seconds | Previous/next track |
| Triple tap | Lock Mac (`Control-Command-Q`) |

Rotation keeps the active brightness or track mode alive for another second. Duplicate BLE reports are ignored. Fast spins drop steps rather than queueing them, so the dial never keeps moving after you stop.

If the Mac is asleep, any pod input wakes it; that input isn't applied. This needs the Mac to keep USB powered while asleep.

Volume and brightness move in quarter steps (64 across the full range instead of 16): the bridge holds Shift+Option with each key, the macOS shortcut for fine steps.

The timings and fine steps are constants at the top of `src/main.cpp` (`TAP_WINDOW_MS`, `MODE_HOLD_MS`, `FINE_STEPS`).

## Cross-Platform Support

This firmware is designed primarily for macOS, but it can be easily adapted for other operating systems. If you want to use this firmware on Windows or Linux, you can fork the repository and modify the firmware to send the appropriate lock commands for your operating system.

### Windows

To lock a Windows machine, you would need to modify the firmware to send the `Win+L` combination instead of `Control-Command-Q`.

### Linux

To lock a Linux machine, you would need to modify the firmware to send the `Super+L` combination instead of `Control-Command-Q`.

## Hardware

- Razer Wireless Control Pod
- Seeed Studio XIAO nRF52840 Sense
- USB-C cable or adapter connecting the XIAO to the Mac or dock

The regular XIAO nRF52840 should also be compatible, but it has not been tested. Other nRF52840 boards may work after changing the PlatformIO board configuration and USB/BLE setup.

## Build and upload

Install [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html), then run:

```sh
pio run
pio run --target upload
```

If automatic upload reset fails, double-tap the XIAO reset button to enter its bootloader, then run the upload command again.

## First pairing

1. Put the Razer Wireless Control Pod into full pairing mode.
2. Power or reset the XIAO.
3. Wait a few seconds for the connection.
4. Test the dial while the XIAO is connected to the Mac over USB.

The bond persists across normal resets and power loss. Re-enter pairing mode only if either device's bond is cleared or the pod is paired to another host.

## Development

View build output:

```sh
pio run
```

View serial output when troubleshooting:

```sh
pio device monitor --baud 115200
```

The log shows scan, connect, secure and disconnect events, plus each pod report and each USB key sent. Every line starts with a millisecond timestamp, so the gap between `pod found` and `ready` is how long a reconnect takes.

## License

MIT
