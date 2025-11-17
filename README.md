# Thermostat

## Description
Thermostat for ordinary residential central heating and cooling systems based on Raspberry Pi Pico2 W. 
- Provides a web inteface for configuration.
- Supports monitoring Tesla Powerwall 2 to control power consumption during grid failure.

## Installation on Ubuntu Linux
```
sudo apt install git build-essential cmake gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib
git clone --recurse-submodules https://github.com/NewmanIsTheStar/thermostat.git 
cd thermostat
mkdir build
cd build
cmake ..
make
```
Upon completion of a successful build the file thermostat.uf2 should be created.  This may be loaded onto the Pico2 W in the usual manner.  **NB:** The default board target is Pico2_W.

## Initial Configuration
- The Pico W will initially create a WiFi network called **pluto**.  Connect to this WiFi network and then point your web browser to http://192.168.4.1
  - Note that many web browsers automatically change the URL from http:// to https:// so if it is not connecting you might need to reenter the URL.
- Set the WiFi country, network and password then hit save and reboot.  The Pico will attempt to connect to the WiFi network.  If it fails then it will fall back to AP mode and you can once again connect to the pluto network and correct your mistakes.  

## Licenses
- SPDX-License-Identifier: BSD-3-Clause
- SPDX-License-Identifier: MIT 
