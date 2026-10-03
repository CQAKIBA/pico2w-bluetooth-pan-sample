# Changelog

## 1.0.0-demo (2026-10-04) — publication packaging

- Published **the previously hardware-verified v0.8 `main.c` without functional changes** (only a copyright header comment added).
- Replaced cumulative legacy README with English and Japanese quick-start guides.
- Added architecture notes, Android tethering troubleshooting, observed test report, source license, and GitHub-ready packaging.
- Retained real-device-proven 2-second DHCP diagnostic delay and all v0.8 recovery instrumentation.
- Included source CI workflow (not executed by this packaging step).
- Added `Copyright (c) 2026 Daisuke JA1UMW / CQAKIBA.TOKYO` as MIT copyright owner; expanded English/Japanese READMEs to explain the phone-side power advantage and Wi-Fi trade-offs.

## Earlier development checkpoints

- **v0.8** — boot-time restore of flash-backed Classic pairing; Pico-only reboot test passed.
- **v0.7** — BNEP/DHCP delay diagnostics; showed that missing Android tethering could cause early peer disconnect even before DHCP.
- **v0.6** — Bluetooth ACL status / retry instrumentation.
- **v0.5** — APRS 30s warning / 60s inactivity watchdog.
- **v0.4** — continuous APRS receive and retries.
- **initial** — smartphone PANU + DHCP + DNS + HTTP 200 proof of concept.