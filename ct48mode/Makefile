CFLAGS += -Wall

%.o: %.c
	$(CC) -c $(CPPFLAGS) $(CFLAGS) -o $@ $<

all: ct48mode ct48text modClock

ct48mode: ct48mode.c lrmi.o
	$(CC) $(CFLAGS) -o $@ $^

ct48text: ct48text.c lrmi.o
	$(CC) $(CFLAGS) -o $@ $^

modClock: modClock.c
	$(CC) $(CFLAGS) -o $@ $^

.PHONY: clean
clean:
	rm *.o
