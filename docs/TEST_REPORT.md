# Hardware verification — 2026-10-03 to 2026-10-04

Tested with **Raspberry Pi Pico 2 W**, **Pico SDK 2.3.1**, and **Blackview BV9300 Android** Bluetooth tethering.

| Test | Result |
|---|---|
| VS Code Pico SDK build | PASS |
| Bluetooth Classic pairing / flash-backed link key | PASS |
| SDP NAP discovery, BNEP PANU connection | PASS |
| DHCP IPv4 allocation | PASS |
| DNS, TCP and example.com HTTP/1.1 200 OK | PASS |
| APRS-IS login (pass -1), real receive-only stream | PASS |
| Recover from phone BT OFF/ON after re-enabling tethering | PASS |
| Reconnect after Pico-only reset, without pairing again | PASS |
| Multi-phone interoperability / battery current / extended stability | NOT TESTED |

The phone disables its Bluetooth tethering toggle after Bluetooth OFF→ON. This explains short-lived BNEP links observed before DHCP. Once tethering was manually restored, the Pico recovered automatically, including an occasional transient retry.

Actual device identifiers and third-party APRS packets are omitted from this public report. The 2-second DHCP diagnostic delay remains in the tested v0.8 implementation.
