# SPDX-License-Identifier: GPL-2.0-or-later
#
# Armiga Joypad Kernel Module Makefile
#

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

MODULE_NAME := armiga-joypad
obj-m := $(MODULE_NAME).o
$(MODULE_NAME)-y := armiga_joypad.o

ccflags-y += -I$(src) -Wall -Wextra

all:
	@echo "==> Compilando módulo $(MODULE_NAME) para el kernel..."
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	@echo "==> Limpiando archivos objeto y binarios..."
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f *.o *.ko *.mod *.mod.c .*.cmd Module.symvers modules.order

install:
	@echo "==> Instalando módulo en el sistema..."
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

.PHONY: all clean install
