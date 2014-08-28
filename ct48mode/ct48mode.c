
#include <stdio.h>
#include <stdlib.h>

#include "lrmi.h"

int main(int argc, char *argv[]){

struct LRMI_regs r;
int mode;			/* 0101 for 640x480, 0103 for 800x600 */

	if (!LRMI_init())
		return 1;

	mode = 0x0101;		/* 640x480 is default... */
	if (argc>1) {
		if ((argv[1][0] == '8') && (argv[1][1] == '0') && (argv[1][2]=='0')) {
			mode = 0x0103;	/* ...unless user selected otherwise */
		}
	}

	ioperm(0, 0x400, 1);
	iopl(3);
	memset(&r, 0, sizeof(r));
	r.eax = 0x4f02;
	r.ebx = mode;
	if (!LRMI_int(0x10, &r)) {
	    fprintf(stderr, "Can't set video mode (vm86 failure)\n");
	    return 2;
	}

	return 0;
}
