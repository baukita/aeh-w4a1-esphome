# Hardware

The AEH-W4A1 Wi-Fi module plugs into a 4-pin socket on the AC's indoor unit and passes
frames between Wi-Fi and the AC's RS-485 bus without changing them, at 9600 baud. An ESP32
with an RS-485 transceiver can take its place on the same socket, powered by the socket's
own 5 V. The module itself can't run ESPHome: it pairs a separate microcontroller with a
Wi-Fi chip that isn't an ESP32.

## The socket

The socket has four pins: +5 V, the two RS-485 data lines (A+ and B−) and ground. On our
unit, looking at the module's plug with its latch facing you, the wires from left to right
are:

| Wire | Signal |
|---|---|
| Red | +5 V |
| White | A+ |
| Yellow | B− |
| Blue | GND |

Read from right to left, that's the pinout other projects publish (GND, B−, A+, +5 V).
Wire colours differ between units, so measure before connecting anything. With the module
unplugged and the AC switched on, +5 V and A+ both read about 4–5 V above GND and B−. That's
an idle RS-485 line, where A is pulled up and B is pulled down. While measuring, we once
powered the board the wrong way round for a moment. It survived, but don't rely on that.

We connected the board with male jumper wires pushed into the AC side of the connector. The
module's own plug looks like a JST XH (2.5 mm pin spacing) housing, but we didn't measure it.

## The board we used: LilyGO T-CAN485

The [LilyGO T-CAN485](https://github.com/Xinyuan-LilyGO/T-CAN485) is an ESP32 with a
MAX13487 RS-485 transceiver, a screw terminal for RS-485 and a screw terminal for 5–12 V
power. So it runs straight from the AC's 5 V, and the whole job is four wires:

| AC socket | T-CAN485 |
|---|---|
| +5 V | Power terminal + |
| GND | Power terminal − |
| A+ | RS-485 terminal RS_A |
| B− | RS-485 terminal RS_B |

The RS-485 terminal is printed `RS_A · GND · RS_B`. Its middle GND screw can stay empty,
because ground already comes through the power terminal. Check the printing on the power
terminal before wiring.

Four ESP32 pins matter, and `example.yaml` sets all four:

| Pin | Job | Setting |
|---|---|---|
| GPIO22 / GPIO21 | RS-485 transmit / receive | `uart: tx_pin: GPIO22, rx_pin: GPIO21, baud_rate: 9600` |
| GPIO17 | Transceiver enable: high while sending, low while listening | `flow_control_pin: GPIO17`. The component drives it; left high, the board can't hear replies. |
| GPIO16 | Turns on the transceiver's 5 V supply | On at boot, or the transceiver stays dead |
| GPIO19 | Transceiver SE pin | On at boot |

LilyGO's pin map doesn't list GPIO16, but LilyGO's GitHub page and a
[French write-up on forum.hacf.fr](https://forum.hacf.fr/t/retex-pilotage-complet-dun-variteur-piscine-varipool-isaver-en-rs485-avec-esphome-et-une-lilygo-t-can485/81668)
both use it to power the transceiver, and the board doesn't hear the AC without it.

Flash the board the first time over its USB-C port, before it goes into the AC. After that,
updates go over Wi-Fi. Don't plug USB in while the board is powered by the AC.

The board is 80 mm long, 107 mm including the screw terminals, and 35 mm wide, with the
terminals standing about 11 mm tall. If you print a box for it, use PETG rather than PLA,
since it sits inside the indoor unit.

## Other boards

We haven't tested these, but other AEH-W4A1 owners have reported them working:

- An ESP32 dev board with a 3.3 V RS-485 module that switches direction by itself, such as
  the XY-017 (also sold as HW-519). It only needs TX and RX, so leave out
  `flow_control_pin`. Run the module from the ESP32's 3.3 V so its output is safe for the
  ESP32.
- An ESP32 with the common 5 V MAX485 board (pins RO, RE, DE and DI). It needs a GPIO for
  DE and RE, wired together, as `flow_control_pin`. It also needs a level shifter between
  it and the ESP32, because its 5 V output can damage an ESP32 pin.

One report describes an ESP32-C3 SuperMini crashing until its framework was changed, so a
classic ESP32 is the safer choice.

## If the board keeps restarting

ESPHome puts a board into safe mode if it restarts about 10 times in a row, each time less
than 60 seconds after starting. Only Wi-Fi and updates run then, so Home Assistant shows
everything as unavailable. A new firmware flashed during safe mode starts straight in safe
mode. A firmware that never runs for 60 seconds is rolled back to the previous one once
safe mode ends, and the log then says `OTA rollback detected`. After a restart loop, wait
for safe mode to end and check the log for that message before drawing conclusions from a
test build.
