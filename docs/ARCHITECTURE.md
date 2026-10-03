# Architecture

Pico 2 W uses CYW43439 Bluetooth Classic, BTstack PANU, the BNEP lwIP adapter, DHCP, DNS and TCP. The Android phone provides the Bluetooth NAP tethering service.

1. First pairing: phone initiates SSP Just Works. The SDK stores the link key in flash TLV.
2. On reboot: `restore_bonded_phone()` picks the first saved Bluetooth Classic bond and initiates SDP discovery.
3. Discover the phone's NAP service, then connect from PANU to NAP using BNEP.
4. Start lwIP DHCP client after a diagnostic two-second delay; perform DNS and HTTP GET to example.com.
5. Connect to rotate.aprs2.net:14580 with pass -1, filter for the Japan region, and stream read-only APRS lines.
6. Log connection state and counts over USB serial; retry completed failures.

## Limitations

SDK 2.3.1 is the tested version. Other phones, power use and long-term reliability have not been characterized. Only the first stored Classic bond is automatically selected. A stuck asynchronous SDP request may still need a reset. TCP watchdog thresholds are 30 seconds warning and 60 seconds reconnect, based on any incoming TCP data, not only aprsc comment lines. The diagnostic DHCP startup delay is retained to preserve the tested firmware path. No APRS frames are transmitted.
