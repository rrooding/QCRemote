# QCRemote

**Your Quad Cortex, without the cable.**

The Neural DSP Quad Cortex is a great piece of gear, but seeing and controlling it from across the room usually means bending down to the unit or running a USB cable to a laptop. QCRemote is a small open-source bridge that plugs into the unit's USB port and aims to put it on your phone, tablet or laptop, wirelessly.

## What works today

The firmware connects to a Quad Cortex Mini over USB, holds a live session, and reads your presets:

```
Preset catalog: 3 folder(s), 48 preset(s) total
Preset folder "My Presets" (21 preset(s)):
  [0] Clean Rhythm
  [1] Crunch Lead
  [2] Ambient Swells
  ...
```

It also sees preset changes as you make them on the unit. Right now the output goes to a serial console, so this is an early project for tinkerers rather than a finished product.

## Where it's going

- **Wireless:** browse your presets from a phone or laptop
- **Control:** switch presets and scenes from your device
- **Deeper editing:** block parameters and footswitch assignments
- **More hardware:** the full-size Quad Cortex, and a smaller board for the final bridge

Follow progress in the [issues](https://github.com/rrooding/QCRemote/issues).

## Try it

You'll need an NXP FRDM-RW612 board, a Quad Cortex Mini, and a Zephyr toolchain. Tested so far on one unit running CorOS 4.1.1. See the [firmware README](firmware/README.md) for setup, build and flash steps.

## Learn more

- [Protocol notes](docs/PROTOCOL-NOTES.md): what we've learned about talking to the unit
- [Architecture decisions](docs/adr/): why things are built the way they are
- [Coding standards](CODING_STANDARDS.md): how to contribute code

Ideas, bug reports and test results from other units are very welcome. Please [open an issue](https://github.com/rrooding/QCRemote/issues).

## Credits and disclaimer

QCRemote builds on the protocol research in [qc-mcp](https://github.com/lexasoft123/qc-mcp).

This is an unofficial community project, not affiliated with or endorsed by Neural DSP. Quad Cortex is a trademark of its owner. The protocol is reverse-engineered and may change with any CorOS update, so use it at your own risk and keep your presets backed up.

## License

[MIT](LICENSE)
