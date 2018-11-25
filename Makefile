
KDIR ?= /lib/modules/$(shell uname -r)/build
obj-m := asus-wmi-sensors.o

all:
	make -C $(KDIR) C=2 M=$$PWD modules

install:
	make -C $(KDIR) M=$$PWD modules_install

clean:
	make -C $(KDIR) M=$$PWD clean

test:
	-rmmod asus_wmi_sensors
	-insmod asus-wmi-sensors.ko
	dmesg
	ls -l /sys/devices/platform/PNP0C14:02/wmi_bus/wmi_bus-PNP0C14:02/466747A0-70EC-11DE-8A39-0800200C9A66/hwmon/hwmon4
	cat /sys/devices/platform/PNP0C14:02/wmi_bus/wmi_bus-PNP0C14:02/466747A0-70EC-11DE-8A39-0800200C9A66/hwmon/hwmon4/*label
	