all: sensor, border
MAKE_NET = MAKE_NET_NULLNET
CONTIKI = ..
include $(CONTIKI)/Makefile.include
