include $(APPDIR)/Make.defs

PROGNAME  = $(CONFIG_SYSTEM_C6PROBE_PROGNAME)
PRIORITY  = $(CONFIG_SYSTEM_C6PROBE_PRIORITY)
STACKSIZE = $(CONFIG_SYSTEM_C6PROBE_STACKSIZE)
MODULE    = $(CONFIG_SYSTEM_C6PROBE)

CFLAGS += -I$(TOPDIR)/arch/risc-v/src/common/espressif

MAINSRC = c6probe_main.c
CSRCS   = esp_hosted.c esp_hosted_rpc.c c6net.c protobuf-c.c esp_hosted_rpc.pb-c.c

include $(APPDIR)/Application.mk
