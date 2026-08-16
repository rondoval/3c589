# File: Makefile

CFLAGS = -msmall-code -O3 -ansi -Wall -Wno-uninitialized -Wno-parentheses
CC = gcc $(CFLAGS)
MAKEFILE = Makefile
LINKER = ld -e _Main -s
AMIGA_LIB = GG:lib/libamiga.a
C_LIB = GG:lib/libnix/libnix13.a

DEV_NAME = 3c589.device
OBJS = device.o unit.o request.o
HDRS = device.h

.c.o :
	$(CC) -c $<

$(DEV_NAME): $(MAKEFILE) $(OBJS)
	$(LINKER) $(OBJS) $(AMIGA_LIB) $(C_LIB) -o $(DEV_NAME)
	Protect "$(DEV_NAME)" "-e"

$(OBJS): $(MAKEFILE) $(HDRS)

unit.o: etherlink3.h

install: $(DEV_NAME)
	Copy CLONE "$(DEV_NAME)" "DEVS:networks/"
	Flush "$(DEV_NAME)"

clean:
	Delete "$(DEV_NAME)" "#?.o"

