# esp-802154-repeater

A small **Zigbee router** or **Thread router** firmware for the ESP32-C6. It is
meant to give otherwise useless ESP32-C6 boards a second life as mesh range
extenders instead of ending up as e-waste.

It was written for a batch of boards that were meant to be low-power UART debuggers
(ESP32-C6-MINI-1-N4: 4 MB flash, no PSRAM) but have an unusable SD card slot. It
should run on any ESP32-C6 board, since it uses only the radio and two GPIOs.

- Written in plain C, on ESP-IDF v6.1
- One protocol per firmware image, chosen at build time
- One jumper (GPIO2 to GND during boot) wipes the stored network and starts pairing
- Another jumper (GPIO3 to GND during boot) starts a Wi-Fi access point with a web
  page for firmware updates
- No allocations in the application code after start-up. The stacks allocate their
  memory once when they initialise.

## Contents

- [How it behaves](#how-it-behaves)
- [Building](#building)
- [Flashing](#flashing)
- [Pairing with Zigbee](#pairing-with-zigbee)
- [Pairing with Thread](#pairing-with-thread)
- [Firmware update over Wi-Fi](#firmware-update-over-wi-fi)
- [Configuration](#configuration)
- [Project layout](#project-layout)
- [Design notes](#design-notes)

## How it behaves

```
power on
   │
   ▼
sample GPIO2 and GPIO3 for 3 s (internal pull-ups)
   │
   ├── GPIO3 held low ≥ 1 s ──► UPDATE PORTAL (Wi-Fi AP + web page, no mesh)
   │                               ├─ image uploaded ─► reboot into new firmware
   │                               └─ 10 min idle    ─► reboot
   │
   ├── GPIO2 held low ≥ 1 s ──► RESET-AND-PAIR
   │                                              forget the stored network,
   │                                              try to join for 3 minutes
   │                                                 ├─ joined ─► ROUTER (forever)
   │                                                 └─ timeout ─► idle until reboot
   │
   └── neither held, only glitches, or BOTH held ──► network stored?
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
- **Both jumpers at once** is treated as a mistake: both are ignored and the device
  starts normally.
- **Jumper left in place.** After the 3 s window the pull-ups are turned off, so a
  jumper left in place draws no current. It does trigger the same action again on
  the next boot, though.

## Building

Install [ESP-IDF v6.1](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c6/get-started/)
and run `. $IDF_PATH/export.sh`.
The Zigbee library (`espressif/esp-zigbee-lib` 2.0.x) is downloaded automatically
by the IDF component manager.

### Option A: build script (recommended)

```bash
scripts/build.sh
```

This builds both images; add `zigbee` or `thread` to build only one. Each protocol
gets its own build directory (`build-zigbee/`, `build-thread/`), and the flashable
images are collected in `dist/`:

| File                                                | Use                                                          |
|-----------------------------------------------------|--------------------------------------------------------------|
| `esp-802154-repeater-<protocol>-<version>-full.bin` | USB flashing at `0x0`: bootloader, partition table, app (4 MB, mostly padding) |
| `esp-802154-repeater-<protocol>-<version>-app.bin`  | Upload in the [Wi-Fi update portal](#firmware-update-over-wi-fi) |
| `SHA256SUMS`                                        | Checksums                                                    |

The script just runs `idf.py` with a separate `sdkconfig` per protocol, so you can
do the same by hand:

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

GitHub Actions (`.github/workflows/build.yml`) runs `scripts/build.sh` for both
protocols in the `espressif/idf:v6.1` container. This happens on every push to
`master` and on every pull request. The images are attached to the workflow run as
`firmware-zigbee` and `firmware-thread` artifacts.

Pushing a tag such as `v1.0.0` also creates a GitHub release with all images and
their checksums:

```bash
git tag v1.0.0
```

```bash
git push origin v1.0.0
```

## Flashing

```bash
idf.py -B build-zigbee -p /dev/ttyACM0 flash monitor
```

Or flash a merged image from `dist/`, a CI run or a release:

```bash
python -m esptool --chip esp32c6 -p /dev/ttyACM0 write-flash 0x0 esp-802154-repeater-zigbee-<version>-full.bin
```

After the first USB flash, later updates can go over Wi-Fi (see
[Firmware update over Wi-Fi](#firmware-update-over-wi-fi)).

The full image also resets `otadata`, so the board boots from `ota_0` again even if
an over-the-air update had switched it to `ota_1`. Flash the full image, not only the
app, when you use USB.

When you flash a different protocol over an existing install, the old protocol's
credentials stay in NVS but are never used. To start completely clean, run
`idf.py erase-flash` first.

Partition layout (`partitions.csv`, 4 MB flash):

| Name     | Type      | Offset   | Size     | Purpose                                         |
|----------|-----------|----------|----------|-------------------------------------------------|
| nvs      | data/nvs  | 0x9000   | 88 KB    | Network credentials (Zigbee and OpenThread)     |
| phy_init | data/phy  | 0x1F000  | 4 KB     | RF calibration                                  |
| ota_0    | app       | 0x20000  | 1.875 MB | Firmware slot A                                 |
| ota_1    | app       | 0x200000 | 1.875 MB | Firmware slot B                                 |
| otadata  | data/ota  | 0x3E0000 | 8 KB     | Which slot to boot                              |
| zb_fct   | data/fat  | 0x3E2000 | 4 KB     | Zigbee factory data, reserved by esp-zigbee-lib |

The images are about 1.1 MB (Zigbee) and 1.25 MB (Thread), so each fits in one slot
with room to spare.

> **Coming from the first single-app version?** The partition table changed, so flash
> the new full image over USB once. `nvs` and the first app slot have the same offsets
> as before, so the board keeps its stored network.

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

## Firmware update over Wi-Fi

1. Connect GPIO3 to GND and power the board (keep it connected for at least 1 s
   within the first 3 s). The mesh stack does **not** start in this mode.
2. Join the Wi-Fi network **`C6-Repeater-XXXX`**. `XXXX` is the end of the board's
   MAC address, and the default password is `repeater`.
3. Open **http://192.168.4.1/**, choose the `…-app.bin` file (from `dist/`, a CI run
   or a release) and press *Upload and install*.
4. **Remove the GPIO3 jumper** while it uploads. When the upload finishes, the device
   reboots into the new firmware. If the jumper is still connected, it starts the
   portal again. In that case, remove the jumper and power-cycle the board.

Safety checks:

- **Before anything is written,** the first 4 KB are checked: is it an ESP-IDF app
  image, built for the ESP32-C6, from this project? A merged `-full.bin`, a file for
  another chip or another firmware is rejected.
- **The new image goes into the slot that isn't running.** It is fully verified
  before it is selected for boot. A failed or interrupted upload leaves the current
  firmware untouched.
- **Rollback.** A new image that crashes before it starts its stack (or the portal)
  is rolled back to the previous one by the bootloader on the next reset.
- **Switching protocol.** Both Zigbee and Thread images are accepted, so you can
  switch protocol over Wi-Fi.
- **Idle timeout.** If nothing is uploaded, the portal reboots after 10 minutes.

## Configuration

Everything is in `idf.py menuconfig` → **802.15.4 Repeater**:

| Option                          | Default              | Meaning                                                  |
|---------------------------------|----------------------|----------------------------------------------------------|
| Mesh protocol                   | Zigbee router        | Which stack is built into the image                      |
| Reset-and-pair GPIO             | 2                    | Pin to short to GND at boot to re-pair                   |
| Firmware update portal GPIO     | 3                    | Pin to short to GND at boot for the Wi-Fi update portal  |
| Boot window to sample the GPIOs | 3000 ms              | How long after boot the pins are watched                 |
| Minimum continuous low time     | 1000 ms              | Glitch filter                                            |
| Pairing window after a GPIO reset | 180 s              | How long to try joining after a reset                    |
| Delay between join attempts     | 5000 ms              | Back-off between Zigbee steering / Thread Joiner attempts |
| Vendor / Model name             | `DIY` / `C6-802154-Repeater` | Zigbee Basic cluster strings, Thread Joiner vendor info |
| Zigbee → Channel mask           | `0x07FFF800` (11–26) | Channels scanned when joining                            |
| Zigbee → Max children           | 10                   | End devices that can use this router as their parent     |
| Thread → Joiner PSKd            | `J01NU5`             | Pairing code                                             |
| Portal → Access point name prefix | `C6-Repeater`      | SSID is `<prefix>-XXXX` (end of the MAC address)         |
| Portal → Access point password  | `repeater`           | WPA2, at least 8 characters; empty = open network        |
| Portal → Wi-Fi channel          | 6                    |                                                          |
| Portal → Reboot after idle      | 10 min               | 0 = keep the portal running                              |

## Project layout

```
main/
  app_main.c        NVS init, boot jumpers, starts the portal or the selected back-end
  boot_gpio.c/.h    3 s boot window, ≥ 1 s hold detection on GPIO2 / GPIO3
  ota_portal.c/.h   Wi-Fi AP + HTTP server for firmware uploads
  ota_portal.html   the upload page (embedded into the firmware)
  repeater.h        back-end interface (+ guard against both stacks)
  zigbee_router.c   Zigbee router (esp-zigbee-lib 2.x)
  thread_router.c   Thread FTD + Joiner (ESP-IDF OpenThread)
  Kconfig.projbuild "802.15.4 Repeater" menu
scripts/build.sh            builds both images into dist/ (used by CI)
.github/workflows/build.yml CI + releases on v* tags
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

**Memory.** The boards have no PSRAM. Static RAM usage is roughly 150 KB for Zigbee
and 175 KB for Thread, out of about 450 KB, so the heap has plenty of room. About
75 KB of that is the Wi-Fi driver, which is linked in but only started in portal
mode. The application allocates only at start-up: a few `esp_timer`s, the Zigbee
device/endpoint descriptors, and the stack tasks. Uploads go through one static
4 KB buffer. Timer callbacks don't allocate.
On Zigbee they post to the stack task with `esp_zigbee_task_queue_post()`; on Thread
they take the OpenThread lock.

**Zigbee reset.** A GPIO reset uses the BDB *local reset*
(`ezb_bdb_reset_via_local_action()`) rather than erasing flash. This way the device
announces that it is leaving, and its outgoing NWK frame counter is kept, so the
other routers don't reject its frames if it rejoins the same network.

**Thread reset.** The OpenThread NVS namespace (`openthread`) is erased before the
stack starts, which is the same as `otInstanceFactoryReset()` but without the
extra reboot.

**No IP stack on Thread.** `CONFIG_OPENTHREAD_PLATFORM_NETIF` is off, so OpenThread
is not attached to lwIP. The device only forwards Thread frames. lwIP is only used
by the Wi-Fi update portal. The code still works if you turn the option back on.

**Wi-Fi and 802.15.4 never run together.** Portal mode doesn't start the mesh stack,
so the radio doesn't have to share time between Wi-Fi and 802.15.4.

**Not done (on purpose, to keep it simple):** status LED, a console, updates over
Zigbee or Thread.
