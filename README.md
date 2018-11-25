## Asus WMI Sensors driver.

### Why does this driver exist?
Many of Asus' recent Ryzen motherboards have the ITE IT8665E sensor IC, which does not have any publically available datasheets. Some support has been added to the out-of-tree IT87 driver, but this is kind of unmaintained currently. Also many Windows drivers are moving to use this WMI interface rather than accessing the chip directly.

On some of these recent boards, Asus has added a sensors WMI interface to the firmware, which lets you get the same values as reported in the BIOS.

### Why have you created a new driver and not added to the existing Asus/eeepc drivers?
- The existing drivers are basic platform devices rather than using the kernels' WMI bus
- These new sensor methods are on a different WMI class - "ASUSHW" than the existing "ASUSManagment" class which the other driver uses. The existing driver largely deals with laptop functionality (hotkeys, WiFi kill switches, screen brightness). Adding to that driver support for this additional sensors functionality would make it quite large.

Currently only tested on the Asus ROG Crosshair Hero VII.

Other boards known to have this WMI implementation:
- ASUS ROG STRIX B450/X470
- ASUS ROG Zenith Extreme X399