# Linux ASUS WMI Sensor driver.

## General info

Provides a Linux kernel module "asus_wmi_sensors" that provides sensor readouts via ASUS' WMI interface present in the UEFI of some recent Ryzen motherboards.

## Features
- Reports all values scaled and named identically to in the UEFI interface
- No sensor config required
- 
## Supported hardware

|Board                              | Minimum BIOS Version |
|-----------------------------------|----------------------|
|Asus ROG Crosshair Hero VII (WiFi) | 1002                 |
|Asus ROG Crosshair Hero VII        | 1002                 |
|Asus ROG Crosshair Hero VI         | 6301                 |
|Asus ROG Crosshair Hero VI (WiFi)  | 6302                 |
|ASUS Zenith Extreme                | 1607/1701            |
|Prime X470-Pro                     | 4602                 |

## Currently don't work / unknown

|Board                              |
|-----------------------------------|
|ASUS ROG STRIX B450/X470 (e.g. Strix B450-F)|
|Prime B450-Plus|
|ASUS ROG CROSSHAIR VI EXTREME|

## How to install

Ensure you have lm_sensors, DKMS, kernel sources, GCC etc installed.

### Arch Linux
Available as an AUR package - https://aur.archlinux.org/packages/asus-wmi-sensors-dkms-git/

This hooks into DKMS to build a module for your available kernels and adds a ```/etc/module-load.d/``` entry so the module is loaded at boot.

The module can be manually loaded by issuing ```sudo modprobe asus_wmi_sensors```

Run ```sensors``` and you should see a ```asuswmisensors-isa-0000``` device and readouts as you see in the UEFI interface.

### Other distributions

Clone the git repo: ```git clone https://github.com/electrified/asus-wmi-sensors.git```

Build the module ```sudo make dkms```

Insert the module ```sudo modprobe asus-wmi-sensors```

Run ```sensors``` and you should see a ```asuswmisensors-isa-0000``` device and readouts as you see in the UEFI interface.

Optional - consult your distro's documentation for info on how to make the module be loaded automatically at boot

## FAQ

### I think my motherboard should be supported but it doesn't work, what can I do?
First verify that WMI hardware monitoring is working for your board under Windows. Both HWiNFO (https://www.hwinfo.com/) and SIV(http://rh-software.com/) will make use of ASUS WMI for reading sensors if available. If your board is supported by those, post the output of ```sudo dmidecode -t baseboard``` and it should be possible to add support.

### Why do some of my temperatures return 216 deg C?
This is the value returned for temperature sensor headers with no sensor connected.

### Why is reading from the sensors so slow?
This driver is not reading from the SuperIO/ Embedded controller directly, it uses a WMI interface put in the UEFI firmware by ASUS. Reading from this WMI interface seems inherently slow. I am investigating calling the underlying ACPI methods that the WMI interface calls which I have been told performs better.

### Why does this driver exist?
Many of Asus' recent Ryzen motherboards have the ITE IT8665E sensor IC, which does not have any publically available datasheets. Some support has been added to the out-of-tree IT87 driver, but this is currently unmaintained and not working on recent kernels. Also many Windows drivers are moving to use this WMI interface rather than accessing the chip directly as this avoids conflicts when multiple monitoring apps attempt to read the sensors simultaneously.

### Why have you created a new driver and not added to the existing Asus/eeepc drivers?
- The existing drivers are basic platform devices rather than using the kernels' WMI bus
- These new sensor methods are on a different WMI class - "ASUSHW" than the existing "ASUSManagment" class which the other driver uses. The existing driver largely deals with laptop functionality (hotkeys, WiFi kill switches, screen brightness). Adding to that driver support for this additional sensors functionality would make it quite large.

## Example sensors output

```
asuswmisensors-isa-0000
Adapter: Virtual device
CPU Core Voltage:         +0.88 V  
CPU SOC Voltage:          +1.13 V  
DRAM Voltage:             +1.34 V  
VDDP Voltage:             +0.24 V  
1.8V PLL Voltage:         +1.85 V  
+12V Voltage:            +11.88 V  
+5V Voltage:              +5.01 V  
3VSB Voltage:             +3.33 V  
VBAT Voltage:             +3.18 V  
AVCC3 Voltage:            +3.36 V  
SB 1.05V Voltage:         +1.07 V  
CPU Core Voltage:         +0.81 V  
CPU SOC Voltage:          +1.14 V  
DRAM Voltage:             +1.35 V  
CPU Fan:                  749 RPM
Chassis Fan 1:              0 RPM
Chassis Fan 2:            904 RPM
Chassis Fan 3:            888 RPM
HAMP Fan:                   0 RPM
Water Pump:                 0 RPM
CPU OPT:                    0 RPM
Water Flow:                 0 RPM
AIO Pump:                   0 RPM
CPU Temperature:          +37.0°C  
CPU Socket Temperature:   +31.0°C  
Motherboard Temperature:  +28.0°C  
Chipset Temperature:      +45.0°C  
Tsensor 1 Temperature:   +216.0°C  
CPU VRM Temperature:      +31.0°C  
Water In:                +216.0°C  
Water Out:                +28.0°C  
CPU VRM Output Current:   +1.00 A 
```

## Thanks
- Ray Hinchcliffe, author of SIV for info
- Original authors of the IT87 makefile
- Authors of other mainlined HWMON kernel modules that I've studied while writing this