# Troubleshooting

| Symptom | Check |
|---|---|
| No USB log | Pico 2 W board selection, USB serial port, flashed UF2 |
| No saved Classic bonds at reboot | Flash erase or changed storage layout; pair again |
| NAP not found | Enable phone Bluetooth and Bluetooth tethering |
| BNEP connected then closes in milliseconds | **Re-enable Bluetooth tethering** after toggling phone Bluetooth OFF/ON |
| DHCP lease missing | Smartphone's PAN connection permission / tethering state |
| HTTP/DNS timeout | Phone mobile data, DNS, transient network outage; retries are automatic |
| APRS silence | Internet connection, server availability, watchdog stats |
| Wrong saved peer selected | Use `c AA:BB:CC:DD:EE:FF` on USB serial |

The hardware-tested Blackview BV9300 disables Bluetooth tethering when Bluetooth is switched OFF. Turning Bluetooth back ON does not automatically restore tethering. Even with NAP advertised and a momentary BNEP connection, the phone can terminate the channel before DHCP starts. Re-enable tethering to recover.

HCI reason 0x13 indicates a remote-terminated ACL connection but does not, alone, identify the cause. The code also logs 0x16 on explicit local ACL reset.

Resetting only the Pico should reconnect without re-pairing when the Classic link key survives in flash.
