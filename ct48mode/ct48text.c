
#include <stdio.h>
#include <stdlib.h>

#include "lrmi.h"

int main(int argc, char *argv[]){

struct LRMI_regs r;

	if (!LRMI_init())
		return 1;

	ioperm(0, 0x400, 1);
	iopl(3);
	memset(&r, 0, sizeof(r));
	r.eax = 3;

	if (!LRMI_int(0x10, &r)) {
	    fprintf(stderr, "Can't set text mode (vm86 failure)\n");
	    return 2;
	}

	return 0;
}
