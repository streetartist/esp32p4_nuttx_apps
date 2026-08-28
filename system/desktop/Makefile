############################################################################
# apps/system/desktop/Makefile
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = $(CONFIG_SYSTEM_DESKTOP_PROGNAME)
PRIORITY  = $(CONFIG_SYSTEM_DESKTOP_PRIORITY)
STACKSIZE = $(CONFIG_SYSTEM_DESKTOP_STACKSIZE)
MODULE    = $(CONFIG_SYSTEM_DESKTOP)

CFLAGS += -I$(APPDIR)/interpreters/quickjs/quickjs
CFLAGS += -I$(APPDIR)/include
CFLAGS += -I$(APPDIR)/system/c6probe

CSRCS   = desktop_font.c qpk_runtime.c qpk_net.c desktop_filemgr.c \
	  desktop_camera.c
ASRCS   = qpk_ca_bundle.S
MAINSRC = desktop_main.c

include $(APPDIR)/Application.mk
