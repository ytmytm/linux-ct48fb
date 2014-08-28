OPTS=-Wall -O2 -mpentium -march=pentium -mcpu=pentium
DEFS=-DUSE_OWN_FBGEN
INC=/lib/modules/`uname -r`/build/include

all: ct-fbgen.h vga.h ct48fb.c
	gcc -I $(INC) -D__KERNEL__ -DMODULE $(DEFS) $(OPTS) -c ct48fb.c -o ct48fb.o

install:
	install -m 644 ct48fb.o /lib/modules/`uname -r`/kernel/drivers/video
	depmod -a

clean:
	rm *.o
