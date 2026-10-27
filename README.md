# AEH-W4A1 replacement for ESPHome

Local Home Assistant control for an air conditioner that uses the **AEH-W4A1** Wi-Fi
module. The module comes out, and an ESP32 with an RS-485 transceiver goes into its socket,
running ESPHome. It talks to the AC directly over the same RS-485 bus, using the same
frames the module used. That means no vendor app, no cloud and no Wi-Fi module dropping off
every few minutes.

It was built and tested on an **Electra**-branded split, sold in Argentina, which uses the
AEH-W4A1 and Hisense's protocol. Hisense-branded units speak the same protocol, but some
frames behaved differently on this AC than the Python library expects, so details may
differ on yours. [docs/frames.md](docs/frames.md) lists every frame and what it did here.

## What works

Every button on the remote except iFEEL and the timers can be used from Home Assistant, and
Home Assistant shows what the AC is really doing, including changes made with the remote.

| Remote | Home Assistant |
|---|---|
| POWER, MODE, TEMP | Off, heat, cool, dry, fan only and the setpoint. Each mode gets back the setpoint it last used, as on the remote. |
| SMART | Mode "auto" |
| FAN | Auto and five speeds, shown as "Speed 1" to "Speed 5" |
| QUIET | Fan mode "quiet" |
| SUPER | Preset "boost" |
| ECONOMY | Preset "eco" |
| SLEEP | Presets "sleep" and "Sleep 2" to "Sleep 4" |
| 8° HEAT | Preset "8° Heat", in heat mode |
| Swing buttons | Swing off, vertical, horizontal or both |
| DIMMER | A "Display" switch |

Ending boost, eco or sleep puts the setpoint and fan back to what they were before. The AC
doesn't do that by itself. The remote only seems to, most likely because an IR remote sends
its whole state with every press.

There's also an "AC link" sensor that turns off if the AC stops answering.

The AC beeps once for every command it accepts. The firmware sends each command exactly
once and never resends it, so one change in Home Assistant gives one beep. It reads the AC's
status every 5 seconds, plus once 1.5 seconds after each command to confirm what the AC did.

## What you need

- An ESP32 board with an RS-485 transceiver. We used a LilyGO T-CAN485, which runs straight
  from the AC's 5 V.
- Four wires from the AC's module socket to the board.
- ESPHome. This was built and tested with 2026.10.

[docs/hardware.md](docs/hardware.md) covers the socket's pinout, the wiring, the board's
pins and other boards that should work.

## Setup

1. Measure and wire the socket as described in [docs/hardware.md](docs/hardware.md).
2. Copy [example.yaml](example.yaml) into ESPHome, and add `api_encryption_key`,
   `ota_password`, `fallback_ap_password`, `wifi_ssid` and `wifi_password` to your secrets.
   The config pulls this component from GitHub:

   ```yaml
   external_components:
     - source: github://baukita/aeh-w4a1-esphome@v1.0.0
       components: [hisense_w4a1]
   ```

3. Flash the board over USB before it goes into the AC. After that, updates go over Wi-Fi.
4. Unplug the AEH-W4A1, connect the board in its place and switch the AC on. The board shows
   up in Home Assistant as an ESPHome device.

## How the frames are made

The frames come from [pyaehw4a1](https://github.com/bannhead/pyaehw4a1), copied byte for
byte by [tools/gen_hisense_commands.py](tools/gen_hisense_commands.py) into
`components/hisense_w4a1/commands.h`. A few frames pyaehw4a1 doesn't have are made from its
frames by changing single bytes, and each change was tested on the AC first. To regenerate
the file:

```bash
pip install pyaehw4a1==0.3.9
python3 tools/gen_hisense_commands.py > components/hisense_w4a1/commands.h
```

## Thanks

This only worked because other people had already done the hard parts.

- [pyaehw4a1](https://github.com/bannhead/pyaehw4a1) by bannhead, the Python library behind
  Home Assistant's integration. Its command frames and its status field table are the base
  of this firmware: every frame sent here is one of its frames, or one of its frames with a
  byte changed.
- Home Assistant's
  [Hisense AEH-W4A1 integration](https://www.home-assistant.io/integrations/hisense_aehw4a1/),
  whose rules this firmware follows, for example fan changes only in cool and fan only.
- [deiger/AirCon issue #1](https://github.com/deiger/AirCon/issues/1), where the module's
  protocol was worked out. It showed the module passes frames to the AC's RS-485 bus
  unchanged, which is what makes replacing it possible. Its reports from other AEH-W4A1
  owners whose modules kept dropping off Wi-Fi told us the module itself was the problem.
- [straga/scrivo_project issue #1](https://github.com/straga/scrivo_project/issues/1), with
  more owners' notes on wiring and boards.
- [pslawinski/esphome_airconintl](https://github.com/pslawinski/esphome_airconintl), an
  ESPHome component for the same module. It gave us the socket's pinout and the idea of
  writing the setpoint into the mode frames.
- The protocol references pyaehw4a1 cites:
  [htqwe22/device](https://github.com/htqwe22/device) and
  [cgdgithub/ControlJavaEdition](https://github.com/cgdgithub/ControlJavaEdition).
- LilyGO's [T-CAN485 page](https://github.com/Xinyuan-LilyGO/T-CAN485) and a
  [write-up on forum.hacf.fr](https://forum.hacf.fr/t/retex-pilotage-complet-dun-variteur-piscine-varipool-isaver-en-rs485-avec-esphome-et-une-lilygo-t-can485/81668)
  about driving a pool pump with the same board, which showed which pins power and switch
  its RS-485 transceiver.
- The Home Assistant community thread
  [ESPHome replacement for Hisense AC Wi-Fi modules](https://community.home-assistant.io/t/esphome-replacement-for-hisense-ac-wi-fi-modules-local-home-assistant-control/1028014),
  with wiring diagrams.
- [ESPHome](https://esphome.io) and [Home Assistant](https://www.home-assistant.io).

## License

[Apache 2.0](LICENSE), the same as pyaehw4a1. The frames in
`components/hisense_w4a1/commands.h` are generated from pyaehw4a1's, copyright bannhead,
also under Apache 2.0.

## Related projects

Other ESPHome components for Hisense-protocol modules, mostly for other module versions:

- [Druidblack/AC-Hisense](https://github.com/Druidblack/AC-Hisense), for the AEH-W4G1
- [akrabi/hisense_ac_esphome](https://github.com/akrabi/hisense_ac_esphome), for the W4F1,
  W4E1 and W4B1
- [sonooby/hisense_AEG](https://github.com/sonooby/hisense_AEG), for the W4A1 and W4G1
- [kadam12g/AEH-W4G2-ESPHome](https://github.com/kadam12g/AEH-W4G2-ESPHome), for the W4G2
