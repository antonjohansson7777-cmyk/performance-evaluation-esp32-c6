# OpenThread Configuration Steps

The ESP-IDF OpenThread CLI example was used for the Thread experiments.

## To be able to use CLI

idf.py menuconfig -> component config -> ESP system settings -> channel for console output -> Change to USB Serial/JTAG controller. S to save

## Device setup

Flash the OpenThread CLI example onto both ESP32-C6 devices.


## Leader configuration

On device A:

ot pollperiod 100
ot dataset channel 25
ot dataset init new
ot dataset commit active
ot ifconfig up
ot thread start

## Wait until device becomes leader

ot state

## Expected output

leader

## Get dataset to use these values for client node

ot dataset active


## Client Configuration

# Stop thread and ifconfig

ot thread stop
ot ifconfig down

## Create dataset by using same values as leader node dataset

ot dataset channel ...
ot dataset extpanid ...
ot dataset networkkey ...
ot dataset networkname ...
ot dataset panid ...
ot dataset meshlocalprefix ...

ot dataset pollperiod 100

# Activate dataset

ot dataset commit active

# Start interface and thread

ot ifconfig up
ot thread start

## Double-check connection

ot state

# Expected result

router or child

## Get ip address

ot ipaddr

## Ping for 2 minutes at 1 second interval with 8 bytes

ot ping <ip adress of target> 8 120 1

## Iperf server command with udp and ipv6 

iperf -s -V -u

## Iperf client command with udp and ipv6, packet length 1024, print interval at 1 and bandwidth at 100 kilobytes

iperf -u -V -c <ip address of target> -l 1024 -t 120 -i 1 -b 100k