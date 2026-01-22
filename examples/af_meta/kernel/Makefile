# AF_QDMETA Kernel Module Makefile

obj-m := qd_af_meta.o

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
	insmod qd_af_meta.ko

unload:
	rmmod qd_af_meta

.PHONY: all clean install load unload
