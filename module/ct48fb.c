
/*
 *  linux/drivers/video/ct48fb.c - Chips&Technologies 65548/45/40 framebuffer driver
 *
 *	Copyright (C) 2003,2004 Maciej Witkowiak <ytm@elysium.pl>
 *	based on vfb, chipsfb, pm2fb, ideas from SVGALib driver (based on X11 driver)
 *	PCI support by Martin Krohn <martin.krohn@web.de>
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License. See the file COPYING in the main directory of this archive for
 *  more details.
 */

/*
    use "vga=377" to properly initialize 640x480x8 video mode
    ( or use ax=0x4f02, bx=0x0101; int 10h; in a lrmi program )

    use ax=0x30; int 10h; in a lrmi program to properly initialize 640x480x8 video mode
    to work for a Sharp LM64P80 Display connected to a PCI interfaced ct65548.

    OPTIONS:
    (kernel) noaccel/accel, noaccputc/accputc, nohwcursor/hwcursor, blink/noblink,
    inverse/noinverse, mode:<xres>x<yres>x<bpp> (see the 4 available modes below)

    DEFAULT OPTIONS:
    video=ct48fb:accel:noaccputc:hwcursor:noblink:noinverse:mode:640x480x8
*/

#include <linux/kernel.h>
#include <linux/config.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/ioport.h>
#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/console.h>
#include <video/fbcon.h>
#include <video/fbcon-cfb8.h>
#include <video/fbcon-cfb16.h>
#include <asm/io.h>
#include <linux/pci.h>
#include "vga.h"

/* this is to have fbi return with correct screen offset */
#define FBIFIX

/* conversion between var and par structures' pixclock */
#define PICOS2KHZ(a) (1000000000UL/(a))
#define KHZ2PICOS(a) (1000000000UL/(a))

struct ct48fb_cursor {
    int enable;
    int x,y;
    int w,h;
};

struct ct48fb_par {
    int bpp;
    u_long base;
    u_int pixclock;			/* in kHz */
    int linelength;
    int accel;
    u_long cursor_base;
};

struct ct48fb_info {
    struct fb_info_gen gen;

    u_long fbmem;
    u_long memsize;
    u_char *fbmem_virt;
    int chipset;
    int xres, yres;			/* these are fixed */

    struct ct48fb_par currentmode;
    struct ct48fb_cursor cursor;
};

static struct ct48fb_info fb_info;
static struct display disp;
static struct pci_dev *ct48fb_pci_dev;

static struct { u_char red, green, blue, transp; } palette[256];
static int pseudo_pal[16];
static struct fb_var_screeninfo default_var;
static char ct48fb_name[] = "ct48fb";

/* options */
static int noaccel = 0;			/* enable acceleration by default */
static int noaccputc = 1;		/* disable accelerated putc by default */
static int nohwcursor = 0;		/* enable hardware cursor by default */
static int noblink = 1;			/* disable hw cursor blink as it looks like shit */
static int noinverse = 1;		/* disable screen inverse */
static char *mode = NULL;		/* selected video mode upon start */
static volatile int wasbmove = 0;	/* hack for accelerated putc */
/* global helper variables */
static int modenum = 0;			/* selected video mode table offset upon start */
static int bpp = 8;			/* this tracks current bpp mode */
static int pci_mode = 0;                /* true, if detection of a pci board succeeded */

static const struct {
	const char *name;
	struct fb_var_screeninfo var;
} ct48fb_predefined[] __initdata = {
    { "640x480x8",	/* 640x480, 8 bpp */
	{ 640, 480, 640, 480, 0, 0, 8, 0,
	  {0, 6, 0}, {0, 6, 0}, {0, 6, 0}, {0, 0, 0},
	  0, FB_ACTIVATE_NOW, -1, -1, FB_ACCEL_NONE, 25000, 64, 64, 32, 32, 64, 2,
	  0, FB_VMODE_NONINTERLACED }
    },
    { "640x480x16",     /* 640x480, 16 bpp */
	{ 640, 480, 640, 480, 0, 0, 16, 0,
	  {0, 5, 0}, {5, 6, 0}, {11, 5, 0}, {0, 0, 0},
	  0, FB_ACTIVATE_NOW, -1, -1, FB_ACCEL_NONE, 20000, 64, 64, 32, 32, 64, 2,
	  0, FB_VMODE_NONINTERLACED }
    },
    { "800x600x8",	/* 800x600, 8 bpp */
	{ 800, 600, 800, 600, 0, 0, 8, 0,
	  {0, 6, 0}, {0, 6, 0}, {0, 6, 0}, {0, 0, 0},
	  0, FB_ACTIVATE_NOW, -1, -1, FB_ACCEL_NONE, 25000, 64, 64, 32, 32, 64, 2,
	  0, FB_VMODE_NONINTERLACED }
    },
    { "800x592x16",     /* 800x592, 16 bpp */
	{ 800, 592, 800, 592, 0, 0, 16, 0,
	  {0, 5, 0}, {5, 6, 0}, {11, 5, 0}, {0, 0, 0},
	  0, FB_ACTIVATE_NOW, -1, -1, FB_ACCEL_NONE, 20000, 64, 64, 32, 32, 64, 2,
	  0, FB_VMODE_NONINTERLACED }
    },
    { "\0", },
};

static struct fb_ops ct48fb_ops = {
	owner:		THIS_MODULE,
	fb_get_fix:	fbgen_get_fix,
	fb_get_var:	fbgen_get_var,
	fb_set_var:	fbgen_set_var,
	fb_get_cmap:	fbgen_get_cmap,
	fb_set_cmap:	fbgen_set_cmap,
	fb_pan_display:	fbgen_pan_display,
};
/* -----------------PCI ---------------------------------------------------- */
static struct pci_driver ct48fb_pci_driver;
static int __devinit ct48_pci_probe(struct pci_dev * dev, const struct pci_device_id * id)
{
    int err;
  
    err = pci_enable_device(dev);
    if (err)
        return err;
      
    ct48fb_pci_dev = dev;
      
    return 0;
}

static void __devexit ct48fb_pci_remove(struct pci_dev * dev)
{
    if (pci_mode) {
        pci_unregister_driver(&ct48fb_pci_driver);
    }
}

static struct pci_device_id ct_devices[] __devinitdata = {
    {PCI_VENDOR_ID_CT,PCI_DEVICE_ID_CT_65548, PCI_ANY_ID,PCI_ANY_ID,0,0,0},
    {0,}
};

static struct pci_driver ct48fb_pci_driver = {
    name:"ct48fb",
    id_table:ct_devices,
    probe:ct48_pci_probe,
    remove:ct48fb_pci_remove,
};
/* ------------------- low level functions --------------------------------- */

/* reference clock frequency [kHz] */
#define CT48_REFERENCE_CLOCK 14318

/* definitions not covered by vga.h */
#define	VGA_XR_I	0x3d6
#define VGA_XR_D	0x3d7

/* extra blitter register */
#define DR00	0x83d0
#define DR02	0x8bd0
#define DR03	0x8fd0
#define DR04	0x93d0
#define DR05	0x97d0
#define DR06	0x9bd0
#define DR07	0x9fd0
#define DR08	0xa3d0
#define DR09	0xa7d0
#define DR0A	0xabd0
#define DR0B	0xafd0
#define DR0C	0xb3d0

#define write_ind(num, val, ap, dp)	do { \
	vga_io_w((ap), (num)); vga_io_w((dp), (val)); \
} while (0)
#define read_ind(num, val, ap, dp)	do { \
	vga_io_w((ap), (num)); val = vga_io_r((dp)); \
} while (0)

/* extension registers */
#define write_xr(num, val)	write_ind(num, val, VGA_XR_I, VGA_XR_D)
#define read_xr(num, var)	read_ind(num, var, VGA_XR_I, VGA_XR_D)
/* CRTC registers */
#define write_cr(num, val)	write_ind(num, val, VGA_CRT_IC, VGA_CRT_DC)
#define read_cr(num, var)	read_ind(num, var, VGA_CRT_IC, VGA_CRT_DC)
/* graphics registers */
#define write_gr(num, val)	write_ind(num, val, VGA_GFX_I, VGA_GFX_D)
#define read_gr(num, var)	read_ind(num, var, VGA_GFX_I, VGA_GFX_D)
/* sequencer registers */
#define write_sr(num, val)	write_ind(num, val, VGA_SEQ_I, VGA_SEQ_D)
#define read_sr(num, var)	read_ind(num, var, VGA_SEQ_I, VGA_SEQ_D)
/* attribute registers - slightly strange */
#define write_ar(num, val)	do { \
	vga_io_r(0x3da); write_ind(num, val, VGA_ATT_W, VGA_ATT_W); \
} while (0)
#define read_ar(num, var)	do { \
	vga_io_r(0x3da); read_ind(num, var, VGA_ATT_W, VGA_ATT_R); \
} while (0)

#define N_ELTS(x)	(sizeof(x) / sizeof(x[0]))

struct chips_init_reg {
	u_char addr;
	u_char data;
};

enum { ENTER, LEAVE };
enum { CT_520, CT_525, CT_530, CT_535, CT_540, CT_545, CT_546, CT_548, CT_550, CT_554,
       CT_555, CT_8554, CT_9000, CT_4300 };

static void CHIPS_enterleave(int enter)
{
    u_int tmp;

    if (enter == ENTER ) {
	/* Unprotect CRTC[0-7] */
	read_cr(VGA_CRTC_V_SYNC_END, tmp);
	write_cr(VGA_CRTC_V_SYNC_END, tmp & 0x7f);
    } else {
	/* Protect CRTC[0-7] */
	read_cr(VGA_CRTC_V_SYNC_END, tmp);
	write_cr(VGA_CRTC_V_SYNC_END, (tmp & 0x7f)|0x80);
    }
}

static inline u_long CHIPS_linearbase(void)
{
    u_int tmp;

    if (pci_mode) {
	return pci_resource_start (ct48fb_pci_dev, 0);
    } else {
	read_xr(0x08, tmp);
	return ((tmp & 0xff) << 20);
    }
}

static void CHIPS_setdisplaystart(u_long addr)
{
    if (pci_mode)
	return;
    addr >>= 2;
    write_cr(0x0D, (addr & 0x000000FF));
    write_cr(0x0C, (addr & 0x0000FF00)>>8);
    write_xr(0x0C, (addr & 0x00FF0000)>>16);
}

static inline void CHIPS_cursorinit(struct ct48fb_info *i)
{
    outl(i->currentmode.cursor_base, DR0C);	/* set cursor base address */
    outl(0x00000020, DR08);		/* hidden, 32x32, pop-up thing disabled, */
					/* ULC is 0,0 of image, blinking disabled (XR60) */
}

static __init int CHIPS_detectchipset(void)
{
    u_int tmp;
    int chipset;

    chipset=-1;
    read_xr(0x00, tmp);
    if (tmp != 0xa5) {
		if ((tmp & 0xF0) == 0x70)
		{	chipset = CT_520;
		}
		else
		if ((tmp & 0xF0) == 0x80)	/* Could also be a 65525 */
		{	chipset = CT_530;
		}
		else
		if ((tmp & 0xF8) == 0xC0)
		{	chipset = CT_535;
		}
		else
		if ((tmp & 0xF8) == 0xD0)
		{	chipset = CT_540;
		}
		else
		if ((tmp & 0xF8) == 0xD8)
		{
		    switch (tmp&0x7) {
		      case 3:
			chipset = CT_546;
			break;
		      case 4:
			chipset = CT_548;
			break;
		      default:
			chipset = CT_545;
		    }
		}
    }

    if ((tmp != 0) && (chipset <0)) {
        read_xr(0x02, tmp);
        switch(tmp) {
		case 0x30:
		case 0xC0:
		    chipset = CT_9000;
		    break;
		case 0xE0:
    		    chipset = CT_550;
		    break;
		case 0xE4:
		    chipset = CT_554;
		    break;
		case 0xE5:
		    chipset = CT_555;
		    break;
		case 0xF4:
		    chipset = CT_8554;
		    break;
		default:
		    chipset = -1;
	}
    }
    return chipset;
}

static __init u_int CHIPS_memorysize(void)
{
    u_int tmp, video_memory;

    video_memory = 1024;
    read_xr(0x0f,tmp);
    switch (tmp & 3) {
	case 0:
	    video_memory = 256;
	    break;
	case 1:
	    video_memory = 512;
	    break;
	case 2:
	case 3:
	    video_memory = 1024;
	    break;
    }
    return video_memory*1024;
}

static __init int CHIPS_detectconfiguration(void)
{
    u_int tmp;

    read_xr(0x01,tmp);
    switch (tmp & 7) {
    case 3:
        /*direct*/
        printk("ct48fb: bus type: CPU Direct\n");
        return 0;
    case 6:
        /*PCI*/
        printk("ct48fb: bus type: PCI Bus\n");
        return 1;
     case 7:
	/*VL*/
	printk("ct48fb: bus type: VL Bus\n");
	return 0;
    default:
	printk("ct48fb: bus type unknown, defaulting to VL Bus\n");
	return 0;
    }
}

/* these are for 640x480 */

static struct chips_init_reg chips_640_init8_xr[] = {
    { 0x40, 0x01 },			/* 8bpp blitter mode */
    { 0x03, 0x02 },
    { 0x06, 0xC2 },
    { 0x0F, 0x82 },
    { 0x17, 0x00 },
    { 0x19, 0x6A },
    { 0x1A, 0x1A },
    { 0x1B, 0x81 },
    { 0x1C, 0x63 },
    { 0x1E, 0x4A },
    { 0x2B, 0x79 },
    { 0x55, 0x03 },
    { 0x57, 0x03 },
};

static struct chips_init_reg chips_640_init8_cr[] = {
    { 0x00, 0x61 },
    { 0x01, 0x4F },
    { 0x02, 0x50 },
    { 0x04, 0x53 },
    { 0x05, 0x9F },
    { 0x07, 0x3E },
    { 0x11, 0x0C },
    { 0x12, 0xDF },
    { 0x13, 0x50 },
};

static struct chips_init_reg chips_640_init16_xr[] = {
    { 0x40, 0x02 },			/* 16bpp blitter mode */
    { 0x03, 0x2A },
    { 0x06, 0xCE },
    { 0x0F, 0x92 },
    { 0x17, 0x09 },
    { 0x19, 0xD5 },
    { 0x1A, 0x15 },
    { 0x1B, 0x07 },
    { 0x1C, 0xC7 },
    { 0x1E, 0xA0 },
    { 0x2B, 0x41 },
    { 0x55, 0x03 },
    { 0x57, 0x03 },
};

static struct chips_init_reg chips_640_init16_cr[] = {
    { 0x00, 0xC7 },
    { 0x01, 0x9F },
    { 0x02, 0x9F },
    { 0x04, 0xA8 },
    { 0x05, 0x96 },
    { 0x07, 0x3E },
    { 0x11, 0x25 },
    { 0x12, 0xDF },
    { 0x13, 0xA0 },
};

/* these are for 800x600 */

static struct chips_init_reg chips_init8_xr[] = {
    { 0x40, 0x01 },			/* 8bpp blitter mode */
    { 0x03, 0x02 },
    { 0x06, 0xC2 },
    { 0x0F, 0x82 },
    { 0x17, 0x00 },
    { 0x19, 0x6A },
    { 0x1A, 0x1A },
    { 0x1B, 0x81 },
    { 0x1C, 0x63 },
    { 0x1E, 0x4A },
    { 0x2B, 0x32 },
    { 0x55, 0xF1 },
};

static struct chips_init_reg chips_init8_cr[] = {
    { 0x00, 0x81 },
    { 0x01, 0x63 },
    { 0x02, 0x64 },
    { 0x04, 0x6A },
    { 0x05, 0x1A },
    { 0x07, 0xF0 },
    { 0x11, 0x0C },
    { 0x12, 0x57 },
    { 0x13, 0x64 },
};

static struct chips_init_reg chips_init16_xr[] = {
    { 0x40, 0x02 },			/* 16bpp blitter mode */
    { 0x03, 0x02 },
    { 0x06, 0xCE },
    { 0x0F, 0x92 },
    { 0x17, 0x08 },
    { 0x19, 0xD7 },
    { 0x1A, 0x17 },
    { 0x1B, 0xF0 },
    { 0x1C, 0xC7 },
    { 0x1E, 0xC8 },
    { 0x2B, 0x43 },
    { 0x55, 0xF1 },
};

static struct chips_init_reg chips_init16_cr[] = {
    { 0x00, 0xFB },
    { 0x01, 0xC7 },
    { 0x02, 0xC7 },
    { 0x04, 0xD2 },
    { 0x05, 0x1C },
    { 0x07, 0xF0 },
    { 0x11, 0x0C },
    { 0x12, 0x4F },
    { 0x13, 0xC8 },
};

static void CHIPS_calcmnp(u_int clk, u_char* mm, u_char* nn, u_char* pp, u_char *psn)
{
    u_char m, n, p, ps;
    u_int f;
    int curr, delta=100000;

    *mm=*nn=*pp=0;
    for (m=3; m<=127; m++) {
        for (n=3; n<=127; n++) {
    	    for (p=0; p<=5; p++) {
	        for (ps=1; ps<5; ps=ps+3) {
	    	    f = CT48_REFERENCE_CLOCK*4/(n*ps);
		    if ((f>=(150*4)) && (f<= (2000*4))) {
    		        f = f*m;
		        if ((f>=48000) && (f<=220000)) {
			    f = f/(1<<p);
			    curr= clk>f ? clk-f : f-clk;
			    if (curr<delta) {
				delta=curr;
				*mm=m; *nn=n; *pp=p; *psn=ps;
			    }
			}
		    }
		}
	    }
	}
    }
}

static void CHIPS_setclock(u_int pixclock)
{
    static u_int lastpixclock;
    u_int tmp;
    u_char m, n, p, psn;
    u_int reg30 = 0;
    
    if (pci_mode)
	return;

    if (lastpixclock != pixclock) {
	lastpixclock = pixclock;

	CHIPS_calcmnp(pixclock, &m, &n, &p, &psn);
	reg30 = p << 1;
	if (psn == 1)
	    reg30++;

	read_xr(0x33, tmp);
	write_xr(0x33, tmp & ~0x20);
	write_xr(0x30, reg30);
	write_xr(0x31, m-2);
	write_xr(0x32, n-2);
	write_xr(0x33, tmp);
    }
}

static void CHIPS_8bpp_setmode(int xres)
{
    int i;
    
    /* rely on lrmi tool to set mode */
    if (pci_mode)
	return;

    bpp = 8;
    if (xres == 800) {
	for (i = 0; i < N_ELTS(chips_init8_xr); ++i)
		write_xr(chips_init8_xr[i].addr, chips_init8_xr[i].data);
	for (i = 0; i < N_ELTS(chips_init8_cr); ++i)
		write_cr(chips_init8_cr[i].addr, chips_init8_cr[i].data);
    } else
    if (xres == 640) {
	for (i = 0; i < N_ELTS(chips_640_init8_xr); ++i)
		write_xr(chips_640_init8_xr[i].addr, chips_640_init8_xr[i].data);
	for (i = 0; i < N_ELTS(chips_640_init8_cr); ++i)
		write_cr(chips_640_init8_cr[i].addr, chips_640_init8_cr[i].data);
    }
};

static void CHIPS_16bpp_setmode(int xres)
{
    int i;

    /* rely on lrmi tool to set mode */
    if (pci_mode)
	return;

    bpp = 16;
    if (xres == 800) {
	for (i = 0; i < N_ELTS(chips_init16_xr); ++i)
		write_xr(chips_init16_xr[i].addr, chips_init16_xr[i].data);
	for (i = 0; i < N_ELTS(chips_init16_cr); ++i)
		write_cr(chips_init16_cr[i].addr, chips_init16_cr[i].data);
    } else
    if (xres == 640) {
	for (i = 0; i < N_ELTS(chips_640_init16_xr); ++i)
		write_xr(chips_640_init16_xr[i].addr, chips_640_init16_xr[i].data);
	for (i = 0; i < N_ELTS(chips_640_init16_cr); ++i)
		write_cr(chips_640_init16_cr[i].addr, chips_640_init16_cr[i].data);
    }
}

static __init void CHIPS_init(void)
{
    u_int tmp;

    write_xr(0x07, 0xf4);			/* set base for DR registers, start @ 0x83d0 */
    write_xr(0x03, 0x02);			/* enable 32-bit DR registers */
    /*
     * ct48_setmode already set this register,
     * so don't set to a wrong value
     *  however: bit #0 must be set to work correctly
     */
    if (!pci_mode)
        write_xr(0x04, 0x24);  
    read_xr(0x0b, tmp);
    write_xr(0x0b, tmp | 0x10);			/* enable linear addressing mode */
    write_xr(0x15, 0x00);			/* unprotect everything */
    write_xr(0x55, 0xF1);
    read_xr(0x72, tmp);
    if ((noaccel==0) && ((tmp & 0x80) != 0)) {
	printk(KERN_ERR "ct48fb: couldn't enable blitter, acceleration disabled\n");
	noaccel = 1;
    }
    write_xr(0x70, 0x00);			/* unprotect 0x3c3 */
    read_xr(0x63,tmp);				/* setup screen inverse */
    if (noinverse)
	write_xr(0x63, tmp & 0x7f);
    else
	write_xr(0x63, tmp | 0x80);
}


/* ------------------- acceleration engine functions prototypes ------------ */

static void ct48fb_acc_setup(struct display *p);
static void ct48fb_acc_bmove(struct display *p, int sy, int sx, int dy, int dx, int height, int width);
static void ct48fb_acc_clear(struct vc_data *conp, struct display *p, int sy, int sx, int h, int w);
static void ct48fb_acc_putc(struct vc_data *conp, struct display *p, int c, int yy, int xx);
static void ct48fb_acc_putcs(struct vc_data *conp, struct display *p, const unsigned short *s, int count, int yy, int xx);
static void ct48fb_acc_revc(struct display *p, int xx, int yy);
static void ct48fb_acc_clear_margins(struct vc_data *conp, struct display *p, int bottom_only);
static void ct48fb_acc_cursor(struct display* p, int mode, int x, int y);
static int  ct48fb_acc_set_font(struct display* p, int w, int h);
static void ct48fb_set_cursor_shape(struct ct48fb_info *p);

static struct display_switch ct48fb_accel = {
    setup:		ct48fb_acc_setup,
    bmove:		ct48fb_acc_bmove,
    clear:		ct48fb_acc_clear,
    putc:		ct48fb_acc_putc,
    putcs:		ct48fb_acc_putcs,
    revc:		ct48fb_acc_revc,
    clear_margins:	ct48fb_acc_clear_margins,
    cursor:		ct48fb_acc_cursor,
    set_font:		ct48fb_acc_set_font,
    fontwidthmask:	FONTWIDTH(4)|FONTWIDTH(8)|FONTWIDTH(12)|FONTWIDTH(16)
};

/* ------------------- generic framebuffer functions ----------------------- */

#ifdef USE_OWN_FBGEN
/* include our generic FB functions */
#include "ct-fbgen.h"
#endif

/* ------------------- chipset specific functions -------------------------- */

static void ct48fb_detect(void)
{
    /* Do nothing. */
}

static int ct48fb_encode_fix(struct fb_fix_screeninfo *fix, const void *par, struct fb_info_gen *info)
{
    struct ct48fb_info * i = (struct ct48fb_info *)info;
    struct ct48fb_par * p = (struct ct48fb_par *)par;

    memset(fix, 0, sizeof(struct fb_fix_screeninfo));
    strcpy(fix->id, ct48fb_name);

    fix->smem_start = i->fbmem;
    fix->smem_len = i->memsize - 96000 - 1024;

    fix->visual = p->bpp==8 ? FB_VISUAL_PSEUDOCOLOR:FB_VISUAL_TRUECOLOR;

    fix->type = FB_TYPE_PACKED_PIXELS;
    fix->type_aux = 0;
    fix->xpanstep = 0;
    fix->ypanstep = 1;
    fix->ywrapstep = 0;
    fix->line_length = p->linelength;

    if (!noaccel)
	fix->accel = FB_ACCEL_CT_6555x;	/* partially true... */
    else
	fix->accel = FB_ACCEL_NONE;

    return 0;
}

static int ct48fb_decode_var(const struct fb_var_screeninfo *var, void *par, struct fb_info_gen *info)
{
    struct ct48fb_info * i = (struct ct48fb_info *)info;
    struct ct48fb_par p;
    struct fb_var_screeninfo *pvar;

    /*
     *  Get the video params out of 'var'. If a value doesn't fit, round it up,
     *  if it's too big, return -EINVAL.
     *
     *  Suggestion: Round up in the following order: bits_per_pixel, xres,
     *  yres, xres_virtual, yres_virtual, xoffset, yoffset, grayscale,
     *  bitfields, horizontal timing, vertical timing.
     */

    memset(&p, 0, sizeof(struct ct48fb_par));

    if (var->bits_per_pixel > 16)
	return -EINVAL;

    p.bpp=var->bits_per_pixel;

    if (p.bpp <=8)
	p.bpp = 8;
    else
	p.bpp = 16;

    p.linelength = var->xres_virtual * p.bpp/8;

    /* this is to make gcc quiet - I know what I'm doing */
    pvar = (struct fb_var_screeninfo*)var;
    pvar->yres_virtual = (((i->memsize - 96000 - 1024)/(var->xres_virtual*(var->bits_per_pixel/8)))/8)*8;

    switch (var->xres) {
	case 640:
	    pvar->yres = 480;
	break;
	case 800:
	    /* with 16bpp up to 800x592 can be done */
	    if (var->bits_per_pixel == 16)
		pvar->yres = 592;
	    else
		pvar->yres = 600;
	break;
	default:
	    return -EINVAL;
	break;
    }

    /* finally read xres/yres */
    i->xres = var->xres; i->yres = var->yres;

    if ((p.linelength * var->yres_virtual) > (i->memsize-96000-1024))
	return -EINVAL;

    p.cursor_base = p.linelength * var->yres_virtual;

    p.pixclock = PICOS2KHZ(var->pixclock);
    if ((p.pixclock < 5000)||(p.pixclock> 220000))
	p.pixclock = 40000;

    p.base = p.linelength * var->yoffset;
#ifdef FBIFIX
    CHIPS_setdisplaystart(p.base); /// XXX fbi fucks offset without it
#endif

    p.accel = var->accel_flags;

    *((struct ct48fb_par* )par)=p;

    return 0;
}

static int ct48fb_encode_var(struct fb_var_screeninfo *var, const void *par, struct fb_info_gen *info)
{
    /*
     *  Fill the 'var' structure based on the values in 'par' and maybe other
     *  values read out of the hardware.
     */
    struct ct48fb_info * i = (struct ct48fb_info *)info;
    struct ct48fb_par * p = (struct ct48fb_par *)par;
    struct fb_var_screeninfo v;
    memset(&v, 0, sizeof(struct fb_var_screeninfo));

    v.bits_per_pixel = p->bpp;
    v.grayscale = 0;

    v.xres = i->xres;
    v.xres_virtual = i->xres;
    v.yres = i->yres;
    if ((v.bits_per_pixel == 16) && (v.yres == 600))
	v.yres = 592;
    v.xoffset = 0;
    v.yoffset = p->base / p->linelength;
    v.yres_virtual = (((i->memsize - 96000 - 1024)/(v.xres_virtual*(v.bits_per_pixel/8)))/8)*8;

    v.transp.offset = 0;
    v.transp.length = 0;
    switch (p->bpp) {
	case 8:
	    v.red.offset = 0;
	    v.green.offset = 0;
	    v.blue.offset = 0;
	    v.red.length = 6;
	    v.green.length = 6;
	    v.blue.length = 6;
	break;
	case 16:
	    v.red.offset = 11;
	    v.green.offset = 5;
	    v.blue.offset = 0;
	    v.red.length = 5;
	    v.green.length = 6;
	    v.blue.length = 5;
	break;
	default:
	    return -EINVAL;
	break;
    }

    v.accel_flags = p->accel;

    /* put some misc shit to make fbset output sane values */
    v.nonstd = 0;
    v.height = -1; v.width = -1;
    v.pixclock = KHZ2PICOS(p->pixclock);
    v.left_margin = 64; v.right_margin = 64;
    v.upper_margin = 32; v.lower_margin = 32;
    v.hsync_len = 64; v.vsync_len = 2;
    v.sync = 0; v.vmode = FB_VMODE_NONINTERLACED;

    *var = v;

    return 0;
}

static void ct48fb_get_par(void *par, struct fb_info_gen *info)
{
    struct ct48fb_info * i = (struct ct48fb_info *)info;
    struct ct48fb_par * p = (struct ct48fb_par *)par;

    *p = i->currentmode;
}

static int ct48fb_pan_display(const struct fb_var_screeninfo *var, struct fb_info_gen *info)
{
    u_long offset;
    struct ct48fb_info * i = (struct ct48fb_info *)info;

    offset = (var->xoffset + (var->yoffset * var->xres)) * var->bits_per_pixel/8;
    i->currentmode.base = offset;
    CHIPS_setdisplaystart(offset);

    return 0;
}

static void ct48fb_set_par(const void *par, struct fb_info_gen *info)
{
    struct ct48fb_info * i = (struct ct48fb_info *)info;
    struct ct48fb_par * p = (struct ct48fb_par *)par;
    /*
     *  Set the hardware according to 'par'.
     */

    /* setup for 16bpp/8bpp mode and blitter mode */
    switch (p->bpp) {
	case 8:
	    CHIPS_8bpp_setmode(i->xres);
	break;
	case 16:
	    CHIPS_16bpp_setmode(i->xres);
	break;
    }
    CHIPS_setclock(p->pixclock);
    CHIPS_setdisplaystart(p->base);

    if ((p->accel & FB_ACCELF_TEXT)==0) {
	/* turn off the cursor */
	nohwcursor = 1;
	ct48fb_accel.cursor = NULL;
	ct48fb_accel.set_font = NULL;
	if ((fb_info.chipset == CT_548)||(fb_info.chipset == CT_545))
	    outl(0x00000000, DR08);
	/* there's no "else" with turning the cursor back on as once it is disabled,
	   software cursor kicks in and I don't know how to disable it */
    }

    i->currentmode = *p;

    if (!nohwcursor) {
	CHIPS_cursorinit(i);
	ct48fb_set_cursor_shape(i);
    }
}

static int ct48fb_getcolreg(unsigned regno, unsigned *red, unsigned *green,
			 unsigned *blue, unsigned *transp, struct fb_info *info)
{
    struct ct48fb_info * i = (struct ct48fb_info *)info;
    int m = i->currentmode.bpp==8?256:16;

    if (regno >= m)
	return 1;
    *red = palette[regno].red;
    *green = palette[regno].green;
    *blue = palette[regno].blue;
    *transp = palette[regno].transp;

    return 0;
}

static int ct48fb_setcolreg(unsigned regno, unsigned red, unsigned green,
			 unsigned blue, unsigned transp, struct fb_info *info)
{
    struct ct48fb_info * i = (struct ct48fb_info *)info;
    int bpp = i->currentmode.bpp;
    int m = bpp==8?256:16;

    if (regno >= m)
	return 1;

    palette[regno].red = red;
    palette[regno].green = green;
    palette[regno].blue = blue;
    palette[regno].transp = transp;

    if (bpp==8) {
    	vga_io_w(VGA_PEL_IW, regno);
    	udelay(1);
    	vga_io_w(VGA_PEL_D, red>>10);
    	vga_io_w(VGA_PEL_D, green>>10);
    	vga_io_w(VGA_PEL_D, blue>>10);
    } else {
	((u16*)info->pseudo_palette)[regno] = (red & 0xF800) | ((green & 0xFC00) >> 5) | ((blue & 0xF800) >> 11);
    }

    return 0;
}

static int ct48fb_blank(int blank, struct fb_info_gen *info)
{
    /* 0 unblank, 1 blank, 2 no vsync, 3 no hsync, 4 off */
    int vgablank=0, tmp;
    struct ct48fb_info * i = (struct ct48fb_info *)info;

    switch (blank) {
	case 0: /* Screen: On; HSync: On, VSync: On */    
	    vgablank = 0;
	    write_xr(0x73, 0x00);
	    read_xr(0x52, tmp);
	    write_xr(0x52, tmp & 0xf7);	/* leave Panel Off mode */
	    udelay(1000);
	    /* for proper reinitialization */
	    if ((i->xres == 800)||((i->xres == 640)&&(i->currentmode.bpp == 16))) {
		CHIPS_8bpp_setmode(i->xres);
		if (i->currentmode.bpp == 16) {
		    udelay(500);
		    CHIPS_16bpp_setmode(i->xres);
		    if (!nohwcursor) {
			CHIPS_cursorinit(i);
			ct48fb_set_cursor_shape(i);
		    }
		}
		CHIPS_setclock(i->currentmode.pixclock+1);
	    }
	break;
	case 1: /* Screen: Off; HSync: On, VSync: On */
	    write_xr(0x73, 0x00);
	    vgablank = 1;
	break;
	case 2: /* Screen: Off; HSync: On, VSync: Off */
	    write_xr(0x73, 0x08);
	    vgablank = 1;
	break;
	case 3: /* Screen: Off; HSync: Off, VSync: On */
	    write_xr(0x73, 0x02);
	    vgablank = 1;
	break;
	case 4: /* Screen: Off; HSync: Off, VSync: Off */
	    write_xr(0x73, 0x0a);
	    vgablank = 1;
	    read_xr(0x52,tmp);
	    write_xr(0x52, tmp | 0x08);	/* enter Panel Off mode */
	break;
    }
    if (vgablank) {
	read_sr(0x01, tmp);		/* disable video output */
	write_sr(0x00, 0x01);
	write_sr(0x01, tmp | 0x20);
	write_sr(0x00, 0x03);
    } else {
	read_sr(0x01, tmp);		/* enable video output */
	write_sr(0x00, 0x01);
	write_sr(0x01, tmp & 0xdf);
	write_sr(0x00, 0x03);
    }

    return 0;
}

static void ct48fb_set_disp(const void *par, struct display *disp, struct fb_info_gen *info)
{
    struct ct48fb_info * i = (struct ct48fb_info *)info;
    struct ct48fb_par * p = (struct ct48fb_par *)par;
    struct fb_info * ii = (struct fb_info *)info;
    int isaccel = p->accel & FB_ACCELF_TEXT;

    disp->screen_base = i->fbmem_virt;
    disp->can_soft_blank = 1;
    if (noinverse)
	disp->inverse = 0;
    else
	disp->inverse = 1;

#ifdef FBCON_HAS_CFB8
    if (p->bpp == 8 ) {
	if ((!noaccel)&&(isaccel))
	    disp->dispsw = &ct48fb_accel;
	else
	    disp->dispsw = &fbcon_cfb8;
    } else
#endif
#ifdef FBCON_HAS_CFB16
    if (p->bpp == 16) {
        disp->dispsw_data =ii->pseudo_palette;	/* console palette */
	if ((!noaccel)&&(isaccel))
	    disp->dispsw = &ct48fb_accel;
	else
	    disp->dispsw = &fbcon_cfb16;
    } else
#endif
    disp->dispsw = &fbcon_dummy;
}


/* ------------ Interfaces to hardware functions ------------ */


struct fbgen_hwswitch ct48fb_switch = {
    ct48fb_detect,
    ct48fb_encode_fix,
    ct48fb_decode_var,
    ct48fb_encode_var,
    ct48fb_get_par,
    ct48fb_set_par,
    ct48fb_getcolreg,
    ct48fb_setcolreg,
    ct48fb_pan_display,
    ct48fb_blank,
    ct48fb_set_disp,
};


/* ------------ Hardware Independent Functions ------------ */

int __init ct48fb_init(void)
{

    if (check_region(0x3C0,32)) {
	printk(KERN_ERR "ct48fb: VGA I/O region is already claimed\n");
	return -EIO;
    } else {
	request_region(0x3C0, 32, "ct48fb");
    }

    pci_mode = CHIPS_detectconfiguration();
    if (pci_mode) {
	if (pci_module_init(&ct48fb_pci_driver) != 0) {
	    printk (KERN_ERR "ct48fb: pci_module_init failed\n");
	    release_region(0x3C0, 32);
	    return -EIO;
	}
    }
    
    CHIPS_enterleave(ENTER);

    fb_info.chipset = CHIPS_detectchipset();
    switch(fb_info.chipset) {
	case CT_548:
		break;
	case CT_545:
		printk (KERN_INFO "ct48fb: detected C&T65545, disabling hardware cursor blinking\n");
		noblink = 1;
		break;
	case CT_540:
		printk (KERN_INFO "ct48fb: detected C&T65540, disabling hardware acceleration\n");
		noaccel = 1;
		nohwcursor = 1;
		break;
	default:
		printk (KERN_ERR "ct48fb: couldn't find C&T65548/45/40 chipset\n");
		release_region(0x3C0,32);
		return -EIO;
    }

    fb_info.memsize = CHIPS_memorysize();

    fb_info.fbmem = CHIPS_linearbase();
    if (!request_mem_region(fb_info.fbmem, fb_info.memsize, "ct48fb"))
	printk(KERN_WARNING "ct48fb: cannot request video memory at 0x%lx\n", fb_info.fbmem);

    fb_info.fbmem_virt = ioremap(fb_info.fbmem,fb_info.memsize);
    if (!fb_info.fbmem_virt) {
	release_region(0x3C0, 32);
	release_mem_region(fb_info.fbmem, fb_info.memsize);
	printk(KERN_ERR "ct48fb: cannot ioremap video memory 0x%lx @ 0x%lx\n", fb_info.memsize, fb_info.fbmem);
	return -EIO;
    }

    fb_info.gen.parsize = sizeof (struct ct48fb_par);
    fb_info.gen.fbhw = &ct48fb_switch;
    strcpy(fb_info.gen.info.modename, ct48fb_name);
    fb_info.gen.info.changevar = NULL;
    fb_info.gen.info.node = NODEV;
    fb_info.gen.info.fbops = &ct48fb_ops;
    fb_info.gen.info.disp = &disp;

    fb_info.gen.info.switch_con = &fbgen_switch;
    fb_info.gen.info.updatevar = &fbgen_update_var;
    fb_info.gen.info.blank = &fbgen_blank;

    fb_info.gen.info.flags = FBINFO_FLAG_DEFAULT;
    fb_info.gen.info.fontname[0] = '\0';
    fb_info.gen.info.pseudo_palette = pseudo_pal;

    CHIPS_init();

    if (noaccel) {
	nohwcursor = 1;
	noaccputc  = 1;
    }

    if (modenum>3) {
	printk (KERN_ERR "ct48fb: Modenum too big:%i:%s: This should never happen!\n",modenum,mode);
	modenum = 0;
    }

    if ((modenum==0)||(modenum==2))
	bpp = 8;
    else
	bpp = 16;

    default_var = ct48fb_predefined[modenum].var;

    fb_info.xres = default_var.xres;		/// XXX it's not right
    fb_info.yres = default_var.yres;

    if (!noaccel)
	default_var.accel_flags |=FB_ACCELF_TEXT;

    default_var.activate |= FB_ACTIVATE_NOW;
    fbgen_do_set_var(&default_var, 1, &fb_info.gen);
    disp.var = default_var;
    fbgen_set_disp(-1, &fb_info.gen);
    fbgen_install_cmap(0, &fb_info.gen);

    if (register_framebuffer(&fb_info.gen.info) < 0)
	return -EINVAL;

    if (!noaccel) {
	request_region(DR00,4,"ct48fb");
	request_region(DR02,4,"ct48fb");
	request_region(DR03,4,"ct48fb");
	request_region(DR04,4,"ct48fb");
	request_region(DR05,4,"ct48fb");
	request_region(DR06,4,"ct48fb");
	request_region(DR07,4,"ct48fb");
	request_region(DR08,4,"ct48fb");
	request_region(DR09,4,"ct48fb");
	request_region(DR0A,4,"ct48fb");
	request_region(DR0B,4,"ct48fb");
	request_region(DR0C,4,"ct48fb");
    };

    fb_info.cursor.x = 0;
    fb_info.cursor.y = 0;
    fb_info.cursor.enable = 0;

    if (!nohwcursor) {
	CHIPS_cursorinit(&fb_info);
	ct48fb_set_cursor_shape(&fb_info);
    } else {
	ct48fb_accel.cursor = NULL;
	if ((fb_info.chipset == CT_548)||(fb_info.chipset == CT_545))
	    outl(0x00000000, DR08);		/* turn off the cursor */
    }

    printk(KERN_INFO "fb%d: %s frame buffer device\n", GET_FB_IDX(fb_info.gen.info.node),
	   fb_info.gen.info.modename);

    if (noaccel) {
	printk(KERN_INFO "fb%d: hardware acceleration disabled\n", GET_FB_IDX(fb_info.gen.info.node));
    } else {
      if (!noaccputc) {
          ct48fb_accel.fontwidthmask = FONTWIDTH(8)|FONTWIDTH(16);
  	  printk(KERN_INFO "fb%d: enabled accelerated putc\n", GET_FB_IDX(fb_info.gen.info.node));
      }
    }

    if (nohwcursor)
	printk(KERN_INFO "fb%d: disabled hardware cursor\n", GET_FB_IDX(fb_info.gen.info.node));

    return 0;
}

void ct48fb_cleanup(struct fb_info *info)
{
    unregister_framebuffer(&fb_info.gen.info);
    CHIPS_enterleave(LEAVE);
    release_region(0x3C0, 32);

    if (!nohwcursor)
        outl(0x00000020, DR08);		/* turn off the cursor */

    if (!noaccel) {
	release_region(DR00,4);
	release_region(DR02,4);
	release_region(DR03,4);
	release_region(DR04,4);
	release_region(DR05,4);
	release_region(DR06,4);
	release_region(DR07,4);
	release_region(DR08,4);
	release_region(DR09,4);
	release_region(DR0A,4);
	release_region(DR0B,4);
	release_region(DR0C,4);
    };
    iounmap(fb_info.fbmem_virt);
}

static void __init ct48fb_mode_setup(char* options) {
    int i;

    for (i=0; ct48fb_predefined[i].name[0] && strcmp(options, ct48fb_predefined[i].name); i++);
	if (ct48fb_predefined[i].name[0])
		modenum = i;
}

int __init ct48fb_setup(char *options)
{
    char *this_opt;

    /* set defaults */
    noaccel = 0;			/* enable acceleration by default */
    noaccputc = 1;			/* disable accelerated putc by default */
    nohwcursor = 0;			/* enable hardware cursor by default */
    noblink = 1;			/* disable blinking because it looks like shit */
    noinverse = 1;			/* disable screen inverse */
    modenum = 0;			/* default mode */
    wasbmove = 0;

    fb_info.gen.info.fontname[0] = '\0';

    if (!options || !*options)
	return 0;

    while ((this_opt = strsep(&options, ",")) != NULL) {
	if (!strncmp(this_opt, "font:", 5))
	    strcpy(fb_info.gen.info.fontname, this_opt+5);
	if (!strncmp(this_opt, "mode:", 5))
	    ct48fb_mode_setup(this_opt+5);
	if (!strncmp(this_opt, "noaccel", 7))
	    noaccel = 1;
	if (!strncmp(this_opt, "accel", 5))
	    noaccel = 0;
	if (!strncmp(this_opt, "noaccputc", 9))
	    noaccputc = 1;
	if (!strncmp(this_opt, "accputc", 7))
	    noaccputc = 0;
	if (!strncmp(this_opt, "nohwcursor", 10))
	    nohwcursor = 1;
	if (!strncmp(this_opt, "hwcursor", 8))
	    nohwcursor = 0;
	if (!strncmp(this_opt, "noblink", 7))
	    noblink = 1;
	if (!strncmp(this_opt, "blink", 5))
	    noblink = 0;
	if (!strncmp(this_opt, "noinverse", 9))
	    noinverse = 1;
	if (!strncmp(this_opt, "inverse", 7))
	    noinverse = 0;
    }
    return 0;
}


/* ------------------------------------------------------------------------- */
/*	acceleration engine */

/* These are the macros for setting the ROP's with the 6554x's */
#define ctBOTTOM2TOP            0x000
#define ctTOP2BOTTOM            0x100
#define ctRIGHT2LEFT            0x000
#define ctLEFT2RIGHT            0x200
#define ctSRCMONO               0x800
#define ctPATMONO               0x1000
#define ctSRCSYSTEM             0x4000
#define ctPATSOLID              0x80000L
#define ctBitBLTBUSY		0x100000L

/* These are the macro functions for programming the Register
 * addressed blitter for the 6554x's */
static inline void ctSETPITCH(int srcPitch, int dstPitch)
{
    outl((((dstPitch & 0xfff)<<16)|(srcPitch & 0xfff)), DR00);
}
static inline void ctSETBGCOLOR(int bgColor)
{
    outl(((bgColor & 0xff)<<8)|(bgColor & 0xff), DR02);
}
static inline void ctSETFGCOLOR(int fgColor)
{
    outl(((fgColor & 0xff)<<8)|(fgColor & 0xff), DR03);
}
static inline void ctSETBGCOLOR16(int bgColor)
{
    outl((bgColor & 0xffff), DR02);
}
static inline void ctSETFGCOLOR16(int fgColor)
{
    outl((fgColor & 0xffff), DR03);
}
static inline void ctSETROP(int op)
{
    outl(op, DR04);
}
static inline void ctBLTWAIT(void)
{
    while(inl(DR04)&ctBitBLTBUSY) { };
}
static inline void ctSETSRCADDR(u_long srcAddr)
{
    outl((srcAddr & 0x1FFFFFL), DR05);
}
static inline void ctSETDSTADDR(u_long dstAddr)
{
    outl((dstAddr & 0x1FFFFFL), DR06);
}
static inline void ctSETHEIGHTWIDTHGO(int lines, int bytes)
{
    outl((((lines & 0xfff)<<16)|(bytes & 0xfff)), DR07);
}
static inline u_long BLTBYTEADDRESS(struct display *p, int x, int y)
{
    return (y * (p->var.xres) * ((p->var.bits_per_pixel)>>3) + x * ((p->var.bits_per_pixel)>>3));
}

#define ROP_COPY	0
#define ROP_OR		1
#define ROP_AND		2
#define ROP_XOR		3
#define ROP_INVERT	4

/* alu to C&T conversion for use with source data */
static unsigned int ctAluConv[] =
{
    0xCC,			/* ROP_COPY   : dest = src; GXcopy */
    0xEE,			/* ROP_OR     : dest |= src; GXor */
    0x88,			/* ROP_AND    : dest &= src; GXand */
    0x66,			/* ROP_XOR    : dest = ^src; GXxor */
    0x55,			/* ROP_INVERT : dest = ~dest; GXInvert */
};
/* alu to C&T conversion for use with pattern data */
static unsigned int ctAluConv2[] =
{
    0xF0,			/* ROP_COPY   : dest = src; GXcopy */
    0xFC,			/* ROP_OR     : dest |= src; GXor */
    0xA0,			/* ROP_AND    : dest &= src; GXand */
    0x5A,			/* ROP_XOR    : dest = ^src; GXxor */
    0x55,			/* ROP_INVERT : dest = ~dest; GXInvert */
};

static void ct48fb_acc_setup(struct display *p)
{
#ifdef FBCON_HAS_CFB8
    if (bpp==8)
	fbcon_cfb8_setup(p);
#endif
#ifdef FBCON_HAS_CFB16
    if (bpp==16)
	fbcon_cfb16_setup(p);
#endif
}

static void ct48fb_acc_bmove(struct display *p, int y1, int x1, int y2, int x2, int h, int w)
{
    u_int srcaddr, destaddr, op;
    u_int linew;

    x1     *= fontwidth(p);
    y1     *= fontheight(p);
    x2     *= fontwidth(p);
    y2     *= fontheight(p);
    w      *= fontwidth(p);
    h      *= fontheight(p);

    linew = p->var.xres * ((p->var.bits_per_pixel)>>3);

    srcaddr = BLTBYTEADDRESS(p, x1, y1);
    destaddr = BLTBYTEADDRESS(p, x2, y2);

    op = ctAluConv[ROP_COPY];
    if (x1 < x2) {
	op |= ctRIGHT2LEFT;
	srcaddr += w * ((p->var.bits_per_pixel)>>3) - 1;
	destaddr += w * ((p->var.bits_per_pixel)>>3) - 1;
    } else {
	op |= ctLEFT2RIGHT;
    }
    if (y1 < y2) {
	op |= ctBOTTOM2TOP;
	srcaddr += (h-1) * linew;
	destaddr += (h-1) * linew;
    } else {
	op |= ctTOP2BOTTOM;
    }

    ctBLTWAIT();
    ctSETROP(op);
    ctSETSRCADDR(srcaddr);
    ctSETDSTADDR(destaddr);
    ctSETPITCH(linew, linew);
    ctSETHEIGHTWIDTHGO(h, w * ((p->var.bits_per_pixel)>>3));
    ctBLTWAIT();
    wasbmove = 1;
}

static void ct48fb_acc_clear(struct vc_data *conp, struct display *p, int sy, int sx, int h, int w)
{
    u_int destaddr;
    u_int bgx;
    u_int linew;

    sx     *= fontwidth(p);
    sy     *= fontheight(p);
    w      *= fontwidth(p);
    h      *= fontheight(p);

    destaddr = BLTBYTEADDRESS(p, sx, sy);

    linew = p->var.xres * ((p->var.bits_per_pixel)>>3);

    ctBLTWAIT();
    ctSETDSTADDR(destaddr);
    if (p->var.bits_per_pixel == 8) {
	bgx=attr_bgcol_ec(p, conp);
	ctSETFGCOLOR(bgx); ctSETBGCOLOR(bgx);
    } else {
	bgx=((u16 *)p->dispsw_data)[attr_bgcol_ec(p, conp)];
	ctSETFGCOLOR16(bgx); ctSETBGCOLOR16(bgx);
    }
    ctSETROP(ctAluConv2[ROP_COPY] | ctTOP2BOTTOM | ctLEFT2RIGHT | ctPATSOLID | ctPATMONO);
    ctSETPITCH(0, linew);
    ctSETHEIGHTWIDTHGO(h, w * ((p->var.bits_per_pixel)>>3));
    ctBLTWAIT();
}

static void ct48fb_acc_putc(struct vc_data *conp, struct display *p, int c, int yy, int xx)
{
    u_int destaddr;
    u_int bgx, fgx;
    u_int linew,step;
    u_char *chardata;

    if (noaccputc || wasbmove) {
	wasbmove = 0;
#ifdef FBCON_HAS_CFB8
	if (bpp==8)
    	    fbcon_cfb8_putc(conp, p, c, yy, xx);
#endif
#ifdef FBCON_HAS_CFB16
	if (bpp==16)
	    fbcon_cfb16_putc(conp, p, c, yy, xx);
#endif
    } else {

	linew = p->var.xres * ((p->var.bits_per_pixel)>>3);

        xx *= fontwidth(p);
	yy *= fontheight(p);
	if (fontwidth(p) <= 8)
    	    step = 1;
	else if (fontwidth(p) <= 16)
	    step = 2;
	else
	    step = 4;

	destaddr = BLTBYTEADDRESS(p, xx, yy);
	chardata = p->fontdata+(c&p->charmask)*fontheight(p)*step;

	ctBLTWAIT();
	ctSETSRCADDR(0);
	ctSETDSTADDR(destaddr);
	if (p->var.bits_per_pixel == 8) {
	    bgx=attr_bgcol(p, c);
	    fgx=attr_fgcol(p, c);
	    ctSETFGCOLOR(fgx); ctSETBGCOLOR(bgx);
	} else {
	    bgx=((u16 *)p->dispsw_data)[attr_bgcol(p, c)];
	    fgx=((u16 *)p->dispsw_data)[attr_fgcol(p, c)];
	    ctSETFGCOLOR16(fgx); ctSETBGCOLOR16(bgx);
	}
	ctSETPITCH(0, linew);
	ctSETROP(ctAluConv[ROP_COPY] | ctSRCMONO | ctSRCSYSTEM | ctTOP2BOTTOM | ctLEFT2RIGHT);
	ctSETHEIGHTWIDTHGO(fontheight(p), step);
	fb_memmove(fb_info.fbmem_virt, chardata, fontheight(p)*step);
	ctBLTWAIT();
    }
}

static void ct48fb_acc_putcs(struct vc_data *conp, struct display *p, const unsigned short *s, int counter, int yy, int xx)
{
    ctBLTWAIT();
#ifdef FBCON_HAS_CFB8
    if (bpp==8)
	fbcon_cfb8_putcs(conp, p, s, counter, yy, xx);
#endif
#ifdef FBCON_HAS_CFB16
    if (bpp==16)
	fbcon_cfb16_putcs(conp, p, s, counter, yy, xx);
#endif
}

static void ct48fb_acc_revc(struct display *p, int xx, int yy)
{
    /* I don't give a shit about making an accelerated version of this
       as the only place where it is used is blinking software cursor */
    ctBLTWAIT();
#ifdef FBCON_HAS_CFB8
    if (bpp==8)
	fbcon_cfb8_revc(p, xx, yy);
#endif
#ifdef FBCON_HAS_CFB16
    if (bpp==16)
	fbcon_cfb16_revc(p, xx, yy);
#endif
}

static void ct48fb_acc_clear_margins(struct vc_data *conp, struct display *p, int bottom_only)
{
    ctBLTWAIT();
#ifdef FBCON_HAS_CFB8
    if (bpp==8)
	fbcon_cfb8_clear_margins(conp, p, bottom_only);
#endif
#ifdef FBCON_HAS_CFB16
    if (bpp==16)
	fbcon_cfb16_clear_margins(conp, p, bottom_only);
#endif
}

static void ct48fb_acc_cursor(struct display* p, int mode, int x, int y)
{
    struct ct48fb_info *fb = (struct ct48fb_info *)p->fb_info;

    if ((fontwidth(p) != fb->cursor.w)||(fontheight(p) != fb->cursor.h)) {
	fb->cursor.w = fontwidth(p);
	fb->cursor.h = fontheight(p);
	ct48fb_set_cursor_shape(fb);
    }

    x *= fontwidth(p);
    x = x & 0xFFFF;
    y = y*fontheight(p) - p->var.yoffset;

    if (y<0)
        y = (y & 0x7FFF) | 0x8000;
    else
	y = (y & 0x7FFF);

    if (fb->cursor.x == x && fb->cursor.y == y && (mode == CM_ERASE) == !fb->cursor.enable)
	return;

    fb->cursor.enable = 0;
    fb->cursor.x = x;
    fb->cursor.y = y;

    ctBLTWAIT();	/* need to wait... */

    /* set cursor position */
    outl((y<<16)+x, DR0B);

    switch (mode) {
	case CM_ERASE:
	    /* turn off cursor */
	    outl(0x00000020, DR08);
	    break;
	case CM_DRAW:
	case CM_MOVE:
	    /* turn on cursor */
	    if (noblink) {
		outl(0x00000021, DR08);
	    } else {
		outl(0x00008021, DR08);
	    }
	    fb->cursor.enable = 1;
	    break;
    }
}

static int ct48fb_acc_set_font(struct display* p, int w, int h)
{
    struct ct48fb_info *fb = (struct ct48fb_info *)p->fb_info;

    fb->cursor.w = fontwidth(p);
    fb->cursor.h = fontheight(p);

    ct48fb_set_cursor_shape(fb);

    return 1;
}

static void ct48fb_set_cursor_shape(struct ct48fb_info *p)
{
    u_char *dest;
    int w,h;
    int i;

    w = p->cursor.w;
    h = p->cursor.h;

    if (!w || !h) {
	w = 8;
	h = 8;
    }

    if (h>32)
	h=32;

    dest = (u_char*)(p->fbmem_virt+p->currentmode.cursor_base);

    ctBLTWAIT();	/* need to wait... */

    for (i=0;i<h;i++) {
      switch(w) {	/* XXX: this is probably endianess broken */
        case 8:      /* XXAAXXAA - 0-15 */
    	    fb_writel(0x00ffffff,dest);
	    break;
	case 12:
	    fb_writel(0xf0ffffff,dest);
	    break;
	case 14:
	    fb_writel(0xfcffffff,dest);
	    break;
	case 16:
	    fb_writel(0xffffffff,dest);
	    break;
	default:
	    fb_writel(0x00ff01ff,dest);
	}
      dest+=4;
      fb_writel(0x00ff00ff,dest); dest+=4;
    }
    for (;i<32;i++) {
        fb_writel(0x00ff00ff,dest); dest+=4;
	fb_writel(0x00ff00ff,dest); dest+=4;
    }
}

/* ------------------------------------------------------------------------- */

#ifdef MODULE
MODULE_LICENSE("GPL");
MODULE_SUPPORTED_DEVICE("fb");
MODULE_AUTHOR("(c) 2004 Maciej Witkowiak <ytm@elysium.pl>");
MODULE_DESCRIPTION("CT65548/45/40 framebuffer driver");
MODULE_PARM(noaccel,"i");
MODULE_PARM_DESC(noaccel, "Do not use accelerating engine (1=true, default=0)");
MODULE_PARM(noaccputc,"i");
MODULE_PARM_DESC(noaccputc, "Do not use accelerated putc (1=true, default=1)");
MODULE_PARM(nohwcursor,"i");
MODULE_PARM_DESC(nohwcursor, "Do not use hardware cursor (1=true, default=0)");
MODULE_PARM(noblink,"i");
MODULE_PARM_DESC(noblink, "Do not blink hardware cursor (1=true, default=1)");
MODULE_PARM(noinverse,"i");
MODULE_PARM_DESC(noinverse, "Do not inverse the screen (1=true, default=1)");
MODULE_PARM(mode,"s");
MODULE_PARM_DESC(mode, "Selected primary video mode");
MODULE_DEVICE_TABLE(pci,ct_devices);

int init_module(void)
{
    if (mode) ct48fb_mode_setup(mode);
    return ct48fb_init();
}

void cleanup_module(void)
{
    ct48fb_cleanup(NULL);
}

#endif /* MODULE */
