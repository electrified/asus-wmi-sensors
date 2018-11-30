
KDIR ?= /lib/modules/$(shell uname -r)/build
obj-m := asus-wmi-sensors.o
MY_CFLAGS += -g -DDEBUG
ccflags-y += ${MY_CFLAGS}
CC += ${MY_CFLAGS}

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
	sensors
	