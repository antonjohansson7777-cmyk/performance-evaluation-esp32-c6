# Wi-Fi Configuration

The Wi-Fi 4 and Wi-Fi 6 implementations use the same source code.

To enable Wi-Fi 6 (802.11ax), include the following line in the protocol configuration:

```c
WIFI_PROTOCOL_11AX

To disable Wi-Fi 6 and run Wi-Fi 4, remove or comment out the same line