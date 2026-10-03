# Raspberry Pi Pico 2 W — Bluetooth PANU Internet tethering

**A working Bluetooth Classic PAN client for the Raspberry Pi Pico 2 W.**

Use an Android phone's **Bluetooth tethering** to give a Pico 2 W an ordinary IPv4 Internet connection—**no Wi-Fi hotspot, SIM card, or phone companion app** required. On top of PANU + DHCP, the included firmware checks `example.com` and receives live **APRS-IS amateur-radio packets** over TCP as an optional-but-enabled demonstration.

> **Hardware tested:** Pico 2 W with Raspberry Pi Pico SDK **2.3.1**; Bluetooth tethering from a **Blackview BV9300 (Android)**. The verified test covers pairing, link-key restoration after Pico reset, SDP/NAP discovery, BNEP/PANU, DHCP, DNS, HTTP 200, APRS-IS login/receive, and recovery after phone Bluetooth is re-enabled **with tethering enabled again**. Other phones and power consumption have **not** been characterized.
>
> This is an independent community example, **not an official Raspberry Pi project**. Demonstrated on actual hardware on **2026-10-03–04**.

[日本語の説明 / Japanese README](README.ja.md)

## How it works

```text
              Bluetooth Classic PAN (BNEP)
 Pico 2 W   <-------------------------------->  Android phone
  PANU       SDP -> NAP, DHCP client           Bluetooth NAP
    |                                              |
    +--------- IPv4 / TCP through lwIP ------------+---- Mobile Internet
                                                       |
                                                  example.com (HTTP)
                                                  rotate.aprs2.net:14580
                                                  (APRS-IS read-only)
```

The firmware uses **Bluetooth Classic**, not BLE. It does **not** connect to a Wi-Fi access point, create one, or require an Android application.

## Why Bluetooth tethering instead of a Wi-Fi hotspot?

**Lower battery drain on the smartphone is the main reason to use Bluetooth PAN for mobile IoT.** Bluetooth tethering generally uses less phone battery than Wi-Fi hotspot sharing, especially for the small, occasional data transfers typical of sensors and portable devices. Both [Sony support](https://www.sony.com/electronics/support/articles/SX566701) and [Android's tethering guide](https://www.android.com/intl/en_in/articles/tethering-hotspotting/) describe Bluetooth as a lower-power tethering option.

The trade-off is lower throughput and more awkward connection management compared with Wi-Fi. In particular, some Android devices do not automatically restore Bluetooth tethering after their Bluetooth radio has been turned off. **This firmware initiates PAN reconnects itself** and can restore the saved bond after a Pico reset, but it cannot turn on tethering in the phone's settings.

**Power-use caveat:** No comparative current measurements have been made for this particular Pico 2 W/BV9300 combination. Phone-side battery savings are an expected advantage, not a measured percentage or a guarantee for every device. Bluetooth Classic PAN is **not BLE**, and the Pico's own power consumption also needs measurement.

## Quick start (Windows)

1. Install **Visual Studio Code** and the [Raspberry Pi Pico extension](https://marketplace.visualstudio.com/items?itemName=raspberry-pi.raspberry-pi-pico).
2. Open this folder as a Pico SDK CMake project. Set the board to **`pico2_w`** and the SDK to **2.3.1**.
3. **Compile Project**. The expected artifact is `build/pico2w_bt_pan_tether.uf2` (the folder can vary depending on the extension).
4. Hold **BOOTSEL**, connect the Pico over USB, and copy the `.uf2` to its USB mass-storage drive. Open its **USB CDC serial port** at any baud-rate setting supported by your terminal (USB CDC baud is virtual).
5. On the Android phone, enable **Bluetooth** and **Bluetooth tethering** (may be under *Hotspot & tethering*). Pair with **`Pico2W-PANU`**. Approve the phone's **Internet access** permission for this device if asked. No PIN entry is needed with the current Just Works demo.
6. Watch the serial output: `Saved Classic bond` (on subsequent resets) → `BNEP connected` → `DHCP Lease acquired` → `INTERNET PASS` → `APRS-IS PASS`.

The firmware retries failed completed connection attempts automatically. On the **first** run, pair once from the phone. On **later Pico-only resets**, the firmware reads the saved Bluetooth Classic link-key database and automatically attempts to connect to the previously bonded phone.

**Phone-specific gotcha:** On the tested BV9300, turning Bluetooth **OFF→ON** also turns **Bluetooth tethering OFF**. Turn tethering **ON again**. Otherwise SDP and even a briefly opened BNEP channel may succeed before the phone closes it in milliseconds. This behavior is not proof of a Pico/BTstack failure.

### Command-line build (SDK toolchain already installed)

```sh
cmake -S . -B build -G Ninja -DPICO_BOARD=pico2_w -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
```

You need `PICO_SDK_PATH` pointing at SDK 2.3.1 with submodules, plus a suitable ARM toolchain and Ninja. A GitHub Actions workflow builds UF2 and ELF in the cloud after the source is pushed to your own repository. **That workflow has not yet been executed for this repackaged tree**; see its run results before releasing a binary.

## Serial status and controls

```text
[BOOT] Saved Classic bond #1: 02:AB:CD:EF:01:23 (type=7)
[BOOT] Auto-reconnecting bonded phone ...
[PAN] BNEP connected: src=1115 dst=1116
[DHCP] Lease acquired.
[RESULT] INTERNET PASS (DNS + TCP + HTTP response)
[APRS] # logresp P123456 unverified, server EXAMPLE
[APRS RX 1] <live APRS-IS packet>
[RESULT] APRS-IS PASS (receiving live packets continuously)
```

*This snippet is illustrative and anonymized; it is not a verbatim device log.*

Serial commands (send a line terminated by Enter): `s` = current status/statistics; `t` = rerun HTTP then APRS; `a` = restart APRS after HTTP PASS; `r` = request a PAN retry if offline; `x` = intentionally disconnect the phone ACL; `c XX:XX:XX:XX:XX:XX` = manually select a previously paired phone.

APRS-IS reception is **continuous** rather than stopping after a fixed number of packets. The firmware prints every packet by default; set `APRS_PRINT_PACKETS` to `0` in `main.c` for quieter power/throughput testing. It logs a warning after **30 seconds without application TCP data**, and attempts to reconnect after **60 seconds**; this is a diagnostic heuristic, **not** a server-wide guaranteed heartbeat interval.

## Amateur radio cameo: APRS-IS

After the HTTP check passes, the Pico connects to `rotate.aprs2.net:14580` and logs in with `pass -1` (unverified **receive-only**). The login user is generated as `P` + the final **three bytes of the Pico's Bluetooth MAC**, formatted as six hexadecimal characters, e.g. `P123456`. This avoids asking each experimenter to configure a callsign, but a MAC fragment **is not globally guaranteed unique**, and server policies may change.

The example applies an **approximate Japan-area geographic filter** (including Okinawa and remote islands). Rectangular filters can also include packets from neighboring regions. The code sends **no APRS beacons, messages, or RF transmissions**. Watching APRS-IS data over the Internet does not turn the Pico into a radio transmitter. [Official APRS-IS connection specification](https://www.aprs-is.net/Connecting.aspx) · [Filter reference](https://www.aprs-is.net/javAPRSFilter.aspx).

This is a **plain TCP** demo, not TLS; it is unsuitable for sensitive data. Please avoid creating unnecessary high-bandwidth/duplicate APRS-IS connections.

## What is included

- `main.c` — Bluetooth PANU client + DHCP, DNS, HTTP probe, APRS-IS stream, reconnect, and USB serial diagnostics.
- `btstack_config.h` / `lwipopts.h` — SDK 2.3.1 stack configuration.
- `CMakeLists.txt` — Pico 2 W firmware build.
- `.github/workflows/build.yml` — optional automated source build producing UF2/ELF artifacts.
- `tests/test_autobond.c` — host-only regression tests for the bond-selection routine (not a hardware emulation).
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — stages, retry policy, and limitations.
- [`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md) — symptoms and the tested Android tethering pitfall.
- [`docs/TEST_REPORT.md`](docs/TEST_REPORT.md) — what was actually validated on hardware.
- [`docs/RELEASING.md`](docs/RELEASING.md) — how to attach a tested UF2 as a GitHub Release asset.

## Limitations and licensing

This is a **known-good demonstration with one tested phone**, not a production-reviewed network stack. The saved-bond selection chooses the first Classic link key if more than one is stored. It may not recover from a permanently hung SDP request or lower-level radio failure without reset. Phone-specific PAN support, sleep behavior, firmware update effects on stored keys, RF power consumption, and other OSes remain untested. The code deliberately retains a **2-second delay before DHCP** from the version verified on hardware.

The sample application code and documentation are provided under the **MIT License**; see [LICENSE](LICENSE). **Copyright (c) 2026 Daisuke JA1UMW / CQAKIBA.TOKYO**. Preserve the copyright and license notice when redistributing. The Pico SDK, BTstack, and lwIP are separate upstream projects under **their own respective licenses**; this repository does not redistribute them. Review their terms when redistributing a compiled firmware image.

## Upstream references

- [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk)
- [Official pico-examples](https://github.com/raspberrypi/pico-examples) (contains `pan_lwip_http_server`, which serves the opposite NAP/server role)
- [BTstack](https://github.com/bluekitchen/btstack)
- [APRS-IS client connection specification](https://www.aprs-is.net/Connecting.aspx)
