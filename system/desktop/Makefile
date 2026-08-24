############################################################################
# apps/system/desktop/Makefile
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = $(CONFIG_SYSTEM_DESKTOP_PROGNAME)
PRIORITY  = $(CONFIG_SYSTEM_DESKTOP_PRIORITY)
STACKSIZE = $(CONFIG_SYSTEM_DESKTOP_STACKSIZE)
MODULE    = $(CONFIG_SYSTEM_DESKTOP)

CFLAGS += -I$(APPDIR)/interpreters/quickjs/quickjs

CSRCS   = desktop_font.c qpk_runtime.c
MAINSRC = desktop_main.c

include $(APPDIR)/Application.mk
