# Linux ASUS WMI Sensor driver

## General info

Provides a Linux kernel module "asus_wmi_sensors" that provides sensor readouts via ASUS' WMI interface present in the UEFI of some X370/X470/B450/X399 Ryzen motherboards.

## Features
- Reports all values scaled and named identically to in the UEFI interface
- No sensor configuration required

## Supported hardware

|Board                              | Minimum BIOS Version |
|-----------------------------------|----------------------|
|Asus ROG Crosshair Hero VII (WiFi) | 1002                 |
|Asus ROG Crosshair Hero VII        | 1002                 |
|Asus ROG Crosshair Hero VI         | 6301                 |
|Asus ROG Crosshair Hero VI (WiFi)  | 6302                 |
|Asus ROG Crosshair Hero VI Extreme | ?                    |
|ROG STRIX B450-E GAMING            | 2406                 |
|ROG STRIX B450-F GAMING            | 2406                 |
|ROG STRIX B450-I GAMING            | 2406                 |
|ROG STRIX X470-F GAMING            | 5007                 |
|ROG STRIX X470-I GAMING            | ?                    |
|ROG STRIX X399-E GAMING            |                      |
|ASUS Zenith Extreme                | 1607/1701            |
|ASUS Zenith Extreme Alpha          | 0207                 |
|Prime X399-A                       | 1002                 |
|Prime X470-Pro                     | 4602                 |

## Currently don't work / unknown

|Board                              |
|-----------------------------------|
|X570 boards (no WMI interface - use `nct6775` driver instead)|
|Prime B450-Plus|
|Prime X370-Pro|
|TUF B450-PLUS GAMING|
|TUF X470-PLUS GAMING|

## How to install

Ensure you have lm_sensors, DKMS, kernel sources, GCC etc installed. Kernel version 4.12 or later is required. 

If you have built your own kernel, it must have been built with CONFIG_HWMON (found under Device Drivers) and CONFIG_ACPI_WMI (found under Device Drivers -> X86 Platform Specific Device Drivers) enabled. It is not necessary to have built the "ASUS WMI Driver" (CONFIG_ASUS_WMI) module.

### Arch Linux
Available as an AUR package - https://aur.archlinux.org/packages/asus-wmi-sensors-dkms-git/

This hooks into DKMS to build a module for your available kernels and adds a ```/etc/module-load.d/``` entry so the module is loaded at boot.

The module can be manually loaded by issuing ```sudo modprobe asus_wmi_sensors```

Run ```sensors``` and you should see a ```asuswmisensors-isa-0000``` device and readouts as you see in the UEFI interface.

### Gentoo
An ebuild is available in gyakovlev's overlay. https://github.com/gyakovlev/gentoo-overlay/tree/master/sys-kernel/asus-wmi-sensors

### Other distributions

Clone the git repository: ```git clone https://github.com/electrified/asus-wmi-sensors.git```

Build the module ```sudo make dkms```

Insert the module ```sudo modprobe asus-wmi-sensors```

Run ```sensors``` and you should see a ```asuswmisensors-isa-0000``` device and readouts as you see in the UEFI interface.

Optional - consult your distro's documentation for info on how to make the module be loaded automatically at boot

## FAQ

### I think my motherboard should be supported but it doesn't work, what can I do?
First verify that WMI hardware monitoring is working for your board under Windows. Both HWiNFO (https://www.hwinfo.com/) and SIV(http://rh-software.com/) will make use of ASUS WMI for reading sensors if available. If your board is supported by those, post the output of ```sudo dmidecode -t baseboard``` and it should be possible to add support.

### Why do some of my temperatures return 216 deg C?
This is the value returned for temperature sensor headers with no sensor connected.

### Why are some of the sensors (e.g. CPU Core Voltage) duplicated?
The driver simply returns all the sensors available in the WMI output. CPU Voltage and others are included twice, in the Embedded Controller and SIO banks.

### Why is reading from the sensors so slow?
This driver is not reading from the SuperIO/ Embedded controller directly, it uses a WMI interface put in the UEFI firmware by ASUS. Reading from this WMI interface seems inherently slow. I am investigating calling the underlying ACPI methods that the WMI interface calls which I have been told performs better.

### Why does this driver exist?
Many of Asus' recent Ryzen motherboards have the ITE IT8665E sensor IC, which does not have any publicly available datasheets. Some support has been added to the out-of-tree IT87 driver, but this is currently unmaintained and not working on recent kernels. Also many Windows drivers are moving to use this WMI interface rather than accessing the chip directly as this avoids conflicts when multiple monitoring apps attempt to read the sensors simultaneously.

### Why have you created a new driver and not added to the existing Asus/eeepc drivers?
- The existing drivers are basic platform devices rather than using the kernels' WMI bus
- These new sensor methods are on a different WMI class - "ASUSHW" than the existing "ASUSManagment" class which the other driver uses. The existing driver largely deals with laptop functionality (hotkeys, WiFi kill switches, screen brightness). Adding to that driver support for this additional sensors functionality would make it quite large.

### Is it possible to control the speed of fans with this driver?
No, fan control is not part of the Asus sensors WMI interface. It may be possible via an undocumented method, but that would require reverse engineering effort.

### I am using Ubutu with a Ukuu kernel and the module won't build
If you use ukuu, or any other method to install a kernel, you should use the version of GCC used to build the kernel to build any additional out-of-tree modules otherwise you may run into issues.

Check `cat /proc/version` to see the version of GCC used to build the kernel, and upgrade your GCC install appropriately. (Usually an upgrade from GCC 7 to GCC 9 is needed)

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

## Known Issues
 - The WMI implementation in some of Asus' BIOSes is buggy. This can result in fans stopping, fans getting stuck at max speed, or temperature readouts getting stuck. This is not an issue with the driver, but the BIOS. The Prime X470 Pro seems particularly bad for this. The more frequently the WMI interface is polled the greater the potential for this to happen. Until you have subjected your computer to an extended soak test while polling the sensors frequently, don't leave you computer unattended. I can personally say I've seen the issue on the Crosshair VII with BIOS 2606 and a Ryzen 2700X, upgrading to 3004 rectified the issue.
 - A few boards report 12v voltages to be ~10v. Once again this is a BIOS issue.

## Thanks
- Ray Hinchcliffe, author of SIV for info
- Original authors of the IT87 makefile
- Authors of other mainlined HWMON kernel modules that I've studied while writing this
