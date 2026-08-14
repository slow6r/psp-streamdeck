TARGET = pspdeck
OBJS = main.o ui.o net.o font8x8.o

INCDIR =
CFLAGS = -O2 -G0 -Wall -std=gnu99
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBDIR =
LDFLAGS =
LIBS = -lpspsdk -lpspdisplay -lpspctrl -lpsppower -lpspwlan \
       -lpspnet -lpspnet_inet -lpspnet_apctl -lpspnet_resolver \
       -lpsputility -lc

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = PSP Deck

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
