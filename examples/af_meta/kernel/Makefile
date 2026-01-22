# AF_QDMETA Kernel Module Makefile

obj-m := qd_af_meta.o

KDIR ?= /lib/modules/$(shell uname -r)/build
ROOTDIR ?= $(abspath $(CURDIR)/../../..)
BUILDDIR ?= $(ROOTDIR)/build
KMOD_OUTPUT ?= $(BUILDDIR)/kmods/af_meta

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) MO=$(KMOD_OUTPUT) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) MO=$(KMOD_OUTPUT) clean

install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) MO=$(KMOD_OUTPUT) modules_install
	depmod -a

load:
	insmod $(KMOD_OUTPUT)/qd_af_meta.ko

unload:
	rmmod qd_af_meta

.PHONY: all clean install load unload
