# esp-802154-repeater

A small **Zigbee router** or **Thread router** firmware for the ESP32-C6. It is
meant to give otherwise useless ESP32-C6 boards a second life as mesh range
extenders instead of ending up as e-waste.

It was written for a batch of boards that were meant to be low-power UART debuggers
(ESP32-C6-MINI-1-N4: 4 MB flash, no PSRAM) but have an unusable SD card slot. It
should run on any ESP32-C6 board, since it uses only the 802.15.4 radio and one
GPIO.

- Written in plain C, on ESP-IDF v6.1
- One protocol per firmware image, chosen at build time
- One jumper (GPIO2 to GND during boot) wipes the stored network and starts pairing
- No allocations in the application code after start-up. The stacks allocate their
  memory once when they initialise.

## Contents

- [How it behaves](#how-it-behaves)
- [Building](#building)
- [Flashing](#flashing)
- [Pairing with Zigbee](#pairing-with-zigbee)
- [Pairing with Thread](#pairing-with-thread)
- [Configuration](#configuration)
- [Project layout](#project-layout)
- [Design notes](#design-notes)

## How it behaves

```
power on
   │
   ▼
sample GPIO2 for 3 s (internal pull-up)
   │
   ├── held low ≥ 1 s without interruption ──► RESET-AND-PAIR
   │                                              forget the stored network,
   │                                              try to join for 3 minutes
   │                                                 ├─ joined ─► ROUTER (forever)
   │                                                 └─ timeout ─► idle until reboot
   │
   └── not held (or only glitches) ──► network stored?
                                          ├─ yes ─► ROUTER (rejoin, forever)
                                          └─ no  ─► PAIRING (retry until joined) ─► ROUTER
```

> **You pair once.** Network credentials are saved to flash when the device
> joins, so it reconnects by itself after every reboot or power cut. You only pair
> it again if you want to move it to another network (GPIO2 reset).

- **Stored network.** Once the device has joined a network, that network's
  credentials are kept in flash (NVS). For Zigbee these are the network key, PAN and
  so on; for Thread it is the operational dataset. On every later boot the device
  rejoins that network and routes until it loses power. Pairing is not needed again.
- **No stored network** means one of two things. Either the board is fresh from
  the factory and has never joined a network, or a GPIO2 reset has just wiped it, or
  the Zigbee coordinator removed the device from its network. In this state the
  device keeps trying to join until it succeeds.
- **The GPIO2 reset is deliberate.** GPIO2 has to be held low for at least 1 s
  without a break, inside the first 3 s after boot. Shorter pulses are ignored. The
  device then has 3 minutes to join. If it doesn't, it stops trying and stays idle
  until the next reboot. After that reboot it has no stored network, so it keeps
  trying to join again.
- **Jumper left in place.** After the 3 s window the pull-up on GPIO2 is turned
  off, so a jumper left in place draws no current. It does trigger another reset
  on the next boot, though.

## Building

Install [ESP-IDF v6.1](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c6/get-started/)
and run `. $IDF_PATH/export.sh`.
The Zigbee library (`espressif/esp-zigbee-lib` 2.0.x) is downloaded automatically
by the IDF component manager.

### Option A: separate build directories (recommended)

You can keep both variants side by side:

```bash
idf.py -B build-zigbee -DSDKCONFIG=build-zigbee/sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.zigbee" build
```

```bash
idf.py -B build-thread -DSDKCONFIG=build-thread/sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.thread" build
```

### Option B: menuconfig

```bash
idf.py menuconfig
```

Go to **802.15.4 Repeater → Mesh protocol**, pick *Zigbee router* or *Thread
router*, then run `idf.py build`. Zigbee is the default.

> **Switching protocol in an existing build directory:** the protocol choice
> *enables* the matching stack, but it can't turn the other one off for you.
> If the build stops with "Zigbee and OpenThread cannot be linked into one
> image", disable the unused stack under *Component config* (`Zigbee Enable` or
> `OpenThread`), or simply delete `sdkconfig` and choose again.

### CI builds

Every push is built by GitHub Actions (`.github/workflows/build.yml`). The workflow
artifacts include a merged `repeater-<protocol>-full.bin` that can be flashed at
offset `0x0`.

## Flashing

```bash
idf.py -B build-zigbee -p /dev/ttyACM0 flash monitor
```

Or flash a merged image from CI:

```bash
python -m esptool --chip esp32c6 -p /dev/ttyACM0 write-flash 0x0 repeater-zigbee-full.bin
```

When you flash a different protocol over an existing install, the old protocol's
credentials stay in NVS but are never used. To start completely clean, run
`idf.py erase-flash` first.

Partition layout (`partitions.csv`, 4 MB flash):

| Name     | Type      | Offset   | Size    | Purpose                                         |
|----------|-----------|----------|---------|-------------------------------------------------|
| nvs      | data/nvs  | 0x9000   | 88 KB   | Network credentials (Zigbee and OpenThread)     |
| phy_init | data/phy  | 0x1F000  | 4 KB    | RF calibration                                  |
| factory  | app       | 0x20000  | 1.875 MB| Firmware (~0.5 MB Zigbee, ~0.7 MB Thread)       |
| zb_fct   | data/fat  | 0x200000 | 4 KB    | Zigbee factory data, reserved by esp-zigbee-lib |

## Pairing with Zigbee

1. Turn on *Permit join* on your coordinator. In Zigbee2MQTT, click *Permit join
   (All)*; in Home Assistant ZHA, use *Add device*.
2. Power the repeater. If it has never joined a network, it starts searching right
   away. To move it to a new network, hold GPIO2 to GND while powering it up (for
   about 1 s) and release.
3. It joins as a **router** with one Home Automation endpoint (endpoint 1, device ID
   `0x0008` *Range Extender*) that has the *Basic* and *Identify* clusters.
   ManufacturerName is `DIY` and ModelIdentifier is `C6-802154-Repeater`; both can be
   changed in menuconfig. Zigbee2MQTT will list it as an unsupported device, which is
   fine: it routes all the same, and you can see it in the network map.

When a GPIO reset is used, the device sends a proper Zigbee *Leave* to its old
network before it searches for a new one.

## Pairing with Thread

A Thread device joins through a **commissioner**, usually your OpenThread Border
Router (OTBR), using a pre-shared pairing code called the *PSKd*. The default PSKd is
`J01NU5`. You can change it in menuconfig; it must be 6–32 characters, uppercase
letters and digits only, without `I`, `O`, `Q` or `Z`.

1. On the border router, start a commissioner and allow the repeater to join:

   ```bash
   ot-ctl commissioner start
   ```

   ```bash
   ot-ctl commissioner joiner add '*' J01NU5
   ```

   Using `'*'` accepts any joiner that knows the PSKd. To allow only this board,
   replace `'*'` with its EUI-64, which the firmware prints at boot:
   `Joiner EUI-64 xxxxxxxxxxxxxxxx, PSKd J01NU5`.

   With the Home Assistant OTBR add-on, run these commands in the add-on's terminal
   (`docker exec -it addon_core_openthread_border_router ot-ctl ...`).

2. Power the repeater. If it has no stored network, or after a GPIO2 reset, it
   runs the Joiner and retries every 5 s until it succeeds (or until the 3 minute
   window closes, after a GPIO reset).
3. Once commissioned, it attaches as a child, and then becomes a router whenever
   the network needs one. Watch the `Thread role:` log lines.

The device is a Full Thread Device (FTD) and can become a router. Thread decides
by itself how many routers it needs; up to 32 are allowed. A router-eligible device
that the network doesn't need yet stays a REED (router-eligible end device) and is
promoted automatically when it's needed. This is normal Thread behaviour.

## Configuration

Everything is in `idf.py menuconfig` → **802.15.4 Repeater**:

| Option                          | Default              | Meaning                                                  |
|---------------------------------|----------------------|----------------------------------------------------------|
| Mesh protocol                   | Zigbee router        | Which stack is built into the image                      |
| Reset-and-pair GPIO             | 2                    | Pin to short to GND at boot                              |
| Boot window to sample the GPIO  | 3000 ms              | How long after boot the pin is watched                   |
| Minimum continuous low time     | 1000 ms              | Glitch filter                                            |
| Pairing window after a GPIO reset | 180 s              | How long to try joining after a reset                    |
| Delay between join attempts     | 5000 ms              | Back-off between Zigbee steering / Thread Joiner attempts |
| Vendor / Model name             | `DIY` / `C6-802154-Repeater` | Zigbee Basic cluster strings, Thread Joiner vendor info |
| Zigbee → Channel mask           | `0x07FFF800` (11–26) | Channels scanned when joining                            |
| Zigbee → Max children           | 10                   | End devices that can use this router as their parent     |
| Thread → Joiner PSKd            | `J01NU5`             | Pairing code                                             |

## Project layout

```
main/
  app_main.c        NVS init, GPIO check, starts the selected back-end
  pair_gpio.c/.h    3 s boot window, ≥ 1 s hold detection on GPIO2
  repeater.h        back-end interface (+ guard against both stacks)
  zigbee_router.c   Zigbee router (esp-zigbee-lib 2.x)
  thread_router.c   Thread FTD + Joiner (ESP-IDF OpenThread)
  Kconfig.projbuild "802.15.4 Repeater" menu
sdkconfig.defaults          common settings (4 MB flash, size optimisation, mbedTLS for the Joiner)
sdkconfig.defaults.zigbee   selects Zigbee
sdkconfig.defaults.thread   selects Thread
partitions.csv
```

## Design notes

**Why one protocol per image?** The prebuilt Zigbee library and ESP-IDF's
OpenThread port both define the IEEE 802.15.4 driver callbacks
(`esp_ieee802154_receive_done()`, `esp_ieee802154_transmit_done()` and others). They
cannot be linked into the same binary without patching a vendor library.
Choosing the protocol at build time keeps the code and the build simple, and each
image only contains the stack it needs.

**Memory.** The boards have no PSRAM. Static RAM usage is roughly 73 KB for Zigbee
and 94 KB for Thread, out of about 450 KB, so the heap has plenty of room. The
application allocates only at start-up: two `esp_timer`s, the Zigbee
device/endpoint descriptors, and the stack tasks. Timer callbacks don't allocate.
On Zigbee they post to the stack task with `esp_zigbee_task_queue_post()`; on Thread
they take the OpenThread lock.

**Zigbee reset.** A GPIO reset uses the BDB *local reset*
(`ezb_bdb_reset_via_local_action()`) rather than erasing flash. This way the device
announces that it is leaving, and its outgoing NWK frame counter is kept, so the
other routers don't reject its frames if it rejoins the same network.

**Thread reset.** The OpenThread NVS namespace (`openthread`) is erased before the
stack starts, which is the same as `otInstanceFactoryReset()` but without the
extra reboot.

**No IP stack on Thread.** `CONFIG_OPENTHREAD_PLATFORM_NETIF` is off, so neither
lwIP nor esp_netif is started. The device only forwards Thread frames. The code
still works if you turn the option back on.

**Not done (on purpose, to keep it simple):** status LED, OTA updates, a console.
