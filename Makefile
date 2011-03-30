SOURCES=exim-socket.c msg.c net.c
HEADERS=net.h msg.h
OBJECTS=$(SOURCES:.c=.o)
LDFLAGS+=-lpthread

all: exim-quotad
exim-quotad: $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $+ $(LDLIBS)

$(OBJECTS): $(HEADERS) Makefile

.PHONY: clean
clean:
	-$(RM) *.o exim-quotad
