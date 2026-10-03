# Building and publishing a UF2

1. Use Pico SDK **2.3.1** with submodules; board **pico2_w**.
2. In VS Code with the Raspberry Pi Pico extension, compile the project, or use the workflow **Actions → Build Pico 2 W PANU + APRS-IS UF2**.
3. Confirm the Actions job succeeds and download the UF2 artifact. The workflow artifact is not automatically a GitHub Release attachment.
4. Flash the UF2 on a Pico 2 W; ensure PAN, DHCP, HTTP and APRS-IS work with a phone that has Bluetooth tethering enabled.
5. Manually create a GitHub Release and attach that verified UF2. State the exact build source/commit and test hardware.

This project's source is MIT-licensed: Copyright (c) 2026 Daisuke JA1UMW / CQAKIBA.TOKYO. Review upstream Pico SDK, BTstack and lwIP licenses when distributing binaries. Do not publicly upload private device identifiers or third-party APRS traffic unless intentionally disclosed.
