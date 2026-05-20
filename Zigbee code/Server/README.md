# Zigbee Configuration

The same throughput implementation was used for both baseline and load measurements.

To disable additional network load (baseline testing), set:

```c
#define LOAD_INTERVAL_MS 0

To enable network load, set:

#define LOAD_INTERVAL_MS 50