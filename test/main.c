#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <debug.h>
#include <xenos/xenos.h>
#include <diskio/ata.h>
#include <input/input.h>
#include <console/console.h>
#include <diskio/disc_io.h>
#include <sys/iosupport.h>
#include <usb/usbmain.h>
#include <time/time.h>
#include <ppc/timebase.h>
#include <xenon_soc/xenon_power.h>
#include <elf/elf.h>
#include <dirent.h>

#include <libfat/fat.h>

#include <libcaps/lib.h>

int main()
{
	xenos_init(VIDEO_MODE_VGA_640x480);
	console_init();

	xenon_make_it_faster(XENON_SPEED_FULL);

	usb_init();
	usb_do_poll();

	xenon_ata_init();

	xenon_atapi_init();

	fatInitDefault();
	
	xenon_caps_init("uda:/video.avi");
	xenon_caps_start();
	
	int i = 20;
	while(i>0) {
		printf("I'm happy and you ?\n");
		delay(1);
		printf("Kick Ass !!\n");
		delay(1);
		printf("Real Time encoding !!!\n");
		i--;
	}
	
	xenon_caps_end();
	
	return 0;
}
