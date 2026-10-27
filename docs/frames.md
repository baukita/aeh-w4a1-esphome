# Frames

This is what the firmware sends to the AC and reads back, and what each frame did on our
unit, an Electra-branded split that uses the AEH-W4A1 module. Everything here was checked on
that one AC in October 2026, with the IR remote as the reference for what each button really
sets. A Hisense-branded unit speaks the same protocol, but some frames may behave
differently, as several did compared with what pyaehw4a1 expects.

The frames come from [pyaehw4a1](https://github.com/bannhead/pyaehw4a1) 0.3.9. Its update
frames and its status field table are the starting point, and `tools/gen_hisense_commands.py`
copies the frames from it. Byte numbers below count from the `F4` that starts a frame, before
escaping, and bit numbers follow pyaehw4a1's `DataPacket.data_102_0` table.

## Framing

```
F4 F5 | 00 | 40 | len | ... | type  | ... data ... | checksum | F4 FB
 0  1    2    3    4          13 14                   2 bytes
```

- Every frame starts with `F4 F5` and ends with `F4 FB`.
- Byte 2 is `00` on frames sent to the AC and `01` on frames from it.
- Byte 4 is the frame's length minus 9.
- Bytes 13 and 14 give the frame type: `66 00` is a full status frame and `65 00` is an update
  (a command).
- The checksum is the sum of every byte from byte 2 up to the checksum, kept to 16 bits and
  sent high byte first.
- Any `F4` between the start and end markers, the checksum included, is sent as `F4 F4`. For
  example, the 16 °C frame's checksum `01 F4` goes on the wire as `01 F4 F4`, followed by
  `F4 FB`.

The bus runs at 9600 baud. The AC only speaks when asked. A status request is

```
F4 F5 00 40 0C 00 00 01 01 FE 01 00 00 66 00 00 00 01 B3 F4 FB
```

and the AC answers in about 220 ms with an 82-byte status frame. It answers each update frame
with a type `65 00` frame that also carries the full status. The display frames get no answer
at all, but they still work.

The AC beeps once for every update frame it accepts, and no frame turns the beep off. That's
why the firmware never sends a command twice, even when no answer comes back. It confirms what
happened with a status read 1.5 seconds after the last command instead.

## Status fields

The status data starts at byte 16. The firmware reads these fields:

| Field (pyaehw4a1 name) | Bits | Meaning on this AC |
|---|---|---|
| `wind_status` | 1–8 | Fan speed, see below |
| `sleep_status` | 9–15 | 0 off, 1–4 sleep level |
| `mode_status` | 17–20 | 0 fan only, 1 heat, 2 cool, 3 dry, 5 SMART |
| `run_status` | 21 | 1 on |
| `indoor_temperature_setting` | 25–32 | Setpoint, °C |
| `indoor_temperature_status` | 33–40 | Room temperature from the indoor unit's own sensor, whole °C |
| `temperature_Fahrenheit` | 87 | 0 for Celsius |
| `up_down` | 153 | Up/down swing on |
| `left_right` | 154 | Side-to-side swing on |
| `low_electricity` | 158 | Eco on |
| `efficient` | 159 | Turbo (SUPER on the remote) on |
| `mute` | 166 | Quiet fan on |
| `back_led` | 169 | Display lit |

On this AC the display shows in bit 169. Bit 170, which pyaehw4a1 calls `display_led`, stays
0. SMART (mode 5) isn't in pyaehw4a1's table.

The fan values differ from what pyaehw4a1 and the Home Assistant integration expect, which
is why every manual speed used to show as "auto":

| Remote | `wind_status` | pyaehw4a1 expects |
|---|---|---|
| Auto in cool, fan only and dry | 1 | 1 |
| Auto in heat and SMART | 0 | 0 |
| Quiet | 2 | 2 |
| Speed 1 (lowest) | 10 | 4 means low |
| Speed 2 | 12 | none |
| Speed 3 | 14 | 6 means medium |
| Speed 4 | 16 | none |
| Speed 5 (highest) | 18 | 8 means high |

`temperature_compensation` (bits 81–85) follows the setpoint. It reads 0 at 16 °C, 16 at 24 °C
and 20 at 26 °C.

## Update frame layout

An update frame is 50 bytes long. Bytes 16 to 45 hold the settings, bytes 46 and 47 the
checksum and bytes 48 and 49 `F4 FB`. Each setting is written as its value followed by one
"apply" bit, and the AC leaves a setting alone when its apply bit is 0. So a frame changes only
the settings it sets, and one frame can change several at once.

| Byte | Setting | Values |
|---|---|---|
| 16 | Fan | Value × 2 + 1. `01` auto and `03` quiet (with byte 35). pyaehw4a1 sends `05`, `07` and `09`, which give speeds 1, 3 and 5. `0D` and `11` give speeds 2 and 4. |
| 17 | Sleep | Level × 2 + 1. `01` ends sleep, and `03`, `05`, `07` and `09` set levels 1 to 4. |
| 18 | Mode and power | The mode in the top four bits, as mode × 2 + 1: fan only `1`, heat `3`, cool `5`, dry `7`, SMART `B`. Power in bits 3–2: `0C` on, `04` off. |
| 19 | Setpoint | °C × 2 + 1. 16 °C is `21`, 24 °C is `31`, 32 °C is `41`, and 8 °C is `11`. |
| 23 | Unknown | Every pyaehw4a1 frame sets `04` here, and so does the firmware. |
| 32 | Swing | Up/down in bits 7–6 (`C0` swing, `40` stop). Side-to-side in bits 5–4 (`30` swing, `10` stop). |
| 33 | Low power, eco, turbo, dual frequency | Two bits each. Eco on `30`, eco off `10`, turbo on `0C`, turbo off `04`. These can be combined, for example `14` for eco off and turbo off. |
| 35 | Quiet | Bits 5–4: `30` on, `10` off. |
| 36 | Display | Bits 7–6: `C0` lit, `40` dark. |

pyaehw4a1's mode, off and swing frames also set bytes 24, 29, 31, 35 and 37. We didn't work
out what those do, so the firmware sends those frames unchanged apart from the setpoint.

A frame that changes nothing has every byte from 16 to 45 at 0 except byte 23:

```
F4 F5 00 40 29 00 00 01 01 FE 01 00 00 65 00 00   bytes 0-15
00 00 00 00 00 00 00 04 00 00 00 00 00 00 00 00   bytes 16-31
00 00 00 00 00 00 00 00 00 00 00 00 00 00         bytes 32-45
01 D3 F4 FB                                       checksum, end
```

The firmware builds its preset frames from this one. Changing one byte changes the checksum by
the same amount, since the checksum is a plain sum.

## What the firmware sends

`components/hisense_w4a1/commands.h` holds the frames without checksum or escaping, and the
firmware adds both when it sends one. Some frames get a byte changed at that point, as listed
below.

| In Home Assistant | Frame | What the AC does |
|---|---|---|
| Turn on | pyaehw4a1 `on` | Turns on in the mode it was last in. |
| Turn off | pyaehw4a1 `off` | Turns off. The frame also carries turbo off, eco off and sleep off. |
| Heat, cool, dry or fan only | pyaehw4a1 `mode_heat`, `mode_cool`, `mode_dry` or `mode_fan`, with byte 19 set to the setpoint last used in that mode | Changes mode and sets auto fan, except in fan only. The frame also carries sleep off and turbo off, and the AC ended eco on a mode change as well. |
| Auto | The do-nothing frame with byte 18 = `B0` | SMART: the AC picks the mode and setpoint from the room temperature. |
| Setpoint | pyaehw4a1 `temp_16_C` to `temp_32_C` | Changes only the setpoint. |
| Fan auto, quiet, speeds 1, 3 and 5 | pyaehw4a1 `speed_auto`, `speed_mute`, `speed_low`, `speed_med` and `speed_max` | Changes the fan. The frames also carry sleep off and turbo off. |
| Fan speeds 2 and 4 | pyaehw4a1 `speed_low` with byte 16 = `0D`, and `speed_max` with byte 16 = `11` | The same, for the two speeds pyaehw4a1 skips. |
| Swing | pyaehw4a1 `vert_swing`, `vert_dir`, `hor_swing` and `hor_dir` | Each axis has its own start and stop frame. |
| Display switch | pyaehw4a1 `display_on` and `display_off` | Lights or darkens the panel. |
| Boost | Do-nothing frame, byte 33 = `0C` | Turbo flag on. The AC sets 16 °C and the highest fan in cool, or 32 °C in heat. |
| Eco | Do-nothing frame, byte 33 = `30` and byte 16 = `05` | Eco flag on, fan to speed 1, as the remote's ECONOMY button does. In SMART the AC ignores the fan part. |
| Sleep, Sleep 2, 3 and 4 | Do-nothing frame, byte 17 = level × 2 + 1 | Sleep on. The AC drops the fan to speed 1 itself, and the display goes dark about 12 seconds later. |
| 8° Heat (heat mode only) | Do-nothing frame, byte 19 = `11` | Setpoint 8 °C, the remote's 8° HEAT. The AC has no flag for it, so the firmware shows this preset whenever heat is set below 16 °C. |
| No preset | Do-nothing frame with the off value for whatever was on (byte 33, byte 17), plus the setpoint (byte 19) and fan (bytes 16 and 35) from before the preset | Ends the preset and puts the setpoint and fan back. |

A setpoint or fan change while boost is on ends boost in the same frame: turbo off plus the
new setpoint and the old fan, or the new fan and the old setpoint. A setpoint change leaves eco
and sleep on.

The setpoint for each mode is learned from the AC's status while it runs in that mode and is
kept in flash. Until a mode has been seen, a mode change keeps the current setpoint.

## What differed from pyaehw4a1 on this AC

- pyaehw4a1's `turbo_on` works, but the turbo flag stays 0, so Home Assistant could never show
  boost or turn it off. Its `energysave_on` and `sleep_1` to `sleep_4` frames change nothing.
  Each of those frames also carries other settings, such as the fan, sleep off or turbo off.
  The same settings sent alone, in the do-nothing frame, work and set the flags.
- Ending turbo or sleep doesn't put the setpoint and fan back. The remote only seems to,
  most likely because an IR remote sends its whole state with every button press. So the
  firmware remembers them and sends them back itself.
- pyaehw4a1's mode frames carry a fixed setpoint: heat 23 °C, cool 26 °C, dry and fan only
  25 °C. Sent unchanged, they overwrite yours, so the firmware writes the setpoint into byte 19
  first, the way [pslawinski/esphome_airconintl](https://github.com/pslawinski/esphome_airconintl)
  does.
- pyaehw4a1's three fan speeds reach the AC's lowest, middle and highest of five, and the
  status reports values pyaehw4a1 doesn't know.
- The display is in status bit 169, not 170.

## Trying a frame from Home Assistant

With `test_frames: true` in the `climate:` entry, the board offers an action in Home Assistant
called `esphome.<device name>_send_test_frame`. It's off by default and meant for working out
frames the firmware doesn't send yet. Every frame it sends makes the AC beep.

The action's one field, `frame`, is an update frame in hex, from the `F4 F5` up to but not
including the checksum. That's 46 bytes for a normal update frame. Spaces, colons, commas and
dashes between the bytes are ignored. The firmware adds the checksum, the `F4 F4` escaping and
the `F4 FB` end marker. It refuses the frame, with a warning in the log, unless byte 2 is `00`,
bytes 13 and 14 are `65 00`, and byte 4 is the number of bytes typed minus 5.

For example, this is the do-nothing frame with byte 19 set to `31`, which sets 24 °C, on a
device named `hisense-ac`:

```yaml
action: esphome.hisense_ac_send_test_frame
data:
  frame: >-
    F4 F5 00 40 29 00 00 01 01 FE 01 00 00 65 00 00
    00 00 00 31 00 00 00 04 00 00 00 00 00 00 00 00
    00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

A test frame waits in the same queue as every other command. It's sent once and never re-sent,
and a status read 1.5 seconds later shows what the AC did. Only one test frame can wait at a
time, so a second one sent before the first has gone out is refused. The log shows the frame and
its checksum when it's queued, and the bytes as they go on the wire in a line starting with
"Sending test frame".

## Seeing the frames yourself

To log every frame on the bus, add this to the `uart:` block:

```yaml
  debug:
    direction: BOTH
    after:
      delimiter: [0xF4, 0xFB]
    sequence:
      - lambda: UARTDebug::log_hex(direction, bytes, ' ');
```

Lines marked `>>>` are frames the board sent, and `<<<` are the AC's replies.

## Not tested

- Changing the setpoint in SMART. The remote's TEMP buttons nudge the AC's own target by up to
  2 °C there, so the firmware refuses setpoint changes in SMART for now.
- Changing the fan speed in heat or dry. The firmware only allows it in cool and fan only, the
  same rule as the Home Assistant integration.
- Fahrenheit. The firmware only sends Celsius.
- iFEEL. The remote measures the room itself, and nothing about it shows in the AC's status.
