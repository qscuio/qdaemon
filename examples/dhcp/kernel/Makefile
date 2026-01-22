# DHCP Kernel Module Makefile

obj-m := qd_dhcp_kmod.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

load:
	insmod qd_dhcp_kmod.ko

unload:
	rmmod qd_dhcp_kmod

.PHONY: all clean install load unload
