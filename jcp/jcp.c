/* jcp.cpp : Move data to Jaguar by way of EZ-HOST RDB connection
	Skunkboard support - http://harmlesslion.com

Design:
	Data is copied at peak speed via RDB into two buffers in EZ memory (at $2800 and $1800)
	The 68K polls EZ memory for new buffers, copying them into Jaguar memory as they appear

Memory map relative to base address $2800:
	$37E0:	Base address (in 68K memory) for this block
	$37E4:	Address where 68K execution begins once this block is complete
			-1 if more blocks will follow (i.e., stay in the reader loop)
	$37E8:	Base address of next block in EZ-HOST (usually $1800)
	$37EA:	Amount (in bytes) to copy (up to 4064 bytes)
			-1 if the block is not ready for copying
			The 68K sets this at $2800 and $1800 before transfer begins
			After each block is moved, the 68K sets this to -1 again. Some
			sequences are bi-directional.

	Note:   the values FxFF, excluding FFFF, are reserved as flag values
			for future, incompatible versions of JCP, to allow detection.

	OTG has 16k of internal RAM:
	0000 - 04A3 - Interrupt vectors, HW Registers and USB buffers
	04A4 - 0FEF - Free, reserved for GDB
	0FF0 - 102B - turbow (accelerated USB block copy stub)
	102C - 17FF - Free
	1800 - 27FF - Transmit buffer 1
	2800 - 37FF - Transmit buffer 2
	3800 - 3FFF - Free
	4000 - BFFF - external memory (not implemented)
	C000 - C0FF - control registers
	C100 - DFFF - external memory (not implemented)
	E000 - FFFF - EZ-Host ROM BIOS

SB Rev 2:
	Special flags are added to indicate bank instructions:

	During flash, bits are added to the erase block count:
	0x80000000;		// set high bit to flag it for slow (word) write - doesn't require 9v (on SB rev 1 too)
	0x40000000;		// set next bit to flag bank 2 (instead of bank 1)

	During boot, bits are added to the start address:
	0x10000000;		// set bank 2 (instead of bank 1)
	0x70000000;		// set 6MB mode (signed, avoid high bit)

Observations:
	 The X command seems to have a limit of 4080 bytes per transfer
		This applies even with escaping!  (Which makes 9120 bytes...)
	 Roundtrips hurt -- compare 10 seconds/megabyte @ 4080 to 13 seconds/megabyte @ 2048
	 We currently use 'middle endian' because the CPLD does not byteswap 'data regions'

Lots and lots of tweaks by Tursi, sorry, not all documented, though I've updated what
I changed above.

This program and associated binaries are copyright by Mike Brent, http://harmlesslion.com
Commercial use prohibited. All rights reserved. Copyright 2009 by Mike Brent.

linux link with -lusb and -lrt - requires libusb to be installed!
Apple link with only -lusb

May require root priviledges to see the device.

Thanks to Belboz for the OSX patch!
Thanks to SebRmv for ideas/fixes in the Skunklib support code

*/

/* if you don't want to include the BIOS upgrades, comment this out */
/* the upgrade for rev 3 boards also works fine on rev 2 */
#define INCLUDE_BIOS_10204
#define INCLUDE_BIOS_30002
/* uncomment this to try automatic mode (experimental) */
// JCP_AUTO doesn't work too well at the moment.. need to rearchitect.
// should detect the filetype FIRST, then decide how to upload it. Too
// hacky trying to guess first.
//#define JCP_AUTO

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifndef __APPLE__
#include <malloc.h>
#endif
#include <errno.h>
#include <time.h>
#ifdef WIN32
#include <process.h>
#include "winusb.h"
#else
#include <unistd.h>
#include <usb.h>
#include <sys/time.h>
#endif
#include "turbow.h"
#include "univbin.h"
#include "romdump.h"
#include "flashstub.h"
#include "dumpver.h"
#include "readver.h"
#include "e2pget.h"
#include "e2pput.h"
#ifdef INCLUDE_BIOS_10204
#include "upgrade10204.h"
#endif
#ifdef INCLUDE_BIOS_30002
#include "upgrade30002.h"
#endif

#ifdef REMOVERS
#pragma message "Enabling Removers extensions."
#include "jcp_handler.h"
#endif

/* version major.minor.rev */
#define JCPVERSION 0x020402
/* ROM based address that we can blindly send dummy data to */
#define DUMMYBASE 0xFFE000
/* size of the work buffer (maximum ROM size plus slack) */
#define BUFSIZE (6*1024*1024+0x2000)
/* size of a RAM buffer (for ELF file loading) */
#define RAMBUFSIZE (2*1024*1024)

#define	ENBIGEND(_x) (((_x)[0] << 24) + ((_x)[1] << 16) + ((_x)[2] << 8) + (_x)[3])
#define	HALFBIGEND(_x) (((_x)[0] << 8) + (_x)[1])
#define	ENMIDEND(_x) (((_x)[0] << 16) + ((_x)[1] << 24) + ((_x)[2]) + ((_x)[3] << 8) )
#define	HALFLITTLEEND(_x) (((_x)[0]) + ((_x)[1] << 8) )

/* data for the SerialBig function */
char *szNumDat[16] = {
	" XXX  ",
	"X   X ",
	"  X   ",
	"XXX   ",
	"XXXXX ",
	"    X ",
	"  XX  ",
	" X    ",
	"X     ",
	"  XX  ",
	"X  X  ",
	"   X  ",
	" XXX  ",
	"XXXX  ",
	" XXXX ",
    "      ",
};
int nDigits[12][7] = {
                      { 0,1,1,1,1,1,0 },
                      { 2,3,2,2,2,2,4 },
                      { 12,1,5,6,7,8,4 },
                      { 0,1,5,9,5,1,0 },
                      { 8,10,10,4,11,11,11 },
                      { 4,8,8,0,5,1,0 },
                      { 0,8,8,13,1,1,0 },
                      { 4,5,5,11,2,2,2 },
                      { 0,1,1,0,1,1,0 },
                      { 0,1,1,14,5,5,0 },
                      // space, accessed as -1
                      { 15,15,15,15,15,15,15 },
                      // period, accessed as -2
                      { 15,15,15,15,15,15,6 }
};

#ifndef uchar
#define uchar unsigned char
#endif
#ifndef bool
#define bool int
#endif
#ifndef true
#define true 1
#endif
#ifndef false
#define false 0
#endif

#ifndef WIN32
/* linux compatibility with Windows terms */
#define DWORD unsigned int
#define _stricmp strcasecmp
#define Sleep(x) usleep(x*1000)
#define _execlp execlp

/* returns a count in ms - this one should be osx compat */
DWORD GetTickCount() {
	struct timeval now;
	DWORD ret;

	int rv = gettimeofday(&now, NULL);

	if (rv != 0) {
		return 0;	/* we got nothing */
	}

	ret=now.tv_sec*1000+(now.tv_usec/1000);
	return ret;
}
#endif

usb_dev_handle* findEZ(bool fInstallTurboW, bool fAbortOnFail);
void Reattach();
void bye(char* msg);
void SendFile(int flen, uchar *fptr, int curbase, int base);
int  DoFile(uchar *fdata, int base, int flen, int skip, bool builtin);
void LockBothBuffers();
bool TestIfBuffersLocked();
void WaitForBothBuffers();
void DoResetAndReconnect(bool bForce);
void DoResetAndBoot();
void DoFlash(int nLen);
void DoDump(char *pszName);
void DoE2pDump(char *pszName);
void DoE2pWrite(char *pszName);
void DoSerialInfo();
void DoSerialBig();
void DoBiosUpdate();
void DoE2pSelect(int s);
void DoReset();
void HandleConsole();
void FilenameSanitize(char *buf);
int ParseAddress(const char *pBuf);
int HandleTransfer(uchar *fdata, int base, int flen, int skip, bool part2of6mb);
void CatVal(char *szOut, int nBufLen, int nVal, int nRow);
bool DetermineFileInfo(bool bMute, uchar *fdata, int *base, int *flen, int *skip);

/* globals */
int nextez = 0x1800;
usb_dev_handle* udev = NULL;
uchar *fdata = NULL;
char g_szFilename[256];
FILE *fp=NULL;
bool g_FirstFileSent=false;
char *g_pszExtShell;

bool g_OptDoFlash=false;
bool g_OptDoSlowFlash=false;
bool g_OptOverrideFlash=false;
bool g_OptEraseAllBlocks=false;
bool g_OptDoDump=false;
bool g_OptDoE2pDump=false;
bool g_OptDoE2pWrite=false;
bool g_OptFlashActive=false;
bool g_OptNoBoot=false;
bool g_OptOnlyBoot=false;
bool g_OptOverride=false;
bool g_OptConsole=false;
bool g_OptOnlyConsole=false;
bool g_OptConsoleUp=false;	/* set to true when console actually starts */
bool g_OptSilentConsole=false;
bool g_OptVerbose=false;
bool g_OptAutoMode=false;   /* used to enable JCP auto mode - this is experimental */
int nCartBank=0;			/* 0=bank 1, 1=bank 2, -1=6MB mode (latter 2 not on SB rev 1) */
bool g_SixMegWrite=false;	/* used to help systems that need to know when it's 6mb! */
int  g_HeaderSkip=0;		/* number of bytes to skip in header - manual set */
bool g_OptQuietMode=false;  /* Quiet mode for skunkGUI. no spin and extra text output*/
bool g_skipwait=false;		/* used during file transfer to skip waiting for boot signature */

/* Main function - entry point */
int main(int argc, char* argv[])
{
	int base, flen, skip;
	int nFileArg, nBaseArg, nArg;
	int nUsed;

#ifdef REMOVERS
        printf("rmvjcp v%02X.%02X.%02X built on %s\n\n", ((JCPVERSION&0xFF0000)>>16), ((JCPVERSION&0xFF00)>>8), (JCPVERSION&0xFF), __DATE__);
#else
	printf("jcp v%02X.%02X.%02X built on %s\n\n", ((JCPVERSION&0xFF0000)>>16), ((JCPVERSION&0xFF00)>>8), (JCPVERSION&0xFF), __DATE__);
#endif

	if ((argc<2) || ((argc>1)&&(strchr(argv[1],'?')))) {
		bye("Usage: jcp [-k <0|1|2>] | [-rewfnbocdgpsux] file [$base]\n"	\
			"  -r = reset (no other args needed)\n"			\
			"  -f = flash (pass filename + opt base)\n"		\
			"  -wf= word flash (slow, only if 'f' alone fails)\n"	\
			"  -ef= erase whole flash\n"		\
			"  -k <0|1|2> = Select Serial EEPROM (0=none,1=128B,2=2048B)\n"	\
			"  -n = no boot (pass filename + opt base)\n"	\
			"  -b = boot address (pass base only)\n"		\
			"  -o = override address (pass filename and base)\n" \
			"  -h (cnt) = override the header skip count\n" \
			"  -c = launch console (incompatible with n)\n"	\
			"  -d = dump flash (pass filename)\n" \
			"  -g = dump serial EEPROM (pass filename)\n" \
			"  -p = write serial EEPROM (pass filename)\n" \
			"  -s = get board version and serial number\n" \
			"  -u = upgrade board bios (if available)\n" \
			"  -!u= old upgrade - skip detection and update rev 1\n" \
			"  -fu= force upgrade - upgrade board even if version matches\n" \
			"  -2 = use bank 2 instead of bank 1 (sb2)\n" \
			"  -6 = use 6MB mode instead of banked (sb2)\n" \
			"  -x (extconsole.exe) = shell to external console application\n" \
			"  -q = quiet mode (useful for SkunkGUIs)\n" \
			""
		);
	}

	// Some basic initialization
	fdata = (uchar*)malloc(BUFSIZE);	// 6MB + header
	memset(fdata, 0, BUFSIZE);
	base = 0x4000;
	flen = 0;
	skip = 0;
	strcpy(g_szFilename, "");
	nFileArg=1;
	nBaseArg=2;
	g_pszExtShell=NULL;

#ifdef JCP_AUTO
	g_OptAutoMode=true;
#endif

	nArg=1;
	while (argv[nArg][0] == '-') {
		int nPos;
		int fExitLoop;

		nFileArg++;
		nBaseArg++;
		nPos=1;
		fExitLoop=0;
		while ((argv[nArg][nPos]) && (0 == fExitLoop)) {
			switch (tolower(argv[nArg][nPos])) {
				case 'q': g_OptQuietMode=true; break;
				case 'v': g_OptVerbose=true; break;
				case 'f': g_OptDoFlash=true; base=0x802000; break;
				case 'w': g_OptDoSlowFlash=true; break;		// 'w'ord writes
				case 'e': g_OptEraseAllBlocks=true; break;
				case 'd': g_OptDoDump=true; break;
				case 'g': g_OptDoE2pDump=true; break;
				case 'p': g_OptDoE2pWrite=true; break;
				case 'r': DoReset(); bye(""); break;
				case 'n': g_OptNoBoot=true;	break;
				case 'b': g_OptOnlyBoot=true; break;
				case 'o': g_OptOverride=true; break;
				case 'c': g_OptConsole=true; break;
				case 's': DoSerialInfo(); bye(""); break;
				case 'u': DoBiosUpdate(); Sleep(100); DoReset(); bye(""); break;
				case '!': g_OptOverrideFlash=true; break;	// undocumented! Don't require a flash even if in flash range
				case '*': DoSerialBig(); bye(""); break;	// undocumented! banner serial! Used in test script. :)
				case '2': nCartBank=1; printf("Using bank 2\n"); break;
				case '6': nCartBank=-1; g_SixMegWrite=true; printf("Using 6MB flash mode\n"); break;
				case 'h':
					// get the forced header offset
					if (nArg+1 < argc) {
						nArg++;
						g_HeaderSkip = atoi(argv[nArg]);
						nFileArg++;
						nBaseArg++;
						fExitLoop=1;
					} else {
						bye("-h option requires the number of bytes to skip!");
					}
					break;

				case 'x':
					if (argv[nArg][nPos+1]) {
						bye("-x option requires external shell filename to follow");
					}
					g_OptConsole=true;
					nFileArg++;
					nBaseArg++;
					g_pszExtShell=argv[++nArg];
					fExitLoop=1;
					break;

				case 'k':
					// Apply the EEPROM selection
					if (nArg+1 < argc) {
						DoE2pSelect(atoi(argv[nArg+1]));
						bye("");
					} else {
						bye("-k option requires a serial EEPROM selection!");
					}
					break;

				default: bye("Unknown option");
			}
			nPos++;
		}
		nArg++;
		if (nArg >= argc) {
			break;
		}
	}

	if (argc > nBaseArg) {
		base = ParseAddress(argv[nBaseArg]);
	}

	if ((g_OptOnlyBoot) && (argc > nFileArg)) {
		base = ParseAddress(argv[nFileArg]);
	}

	if (!g_OptOnlyBoot) {
		if ((nFileArg >= argc) && (!g_OptConsole)) {
			bye("No filename was specified");
		} else {
			if (nFileArg >= argc) {
				// user seems to want to try to attach the console, so don't try to load stuff
				if (!g_OptOnlyBoot) {
					g_OptOnlyConsole=true;
				}
			} else {
				strncpy(g_szFilename, argv[nFileArg], 256);
				g_szFilename[255]='\0';
				if (!g_OptDoDump && !g_OptDoE2pDump && !g_OptDoE2pWrite) {
					FILE *fp = fopen(g_szFilename, "rb");
					if (NULL == fp || (flen = (int)fread(fdata, 1, BUFSIZE, fp)) < 1) {
						bye("Couldn't read file");
					}
					fclose(fp);
					fp=NULL;
				} else {
					flen=0;
				}
			}
		}
	}

	if (g_OptDoDump) {
		DoDump(g_szFilename);
		bye("Dump complete.");
	}

	if (g_OptDoE2pDump) {
		DoE2pDump(g_szFilename);
		bye("Serial EERPOM Dump complete.");
	}

	if (g_OptDoE2pWrite) {
		DoE2pWrite(g_szFilename);
		bye("Serial EERPOM Write complete.");
	}

	// Bit of a hack, preparse the file to figure out its true length and address
	DetermineFileInfo(false, fdata, &base, &flen, &skip);

	if ((nCartBank == -1) && (flen <= 4*1024*1024-0x2000) && (!g_OptOnlyBoot)) {
		printf("6MB mode not required, will flash bank 1\n");
		nCartBank=0;
	}

	if ((g_OptAutoMode) && (nCartBank != -1) && (flen > 4*1024*1024)) {
		printf("Assuming 6MB mode...\n");
		nCartBank=-1;
	}

	if (g_OptDoFlash) {
		if (flen == 0) {
			bye("File must be specified with flash!");
		}
		if (g_OptNoBoot) {
			printf("- NoBoot option not supported during flashing\n");
			g_OptNoBoot=false;
		}
		// this is an estimate, headers may make it possible, or the 2k bios
		// gap may make it impossible!
		if ((flen > (4*1024*1024)) && (-1 != nCartBank)) {
			bye("File is too large to be flashed to a 4MB bank, try 6MB mode\n");
		}
	}

	// we handle 6MB mode as two separate uploads, since we can't run it directly
	if (nCartBank == -1) {
		if (!g_OptOnlyBoot) {
			bool bOldConsole = g_OptConsole;
			// tweak things up a bit to make this work
			printf("6MB mode will require two transfers...\n");
			nCartBank=0;
			g_OptNoBoot=true;
			g_OptConsole=false;
			nUsed=HandleTransfer(fdata, base, flen, skip, false);
			WaitForBothBuffers();

			printf("Flashing second bank...\n");
			nCartBank=1;
			g_OptConsole=bOldConsole;
			g_OptFlashActive = false;	// this is necessary because we have to load the flasher stub again
			HandleTransfer(fdata+nUsed, 0x800000, flen-nUsed, 0, true);
			WaitForBothBuffers();
		}

		printf("Requesting start...\n");
		g_OptNoBoot=false;
		g_OptOnlyBoot=true;
		nCartBank=-1;
		DoFile(fdata, base, 0, 0, true);
	} else {
		HandleTransfer(fdata, base, flen, skip, false);
	}

	free(fdata);
	fdata=NULL;
	if (NULL != udev) {
		usb_close(udev);
	}

	return 0;
}

/* handles the transfer and the flash portion */
/* returns the number of bytes actually processed (not necessarily sent, includes headers) */
int HandleTransfer(uchar *fdata, int base, int flen, int skip, bool part2of6mb) {
	if (g_OptDoFlash) {
		bool bOldNoBoot=g_OptNoBoot;	// loading the flash program ALWAYS requires NoBoot to be false
		g_OptNoBoot=false;

		DoFlash(flen);

		g_OptNoBoot=bOldNoBoot;
		g_OptFlashActive=true;
	}

	return DoFile(fdata, base, flen, skip, part2of6mb);
}

/* Generate a little text spinner */
void Spin() {
	static int nSpinner=0;
	static char szSpin[]="\\|/-";

	if(!g_OptQuietMode)
    {
		nSpinner++;
		if (szSpin[nSpinner] == '\0') nSpinner=0;
		putchar(szSpin[nSpinner]);
		putchar('\b');
		fflush(stdout);
	}
}

/* reset the Jaguar then reconnect to it - pass true not to wait on the buffers */
void DoResetAndReconnect(bool bForce) {
	if (!bForce) {
		WaitForBothBuffers();	// make sure the Jag is done the last command
	}

	DoReset();
	Sleep(2000);			// takes the Jag about 2s to come up

	while (NULL == udev) {
		Sleep(100);
		udev=findEZ(true, false);
	}

	WaitForBothBuffers();	// when the Jag clears the buffers, we're up

	// reset pointer
	nextez = 0x1800;
}

/* synch reset the Jag, then send a start packet for the cart */
void DoResetAndBoot() {
	DWORD tmp=0;

	DoResetAndReconnect(false);

	// send the fake boot command
	g_OptOnlyBoot=true;
	g_OptFlashActive=false;

	DoFile((uchar*)&tmp, 0x802000, 0, 0, true);
}

/* mark both buffers as blocked - don't use during upload! */
void LockBothBuffers() {
	unsigned short tmp;

	tmp=0;

	for (;;) {
		if (usb_control_msg(udev, 0x40, 0xfe, 4080, 0x1800+0xFEA, (char*)&tmp, 2, 1000) == 2) {
			break;
		}
		Reattach();
	}
	for (;;) {
		if (usb_control_msg(udev, 0x40, 0xfe, 4080, 0x2800+0xFEA, (char*)&tmp, 2, 1000) == 2) {
			break;
		}
		Reattach();
	}
}

/* returns true if both buffers are locked */
/* Note: locked diffs from 'in-use' in that locked uses a length of 0 */
bool TestIfBuffersLocked() {
	volatile short poll=0;
	bool bRet=false;

	if ((usb_control_msg(udev, 0xC0, 0xff, 4, 0x1800+0xFEA, (char*)&poll, 2, 1000) == 2)) {
		if (poll == 0) {
			if ((usb_control_msg(udev, 0xC0, 0xff, 4, 0x2800+0xFEA, (char*)&poll, 2, 1000) == 2)) {
				if (poll == 0) {
					bRet=true;
				}
			} else {
				bRet=true;	// say locked for now
				Reattach();
			}
		}
	} else {
		bRet=true;	// say locked for now
		Reattach();
	}

	return bRet;
}

/* wait for the Jag to mark both buffers as free */
void WaitForBothBuffers() {
	volatile short poll=0;

	// first buffer
	do {
		Spin();
		if ((usb_control_msg(udev, 0xC0, 0xff, 4, 0x1800+0xFEA, (char*)&poll, 2, 1000) != 2)) {
			Reattach();
		} else {
			Sleep(100);
		}
	} while (-1 != poll);
	printf(".");

	// second buffer
	do {
		Spin();
		if ((usb_control_msg(udev, 0xC0, 0xff, 4, 0x2800+0xFEA, (char*)&poll, 2, 1000) != 2)) {
			Reattach();
		} else {
			Sleep(100);
		}
	} while (-1 != poll);
	printf(".\n");
}

/* Reset the Jaguar */
void DoReset() {
	// We have to use scan mode to access registers (TurboWrite uses DMA engine)
	unsigned char cmd[10] = {0xB6, 0xC3, 0x04, 0x00, 0x00, 0x28, 0xC0, 0x02, 0x00, 0x00};

	if ( (g_OptVerbose) || (!g_OptSilentConsole) ) {
		printf("Resetting jaguar...\n");
	}

	// Reset is 0xc028=2, 0xc028=0
	if (NULL == udev) {
		udev = findEZ(true, true);		// we used to not load turbow here, but for better reconnect, we want to lock buffers first!
	}

	LockBothBuffers();		// carries through the reset

	if (usb_control_msg(udev, 0x40, 0xff, 10, 0x304C, (char*)cmd, 10, 1000) != 10) {
		bye("Reset assert failed to send.");
	}

	// brief delay!
	Sleep(50);

	cmd[7] = 0;
	if (usb_control_msg(udev, 0x40, 0xff, 10, 0x304C, (char*)cmd, 10, 1000) != 10) {
		bye("Reset release failed to send.");
	}

	/* in case it's used elsewhere */
	if (NULL != udev) {
		usb_close(udev);
	}
	udev=NULL;
}

/* Enable one or none of the Serial EEPROMs */
void DoE2pSelect(int s) {
	// EZHost GPIO register memory map:
	//
	// 0xC006 - GPIO control register
	// 0xC01E - GPIO0 output data register
	// 0xC020 - GPIO0 input data register
	// 0xC022 - GPIO0 direction register (0 = in, 1 = out)
	// 0xC024 - GPIO1 output data register
	// 0xC026 - GPIO1 input data register
	// 0xC028 - GPIO1 direction register (0 = in, 1 = out)
	//
	// Additionally, note the UART, which uses GPIO28, is enabled by
	// default. Need to disable it by setting the UART control register
	// bit 0 to zero.
	//
	// 0xC0E0 - UART control register (bit 0 = 0 to disable)
	//
	// Chip select AND lines are on GPIO25 and GPIO28, which are in GPIO
	// "bank" 1, so we'll use the GPIO1 registers. Note these are also
	// used by the reset function, which is wired to GPIO17 and clobbers
	// the entire GPIO1 direction register. However, that's fine; It just
	// means EEPROM selection won't survive a skunk-initiated reset. I
	// didn't find any other writes to the GPIO registers in JCP or the
	// skunk BIOS or library files, so other than a reset or power cycle,
	// the EZHost GPIO pin state should persist.

	// Disable UART
	unsigned char cmdUARTDis[10] = {0xB6, 0xC3, 0x04, 0x00, 0x00, 0xE0, 0xC0, 0x00, 0x00, 0x00};

	// [0] == disable both, [1] == enable EEPROM 1, [2] == enable EEPROM 2
	unsigned char cmdS[3][10] = {
		{0xB6, 0xC3, 0x04, 0x00, 0x00, 0x24, 0xC0, 0x00, 0x00, 0x00},
		{0xB6, 0xC3, 0x04, 0x00, 0x00, 0x24, 0xC0, 0x00, 0x10, 0x00},
		{0xB6, 0xC3, 0x04, 0x00, 0x00, 0x24, 0xC0, 0x00, 0x02, 0x00},
	};
	// Configure GPIO25 and GPIO28 as outputs
	unsigned char cmdOutEn[10] = {0xB6, 0xC3, 0x04, 0x00, 0x00, 0x28, 0xC0, 0x00, 0x12, 0x00};

	if ( s > 2 || s < 0) {
		printf("Invalid serial EEPROM setting: %d. Valid values: 0-2\n", s);
		return;
	}

	if ( (g_OptVerbose) || (!g_OptSilentConsole) ) {
		const char *e2sName[] = {
			"neither",
			"93C46 (128 byte)",
			"93C86 (2048 byte)",
		};
		printf("Selecting %s serial EEPROM...\n", e2sName[s]);
	}

	if (NULL == udev) {
		udev = findEZ(true, true);
	}

	// Disable the UART
	if (usb_control_msg(udev, 0x40, 0xff, 10, 0x304C, (char*)cmdUARTDis, 10, 1000) != 10) {
		bye("UART disable failed to send.");
	}

	// Pre-set the GPIO output values
	if (usb_control_msg(udev, 0x40, 0xff, 10, 0x304C, (char*)cmdS[s], 10, 1000) != 10) {
		bye("EEPROM select failed to send.");
	}

	// Configure GPIO 25 and 28 as outputs
	if (usb_control_msg(udev, 0x40, 0xff, 10, 0x304C, (char*)cmdOutEn, 10, 1000) != 10) {
		bye("GPIO output enable failed to send.");
	}

	/* in case it's used elsewhere */
	if (NULL != udev) {
		usb_close(udev);
	}
	udev=NULL;
}

/* Prepare the Jaguar to receive a flash file */
void DoFlash(int nLen) {
	volatile short poll=0;
	int idx;
	unsigned int nBlocks;

	g_OptSilentConsole=true;

	// due to the flash layout, we can't do a straight sector erase
	// we compromise some and erase only half if we don't need it
	// all - that seems to work okay.
	if (nLen <= 2*1024*1024) {
		nBlocks = 32;
	} else {
		nBlocks = 62;
	}
	// just in case it's needed
	if (g_OptEraseAllBlocks) {
		nBlocks=62;
	}

	// restrict to the legal range
	if (nBlocks > 62) nBlocks=62;
	if (nBlocks < 1) nBlocks=1;

	if (g_OptVerbose) {
		printf("Going to erase %d blocks\n", nBlocks);
	}

	if (g_OptDoSlowFlash) {
		printf("Using slow flash (experimental...)\n");
		nBlocks|=0x80000000;	// set high bit to flag it
	}
	if (nCartBank == 1) {
		nBlocks|=0x40000000;	// set next bit to flag bank 2
	}

	{
		// FLASHSTUB needs to be non-const for this to work
		static int nFlashStubOffset=-1;

		if (-1 == nFlashStubOffset) {
			for (idx=0; idx<SIZE_OF_FLASHSTUB; idx++) {
				if ((FLASHSTUB[idx]==0x0a) && (FLASHSTUB[idx+1]==0xbc) && (FLASHSTUB[idx+2]==0xde) && (FLASHSTUB[idx+3]==0xf0)) {
					nFlashStubOffset=idx;
					break;
				}
			}
		}

		if (-1 == nFlashStubOffset) {
			bye("Failed to find signature - internal error.");
		}

		FLASHSTUB[nFlashStubOffset]=(nBlocks&0xff000000)>>24;
		FLASHSTUB[nFlashStubOffset+1]=(nBlocks&0xff0000)>>16;
		FLASHSTUB[nFlashStubOffset+2]=(nBlocks&0xff00)>>8;
		FLASHSTUB[nFlashStubOffset+3]=(nBlocks&0xff);
	}

	DoFile((uchar*)FLASHSTUB, 0x4100, SIZE_OF_FLASHSTUB, 168, true);

	// Don't scan for the buffers to be ready till they are zeroed, indicates start of flash
	// first buffer
	do {
		Spin();
		if ((usb_control_msg(udev, 0xC0, 0xff, 4, 0x1800+0xFEA, (char*)&poll, 2, 1000) != 2)) {
			Reattach();
		} else {
			Sleep(100);
		}
	} while (0 != poll);
	printf(".");

	// second buffer
	do {
		Spin();
		if ((usb_control_msg(udev, 0xC0, 0xff, 4, 0x2800+0xFEA, (char*)&poll, 2, 1000) != 2)) {
			Reattach();
		} else {
			Sleep(100);
		}
	} while (0 != poll);
	printf(".\n");

	// blocks are 64k each, and each takes about 300ms (somewhat less) to erase
	printf("Waiting for erase to complete (about %ds)", ((nBlocks+1)*300)/1000);

	// Both buffers will be marked ready when the
	// Jag is ready to proceed.

	WaitForBothBuffers();

	g_OptSilentConsole=false;

	// reset pointer
	nextez = 0x1800;
}

/* Request the Jaguar to dump the flash (uses the console to collect it) */
void DoDump(char *pszName) {
	int nCnt, nRes;
	DWORD nEnd,nStart=GetTickCount();

	// First we need to open the file and write the universal header to it
	fp=fopen(pszName, "wb");
	if (NULL == fp) {
		bye("Can't open output file!");
	}
	nCnt=(int)fwrite(univbin, 1, sizeof(univbin), fp);
	// binary doesn't include the useless 0xff padding to 8k, so do that here
	// except, there is a tiny block at 0x400 we need to fill in with standard
	// values.
	while (nCnt < 0x400) {
		fputc(0xff, fp);
		nCnt++;
	}
	// standard values - 32-bit cart, start at 0x802000, show logo
	fputc(0x04, fp);
	fputc(0x04, fp);
	fputc(0x04, fp);
	fputc(0x04, fp);
	fputc(0x00, fp);
	fputc(0x80, fp);
	fputc(0x20, fp);
	fputc(0x00, fp);
	fputc(0x00, fp);
	fputc(0x00, fp);
	fputc(0x00, fp);
	fputc(0x00, fp);
	nCnt+=12;

	// now the rest of the padding
	while (nCnt < 8192) {
		fputc(0xff, fp);
		nCnt++;
	}

	// This will make DoFile shell out to the console before returning
	g_OptConsole=true;
	g_OptSilentConsole=true;

	// we hack ROMDUMP to set the second bank, if needed. This requires ROMDUMP
	// not to be const!
	if (nCartBank == 1) {
		ROMDUMP[0xab]=1;
	}

	printf("Beginning dump to '%s'...\n", pszName);
	DoFile((uchar*)ROMDUMP, 0x10000, SIZE_OF_ROMDUMP, 168, true);

	nEnd=GetTickCount();
	nRes=(nEnd-nStart)/1000;
	if (nRes > 0) {
		printf(" \nDumped 4MB in %ds - %dKB/s\n", nRes, (4*1024-8)/nRes);
	} else {
		printf("Dump time <1s\n");
	}
}

/* Request the Jaguar to dump the serial EEPROM (uses the console to collect it) */
void DoE2pDump(char *pszName) {
	int nRes;
	DWORD nEnd,nStart=GetTickCount();

	// Open the file rather than making the Jaguar dump program do it using
	// skunklib. This avoids the need to send the filename to the Jaguar
	// just so it can send it back, and allows opening files in other
	// directories, unlike skunklib.
	fp=fopen(pszName, "wb");
	if (NULL == fp) {
		bye("Can't open output file!");
	}

	// This will make DoFile shell out to the console before returning
	g_OptConsole=true;
	g_OptSilentConsole=true;

	printf("Beginning dump serial EEPROM to '%s'...\n", pszName);
	DoFile((uchar*)e2pget, 0x4000, SIZE_OF_E2PGET, 168, true);

	nEnd=GetTickCount();
	nRes=nEnd-nStart;
	if (nRes > 0) {
		printf(" \nDumped 128B in %dms - %0.3fKB/s\n", nRes,
		       (128.0 / 1024.0) / (nRes / 1000.0));
	} else {
		printf("Dump time <1ms\n");
	}
}

/* Write the specified file to the serial EEPROM on Jaguar. (uses the console to send it) */
void DoE2pWrite(char *pszName) {
	int nRes;
	DWORD nEnd,nStart=GetTickCount();

	// Open the file rather than making the Jaguar write program do it using
	// skunklib. This avoids the need to send the filename to the Jaguar
	// just so it can send it back, and allows opening files in other
	// directories, unlike skunklib.
	fp=fopen(pszName, "rb");
	if (NULL == fp) {
		bye("Can't open input file!");
	}

	// This will make DoFile shell out to the console before returning
	g_OptConsole=true;
	g_OptSilentConsole=true;

	printf("Beginning write of '%s' to serial EEPROM...\n", pszName);
	DoFile((uchar*)e2pput, 0x4000, SIZE_OF_E2PPUT, 168, true);

	nEnd=GetTickCount();
	nRes=nEnd-nStart;
	if (nRes > 0) {
		printf(" \nWrote 128B in %dms - %0.3fKB/s\n", nRes,
		       (128.0 / 1024.0) / (nRes / 1000.0));
	} else {
		printf("Write time <1ms\n");
	}
}

/* Request the Jaguar to print out serial number (uses the console to collect it) */
void DoSerialInfo() {
	unsigned char SerBuf[12];
	unsigned short poll;
    DWORD curtime,endtime;

	// Open socket to Jaguar
	if (NULL == udev) {
		udev = findEZ(true, true);
	}

	// On the newer boards, we can get this information without uploading a program, so try that first
	// First, make sure that the $2800 buffer is marked as ready
	endtime = GetTickCount() + 2000;

	do {
		Spin();

		if ((usb_control_msg(udev, 0xC0, 0xff, 4, 0x2800+0xFEA, (char*)&poll, 2, 1000) != 2)) {
			Reattach();
		}
//			printf("Polled 0x%08x\n", poll);
		// poll is unsigned. But a value over 0xf0xx is reserved for future use
		// and we can't count on the lower bytes to be correct since the high
		// byte can change first in rare race conditions.
        curtime=GetTickCount();
        if (curtime > endtime) {
            bye("can't connect with skunkboard.");
        }
	} while (0xf0ff != (poll&0xf0ff));

	if (poll == 0xffff) {
		// now, get the lower 12 bytes of that buffer - should contain the serial number
		// If it fails, fall back on the old approach
		if ((usb_control_msg(udev, 0xC0, 0xff, 4, 0x2800, (char*)SerBuf, 12, 1000) == 12)) {
			// check for the magic word at the beginning (note the funky byte swapping!)
			if (0 == memcmp(SerBuf, "\x57\xfa\x0d\xf0", 4)) {
				// that is what we are looking for! The next 8 bytes are the revision and serial number
				// Note that they are in BCD!
				printf("Boot version %02x.%02x.%02x, Serial %02x%02x\n",
					SerBuf[6], SerBuf[5], SerBuf[4],  SerBuf[9], SerBuf[8]);
				return;
			}
		}
	}

	// Else, fall back on the old system. You can tell which one happened by
	// looking for the 'Sending...' output of the upload. :)

	// This will make DoFile shell out to the console before returning
	g_OptConsole=true;
	g_OptSilentConsole=true;
	DoFile((uchar*)DUMPVER, 0x5000, SIZE_OF_DUMPVER, 168, true);
	printf("\n");
}

/* Request the Jaguar to print out serial number in big text (uses the console to collect it) */
/* Sort of an Easter Egg, but meant to help in test. */
void DoSerialBig() {
	unsigned char SerBuf[12];
	unsigned short poll;
	char szOut[128];
	int i;

	// Open socket to Jaguar
	if (NULL == udev) {
		udev = findEZ(true, true);
	}

	// On the newer boards, we can get this information without uploading a program, so try that first
	// First, make sure that the $2800 buffer is marked as ready
	do {
		Spin();

		if ((usb_control_msg(udev, 0xC0, 0xff, 4, 0x2800+0xFEA, (char*)&poll, 2, 1000) != 2)) {
			Reattach();
		}
//			printf("Polled 0x%08x\n", poll);
		// poll is unsigned. But a value over 0xf0xx is reserved for future use
		// and we can't count on the lower bytes to be correct since the high
		// byte can change first in rare race conditions.
	} while (0xf0ff != (poll&0xf0ff));

	if (poll == 0xffff) {
		// now, get the lower 12 bytes of that buffer - should contain the serial number
		// If it fails, fall back on the old approach
		if ((usb_control_msg(udev, 0xC0, 0xff, 4, 0x2800, (char*)SerBuf, 12, 1000) == 12)) {
			// check for the magic word at the beginning (note the funky byte swapping!)
			if (0 == memcmp(SerBuf, "\x57\xfa\x0d\xf0", 4)) {
				// that is what we are looking for! The next 8 bytes are the revision and serial number
				// Note that they are in BCD!
				// build each row - converting 2 digit BCD for the most part (we drop the first 0)
				for (i=0; i<7; i++) {
					szOut[0]='\0';
					CatVal(szOut, sizeof(szOut), SerBuf[6]%16, i);
					CatVal(szOut, sizeof(szOut), -2, i);
					CatVal(szOut, sizeof(szOut), SerBuf[5]/16, i);
					CatVal(szOut, sizeof(szOut), SerBuf[5]%16, i);
					CatVal(szOut, sizeof(szOut), -2, i);
					CatVal(szOut, sizeof(szOut), SerBuf[4]/16, i);
					CatVal(szOut, sizeof(szOut), SerBuf[4]%16, i);
					CatVal(szOut, sizeof(szOut), -1, i);
					CatVal(szOut, sizeof(szOut), SerBuf[9]/16, i);
					CatVal(szOut, sizeof(szOut), SerBuf[9]%16, i);
					CatVal(szOut, sizeof(szOut), SerBuf[8]/16, i);
					CatVal(szOut, sizeof(szOut), SerBuf[8]%16, i);
					printf("%s\n", szOut);
				}
				return;
			}
		}
	}

	// This one won't fall back on the old system
	bye("FAILED TO GET SERIAL NUMBER BY NEW SYSTEM");
}

/* Do a BIOS update (if available) */
void DoBiosUpdate() {
	unsigned char SerBuf[12];
	bool gotupgrade1 = false;
	bool gotupgrade3 = false;
	int currentRev = 0;
	bool bForce = false;

#ifdef INCLUDE_BIOS_10204
	gotupgrade1 = true;
#endif
#ifdef INCLUDE_BIOS_30002
	gotupgrade3 = true;
#endif

	if ((!gotupgrade1) && (!gotupgrade3)) {
		bye("BIOS update not included in this build of JCP");
	}

	if (g_OptDoFlash) {
		// overriding this flash to mean force update, to repair a damaged BIOS
		bForce = true;
		g_OptDoFlash = false;	// don't confuse the poor upload code
	}

	if (g_OptOverrideFlash) {
		// force the old rev 1 updater -- necessary with really old rev 1
		// boards (for instance, 1.01.00 has trouble). Should be okay with
		// rev 2 boards, so no override provided for that. This still won't
		// load rev 1 BIOS on a rev 2 or 3, so it's okay.
		printf("Forcing upgrade to assume Rev 1 board.\n");
		currentRev=1;
	} else {
		// first, find out what board we currently have so we know what to load
		// to do this across all the various revisions, we need to upload some code
		printf("Examining current board...\n");
		g_OptConsole=false;
		g_skipwait=true;
		DoFile((uchar*)readver, 0x5000, SIZE_OF_READVER, 168, true);
		g_skipwait=false;
		Sleep(500);

		// after the program runs, we should be able to just read the current information
		// Open socket to Jaguar
	//	Reattach();

		// readver copies the version data in the same way as the latest BIOSs do, so just read it
		// we don't check for the buffer again, because that doesn't work with the rev 1 board
		// we delayed long enough above that all should be well.
		// now, get the lower 12 bytes of that buffer - should contain the serial number
		if ((usb_control_msg(udev, 0xC0, 0xff, 4, 0x2800, (char*)SerBuf, 12, 1000) == 12)) {
			// check for the magic word at the beginning (note the funky byte swapping!)
			if (0 == memcmp(SerBuf, "\x57\xfa\x0d\xf0", 4)) {
				// that is what we are looking for! The next 8 bytes are the revision and serial number
				// Note that they are in BCD!
				// Major - SerBuf[6]
				// Minor - SerBuf[5]
				// Rev   - SerBuf[4]
				currentRev = SerBuf[6];
			} else {
				bye("\n\nNot able to read current board revision\n");
			}
		}
	}

	if ((currentRev == 1) && (!gotupgrade1)) {
		bye("\n\nBIOS Upgrade for Rev 1 boards not included in this version of JCP\n");
	}
	if (((currentRev == 2)||(currentRev == 3)) && (!gotupgrade3)) {
		bye("\n\nBIOS Upgrade for Rev 2 or 3 boards not included in this version of JCP\n");
	}

	if ((currentRev < 1) || (currentRev > 3)) {
		printf("\n\nCurrent rev is '%02x'\n", currentRev);
		bye("Upgrade is not supported by this version of JCP\n");
	}

	DoResetAndReconnect(true);

	g_OptConsole=true;
	g_OptSilentConsole=true;

	// off we go now!
	if (currentRev == 1) {
#ifdef INCLUDE_BIOS_10204
		// Major - SerBuf[6]
		// Minor - SerBuf[5]
		// Rev   - SerBuf[4]
		if (!bForce) {
			if ((SerBuf[6] == 1) && (SerBuf[5] == 2) && (SerBuf[4] == 4)) {
				bye("Board is already on revision 1.02.04 - upgrade not required (-fu to force).\n");
			}
		}

		printf("\n\nGoing to upgrade Rev 1 board to 1.02.04\n\n");
		DoFile((uchar*)Upgrade10204, 0x80000, SIZE_OF_UPGRADE10204, 168, true);
#endif
	} else {
		// 2 and 3
#ifdef INCLUDE_BIOS_30002
		// Major - SerBuf[6]
		// Minor - SerBuf[5]
		// Rev   - SerBuf[4]
		if (!bForce) {
			if ((SerBuf[6] == 3) && (SerBuf[5] == 0) && (SerBuf[4] == 2)) {
				bye("Board is already on revision 3.00.02 - upgrade not required (-fu to force).\n");
			}
		}

		printf("\n\nGoing to upgrade Rev %d board to Rev 3 BIOS 3.00.02 (okay for Rev2)\n\n", currentRev);
		DoFile((uchar*)upgrayyd30002, 0x80000, SIZE_OF_UPGRAYYD30002, 168, true);
#endif
	}
}

/* Writes a block to the Jaguar */
/* uchar points to data to write, curbase is the base to load at,
   start is the start address or -1 if not starting yet, and len
   is the number of bytes to load. len should be even or at least
   the buffer must be an even size!
   This function writes into the other-than-current block */
void WriteABlock(uchar *data, int curbase, int start, int len) {
	uchar block[4080];
	int i;
	volatile unsigned short poll;
	DWORD curtime,endtime;

	// check for cartridge header space
	if ( ((curbase >= 0x800000) && (curbase < 0x802000)) ||
		((curbase+len >= 0x800000) && (curbase+len < 0x802000)) ) {
			if ((nCartBank != 1) || (g_SixMegWrite == false)) {
				printf("\n* Skipping block at 0x%08X - unwritable.\n", curbase);
				return;
			}
	}
	// verify flash memory if needed
	if ((g_OptDoFlash) && (g_OptFlashActive)) {
		if (curbase < 0x800000) {
			printf("\n* Skipping block at 0x%08X - not flash!\n", curbase);
			return;
		}
	}
	// skip the RAM check if this is our magic block
	if ((curbase == DUMMYBASE) && ((g_OptOnlyBoot) || (g_OptNoBoot) || (g_OptConsoleUp))) {
		// do nothing
	} else {
		if (!g_OptDoFlash) {
			if (curbase >= 0x200000) {
				printf("\n* Skipping block at 0x%08X - not RAM!\n", curbase);
				return;
			}
			if (curbase <= 0x2800) {
				printf("\n* Skipping block at 0x%08X - protected RAM!\n", curbase);
				return;
			}
		}
	}

	memset(block, 0, 4080);

	// 'Fix' the byte order for the next block of file data
	for (i = 0; i < len; i += 2) {
		block[i+1] = *data++;
		block[i] = *data++;
	}

	// Set up block trailer
	block[0xFE2] = curbase & 255;
	block[0xFE3] = (curbase >> 8) & 255;
	block[0xFE0] = (curbase >> 16) & 255;
	block[0xFE1] = (curbase >> 24) & 255;

	block[0xFE6] = start & 255;
	block[0xFE7] = (start >> 8) & 255;
	block[0xFE4] = (start >> 16) & 255;
	block[0xFE5] = (start >> 24) & 255;

	block[0xFE8] = 0;
	block[0xFE9] = nextez >> 8;
	nextez = (0x1800 == nextez) ? 0x2800 : 0x1800;

	block[0xFEA] = len & 255;
	block[0xFEB] = (len >> 8) & 255;

	if (g_OptVerbose) {
		printf("ez: %04x  start: %08x  len: %04x  base: %08x/%08x\n", nextez,
			ENMIDEND(block+0xfe4), HALFLITTLEEND(block+0xfea), ENMIDEND(block+0xfe0), curbase);
	}

	// Wait for the block to come free (handshake with 68K).
	poll=0;
	endtime = GetTickCount() + 2000;
	do {
		Spin();

		if ((usb_control_msg(udev, 0xC0, 0xff, 4, nextez+0xFEA, (char*)&poll, 2, 1000) != 2)) {
			Reattach();
		}
		//printf("Polled 0x%08x\n", poll);
		// poll is unsigned. But a value over 0xf0xx is reserved for future use
		// and we can't count on the lower bytes to be correct since the high
		// byte can change first in rare race conditions.

		curtime=GetTickCount();
        if (curtime > endtime) {
            bye("can't connect with skunkboard");
        }
	} while (0xf0ff != (poll&0xf0ff));

	// any value except 0xffff indicates a future use, possibly that
	// this is a new firmware. We only need this here because this is
	// always the first block sent to the Jaguar.
	if (poll != 0xffff) {
		if (g_OptVerbose) {
			printf("value %04X", poll);
		}
		bye("Got invalid value from block synchronization. Please use a newer JCP.");
	}

	// Send off the finished block.
	if (usb_control_msg(udev, 0x40, 0xfe, 4080, nextez, (char*)block, 4080, 1000) != 4080) {
		Reattach();
	}

	// check for successful start, except for 'internal' utilities. These may
	// boot so fast and return to command mode so quick we can't catch it.
	if ((!g_skipwait) && (-1 != start) && (-2 != start)) {
		// since this block should have triggered a boot on Jag, we wait for it
		// Jag should set to 0000 or 8888 (with a possible ffff intermediate)
		// Wait for the block to change to a valid setting (handshake with 68K).
		poll=0;
		do {
			Spin();

			if ((usb_control_msg(udev, 0xC0, 0xff, 4, nextez+0xFEA, (char*)&poll, 2, 1000) != 2)) {
				Reattach();
			}
			//printf("Polled 0x%08x\n", poll);
		} while ((0x0000 != poll) && (0x8888 != poll));

		if (poll == 0x8888) {
			bye("Unauthorized. You must flash a different rom to proceed.\n(Remember to reset the jag with 'jcp -r'!)");
		} else {
			if ( (g_OptVerbose) || (!g_OptSilentConsole) ) {
				printf("Jag accepted start request at $%08X.\n", start&0x00ffffff);
			}
		}
	}
}

/* Send a file to the Jaguar */
void SendFile(int flen, uchar *fptr, int curbase, int base) {
	int dotty=0;
	int len;

	while (flen > 0) {
		int start = (flen <= 4064) ? base : -1;

		if (-1 != start) {
			if (g_OptNoBoot) {
				if (g_OptFlashActive) {
					// flash technically can't no-boot, so this is the first half
					// of a 6MB copy. Tell the flash program to return to cmd mode
					start=-2;
				} else {
					// it really is no-boot
					start=-1;
				}
			} else {
				dotty=0;
				if (g_OptVerbose) {
					printf(" \nBooting address $%X\n", start);
				} else {
					printf(" \n");
				}
				/* add modifiers only when the flash is proceeding */
				if ((g_OptFlashActive)||(g_OptOnlyBoot)) {
					if (nCartBank == 1) {
						start|=0x10000000;		// set bank 2
					}
					if (nCartBank == -1) {
						start|=0x70000000;		// set 6MB mode (signed, avoid high bit)
					}
				}
			}
		}

		len = (flen <= 4064) ? flen : 4064;

		WriteABlock(fptr, curbase, start, len);

		fptr += 4064;
		curbase += 4064;
		flen -= 4064;

		dotty = (dotty + 1) & 7;
		if ((0 == dotty) && (!g_OptQuietMode)) {
			putchar('.');
		}
	}

	// if this is a no-boot case, we need to make sure the next block will
	// be at $2800, as that's the only address jcp polls to start up. So
	// if needed, we'll send a little dummy block here, like with -b
	if ((g_OptNoBoot) && (nextez != 0x1800)) {
		DWORD dummy=0;
		WriteABlock((unsigned char*)&dummy, DUMMYBASE, -1, 4);
	}
}

/* Parse a file for headers, and prepare to send it to the Jaguar */
/* returns the number of bytes processed including headers */
/* note: builtin files DO NOT autodetect - make sure your base and */
/* skip values are correct! (You can run the file manually to get them) */
int DoFile(uchar *fdata, int base, int flen, int skip, bool builtin) {
	// Send the data to the Jaguar, 4064 bytes at a time
	int curbase;
	int ticks, oldlen;
	int nRet=0;
	uchar *fptr;

	if (!g_OptOnlyBoot) {
		if (!builtin) {
			// this one mutes so we don't print the info twice
			DetermineFileInfo(true, fdata, &base, &flen, &skip);

			if (g_HeaderSkip > 0) {
				printf("Forcing a manual header skip of %d bytes (was %d)\n", g_HeaderSkip, skip);
				skip=g_HeaderSkip;
			}
		}

		curbase=base;
		flen -= skip;

		if ((base >= 0x800000) || (base+flen >= 0x800000)) {
			if ((!g_OptDoFlash) && (!g_OptOverrideFlash)) {
				if (!g_OptAutoMode) {
					bye("This upload requires the -f option to prepare the flash!");
				}
				/* if the exe was renamed, then we WILL do flash */
				g_OptDoFlash=true;
				DoFlash(flen+skip);
				g_OptFlashActive=true;
			}
		}
	} else {
		// the only-boot mode, we send a dummy block to the top of unused ROM space and then boot the address
		curbase=DUMMYBASE;
		flen=4;		// smallest transfer size
		skip=0;
	}

	fptr = fdata + skip;

	// Open socket to Jaguar
	if (NULL == udev) {
		udev = findEZ(true, true);
	}

	/* if this is the first file, and we are in auto mode, check if reset is needed */
	if ((g_OptAutoMode) && (!g_FirstFileSent)) {
		if (TestIfBuffersLocked()) {
			// buffers locked - probably from a previous upload, so reset
			DoResetAndReconnect(true);
		}
	}

	g_FirstFileSent=true;
	ticks = GetTickCount();
	oldlen=flen;

	if (!g_OptOnlyConsole) {
		printf("Sending...");
		if ((g_SixMegWrite) && (!g_OptOnlyBoot)) {
			if ((curbase+flen) >= 0xc00000) {
				// trim it down and only send what's needed
				flen-=(curbase+flen)-0xc00000;
			}
		}
		nRet=flen+skip;
		SendFile(flen, fptr, curbase, base);
	}

	if (!g_OptOnlyBoot) {
		int res;

		ticks = GetTickCount() - ticks;
		if (ticks > 0) {
			res=oldlen/ticks;
		} else {
			res=0;
		}
		if ( (g_OptVerbose) || (!g_OptSilentConsole) ) {
			printf(" \nFinished in %d millis, %dKB/second.\n", ticks, res);
		}
	}

	if (g_OptConsole) {
		HandleConsole();
	}

	return nRet;
}

// Abort nicely(?)
void bye(char* msg) {
	if (msg[0]!='\0') {
		printf("* %s\n", msg);
	}
	if (NULL != udev) {
		usb_close(udev);
	}
	if (NULL != fdata) {
		free(fdata);
	}
	exit(1);
}

/* Locate the Jaguar on the USB bus, open it, get a handle, and upload the turboW tool */
usb_dev_handle* findEZ(bool fInstallTurbo, bool fAbortOnFail) {
	struct usb_bus *bus;
	struct usb_device *dev = NULL;
	int nTriesLeft=3;

	while (nTriesLeft--) {
		usb_init();
		usb_set_debug(0);

		usb_find_busses();
		usb_find_devices();

		for (bus = usb_get_busses(); bus; bus = bus->next) {
			for (dev = bus->devices; dev; dev = dev->next) {
				if (0x4b4 == dev->descriptor.idVendor && 0x7200 == dev->descriptor.idProduct) {
					usb_dev_handle* udev = usb_open(dev);
					if (!udev) {
						bye("- Found, but can't open, EZ-HOST. In use or not ready? ");
						udev=NULL;
						break;		/* should quickly fall out of outer loop too, unless there's another! */
					}
					if (fInstallTurbo) {
						// load turbow from array
						int ret = usb_control_msg(udev, 0x40, 0xff, 0, 0x304c, (char*)turbow, SIZE_OF_TURBOW, 1000);

						if (ret < 1) {
							printf("Failed to install turbow.bin!\n");
						} else {
							if (g_OptVerbose) {
								printf("Installed turbow.bin: %d scan codes sent\n", ret);
							}
						}
					}
					return udev;
				}
			}
		}

		if (NULL == udev) {
			if (nTriesLeft > 0) {
				printf(".. retrying ..\n");
				Sleep(1000);
			}
		}
	}

	if (fAbortOnFail) {
		bye("Can't open EZ-HOST.\n");
		// does not return
	}
	return NULL;
}

/* called from a failed attempt to access the jag */
void Reattach() {
	printf("Waiting to handshake with 68k (control-c to abort)\n");
	udev=NULL;
	Sleep(1000);
	udev = findEZ(true, true);
}

/* Does all the console functions */
void HandleConsole() {
#ifndef REMOVERS
	int nTotalFileLength=0;		// bytes written
#endif
	uchar block[4080];
	unsigned short tmp;
	int i, len;
#ifdef WIN32
	char *p, *oldp;
#endif

	// If the user requested an external console, then we just have to shell out to it here
	if (NULL != g_pszExtShell) {
		printf(" \nStarting external console...\n");
		if (-1 == _execlp(g_pszExtShell, g_pszExtShell, NULL)) {
			printf("Could not exec external shell, error %d\n", errno);
			bye("External shell failed to launch.");
		}
		// if it returns
		exit(0);
	}

	// our basic job is to relay text from the Jaguar. Extension commands are marked with 0xffff as the first two bytes
	// In text mode, we accept text from either buffer, so we scan them both
	if ( (g_OptVerbose) || (!g_OptSilentConsole) ) {
		printf(" \nStarting console...\n");
	}
	if (g_OptDoDump) {
		printf(" \nReceiving flash...\n");
	}
        if (g_OptDoE2pDump) {
		printf(" \nReceiving serial EEPROM...\n");
	}
        if (g_OptDoE2pWrite) {
		printf(" \nWriting serial EEPROM...\n");
	}

	// flag console as up
	g_OptConsoleUp = true;

	// blank both buffers
	memset(block, 0, 4080);

	// Set up block trailer
	block[0xFE8] = 0;
	block[0xFE9] = nextez >> 8;
	nextez = (0x1800 == nextez) ? 0x2800 : 0x1800;

	block[0xFEA] = 0xff;
	block[0xFEB] = 0xff;

	// to handshake with the jaguar, we clear the blocks from this end
	// that way the Jag knows we're up and ready.
	tmp=0xffff;
	for (;;) {
		if (usb_control_msg(udev, 0x40, 0xfe, 4080, 0x1800+0xFEA, (char*)&tmp, 2, 1000) != 2) {
			Reattach();
			continue;
		}
		if (usb_control_msg(udev, 0x40, 0xfe, 4080, 0x2800+0xFEA, (char*)&tmp, 2, 1000) != 2) {
			Reattach();
			continue;
		}
		break;
	}

	// now we can start the main loop
	for (;;) {
		// Wait for the block to be used (handshake with 68K).
		volatile short poll=0;
		do {
			nextez = (0x1800 == nextez) ? 0x2800 : 0x1800;
			// It's actually faster to check this small block and
			// do two reads than to read the whole block just to test
			if ((usb_control_msg(udev, 0xC0, 0xff, 4, nextez+0xFEA, (char*)&poll, 2, 1000) != 2)) {
				Reattach();
			}
		} while (-1 == poll);

		// Read in the finished block.
		for (;;) {
			if (usb_control_msg(udev, 0xC0, 0xff, 4080, nextez, (char*)block, 4080, 1000) == 4080) {
				break;
			}
			Reattach();
		}

		// acknowledge the buffer as read to delag the jag
		tmp=0xffff;
		for (;;) {
			if (usb_control_msg(udev, 0x40, 0xfe, 4080, nextez+0xFEA, (char*)&tmp, 2, 1000) == 2) {
				break;
			}
			Reattach();
		}

		// deswap the block (including the header)
		for (i=0; i<4080; i+=2) {
			int x=block[i+1];
			block[i+1]=block[i];
			block[i]=x;
		}

		if (g_OptVerbose) {
			printf("Read block from %x, len %d, first bytes: %02x %02x %02x %02x\n", nextez, ((block[0xfea]<<8)|block[0xfeb]), block[0], block[1], block[2], block[3]);
		}

		if (0 == ((block[0xfea]<<8)|block[0xfeb])) {
			// bad block (left over flag from booting), ignore
			continue;
		}

		// Now do something with it
		if ((block[0]==0xff) && (block[1]==0xff)) {
			// escape command (16 bit command)
			switch ((block[2]<<8)|block[3]) {
#ifndef REMOVERS
				case 0:		// nop - can be handy for synchronizing?
					if (g_OptVerbose) {
						printf("NOP received.\n");
					}
					break;

				case 1:		// terminate console
					if ( (g_OptVerbose) || (!g_OptSilentConsole) ) {
						printf("Console terminating.\n");
					}
					return;

				case 2:		// receive input
					{
						// get input from the user
						char buf[4064];
						printf("> ");
						if (!fgets(buf, 4064, stdin)) {
							printf("Failed to read from stdin, code %d\n", errno);
							buf[0]='\0';
						} else {
							buf[4063]='\0';
						}

						// strip EOL
						i=(int)strlen(buf)-1;
						while (i > 0) {
							if (buf[i] < ' ') {
								buf[i]='\0';
								i--;
							} else {
								break;
							}
						}

						// write that input to the jag in the alternate buffer
						WriteABlock((unsigned char*)buf, DUMMYBASE, -1, (int)strlen(buf)+1);

						if (g_OptVerbose) {
							printf("Wait for Jag to clear %04X\n", nextez);
						}

						// now we must not proceed from this point until the Jaguar
						// acknowledges that block by clearing its length
						do {
							if ((usb_control_msg(udev, 0xC0, 0xff, 4, nextez+0xFEA, (char*)&poll, 2, 1000) != 2)) {
								Reattach();
							}
							Sleep(500);
						} while (0 != poll);

						// Now clear the buffer back to 0xffff so the Jag can use it again
						tmp=0xffff;
						for (;;) {
							if (usb_control_msg(udev, 0x40, 0xfe, 4080, nextez+0xFEA, (char*)&tmp, 2, 1000) == 2) {
								break;
							}
							Reattach();
						}
					}
					break;

				case 3:		// Open a file for writing
					{
						char buf[4064];
						int i;

						for (i=0; i<4060; i++) {
							buf[i]=block[i+4];
							if (buf[i]=='\0') break;
						}
						buf[i]='\0';	// makesure
						FilenameSanitize(buf);

						if (NULL != fp) {
							printf("Closing file...\n");
							fclose(fp);
							fp=NULL;
						}

						fp=fopen(buf, "wb");
						if (NULL != fp) {
							printf("Opened %s for writing...\n", buf);
							nTotalFileLength=0;
						} else {
							printf("Failed to open %s for writing, code %d\n", buf, errno);
						}
					}
					break;

				case 4:		// open a file for reading
					{
						char buf[4064];
						int i;

						for (i=0; i<4060; i++) {
							buf[i]=block[i+4];
							if (buf[i]=='\0') break;
						}
						buf[i]='\0';	// makesure
						FilenameSanitize(buf);

						if (NULL != fp) {
							printf("Closing file...\n");
							fclose(fp);
							fp=NULL;
						}

						fp=fopen(buf, "rb");
						if (NULL != fp) {
							printf("Opened %s for reading...\n", buf);
						} else {
							printf("Failed to open %s for reading, code %d\n", buf, errno);
						}
					}
					break;

				case 5:		// write a block to the open file
					if (NULL != fp) {
						int nLength;

						Spin();
						nLength=((block[0xfea]<<8)|block[0xfeb])-4;
						if (nLength > 4060) nLength=4060;
						fwrite(&block[4], 1, nLength, fp);
						nTotalFileLength+=nLength;
						if (g_OptVerbose) {
							printf("Wrote %d bytes, total %d\n", nLength, nTotalFileLength);
						}
					}
					break;

				case 6:		// read a block from a file
					if (NULL != fp) {
						char buf[4064];
						int nLength;

						Spin();
						nLength=(block[0xfea]<<8)|block[0xfeb];
						if (g_OptVerbose) {
							printf("Read requested %d -", nLength);
						}
						if (nLength > 4064) nLength=4064;
						nLength=(int)fread(buf, 1, nLength, fp);
						if (g_OptVerbose) {
							printf(" got %d\n", nLength);
						}
						// write that input to the jag in the alternate buffer
						WriteABlock((unsigned char*)buf, DUMMYBASE, -1, nLength);

						if (g_OptVerbose) {
							printf("Wait for Jag to clear %04X\n", nextez);
						}
					} else {
						// need to send an empty reply back to the Jag
						int nDummy=0;

						// write that input to the jag in the alternate buffer
						WriteABlock((unsigned char*)&nDummy, DUMMYBASE, -1, 0);
					}

					// now we must not proceed from this point until the Jaguar
					// acknowledges that block by clearing its length
					do {
						if ((usb_control_msg(udev, 0xC0, 0xff, 4, nextez+0xFEA, (char*)&poll, 2, 1000) != 2)) {
							Reattach();
						}
					} while (0 != poll);

					// Now clear the buffer back to 0xffff so the Jag can use it again
					tmp=0xffff;
					for (;;) {
						if (usb_control_msg(udev, 0x40, 0xfe, 4080, nextez+0xFEA, (char*)&tmp, 2, 1000) == 2) {
							break;
						}
						Reattach();
					}
					break;

				case 7:		// close a file
					if (NULL != fp) {
						printf("Closing file...\n");
						fclose(fp);
						fp=NULL;
					}
					break;
#else
                        case 1:
                          serve_request((char *)block+4, NULL);
                          break;
                        case 2: {
                          char buf[4064];

                          serve_request((char *)block+4, buf);
                          int nLength = MSGHDRSZ+get_message_length(buf); // add header size to content length

                          // write that input to the jag in the alternate buffer
                          WriteABlock((unsigned char*)buf, DUMMYBASE, -1, nLength);

                          // now we must not proceed from this point until the Jaguar
                          // acknowledges that block by clearing its length
                          do {
                            if ((usb_control_msg(udev, 0xC0, 0xff, 4, nextez+0xFEA, (char*)&poll, 2, 1000) != 2)) {
                              Reattach();
                            }
                          } while (0 != poll);

                          // Now clear the buffer back to 0xffff so the Jag can use it again
                          tmp=0xffff;
                          for (;;) {
                            if (usb_control_msg(udev, 0x40, 0xfe, 4080, nextez+0xFEA, (char*)&tmp, 2, 1000) == 2) {
                              break;
                            }
                            Reattach();
                          }

                          break;
                        }
#endif
				default:
					printf("Unimplemented command 0x%04X\n", (block[2]<<8)|block[3]);
					break;
			}
			continue;
		}

		// else get the length and reformat as a string
		len=block[0xfea]|(block[0xfeb]<<8);
		if (len>4064) len=4064;
		block[len]='\0';

		// Check for and trap formfeed characters
		// use them to clear the screen, like an old-school terminal
		// Not sure if Linux/Apple will support this?
#ifdef WIN32
		p = block;
		oldp = p;
		while (NULL != (p=strchr(p, '\xc'))) {
			*p='\0';
			printf("%s", oldp);
			system("cls");
			oldp=p+1;
		}
		printf("%s", oldp);
#else
		printf("%s", (const char*)block);
#endif
	}
}


// remove any path information from buf
void FilenameSanitize(char *buf) {
	char *ptr;

	ptr=strrchr(buf, '/');
	if (NULL != ptr) {
		memmove(buf, ptr, strlen(ptr)+1);
	}
	ptr=strrchr(buf, '\\');
	if (NULL != ptr) {
		memmove(buf, ptr, strlen(ptr)+1);
	}
}

// parse a string address (used by command line)
// Does not return on failure!
int ParseAddress(const char *pBuf) {
	int base;

	if ((pBuf[0] != '$') && ((pBuf[0]!='0')||(pBuf[1]!='x'))) {
		printf("Could not parse address '%s'\n", pBuf);
		bye("Address must start with '$' or 0x. (Linux may require '\\$')");
	}
	if (pBuf[0] == '$') {
		if (1 != sscanf(&pBuf[1], "%x", &base)) {
			printf("Could not scan address '%s'\n", pBuf);
			bye("Failed to parse address.");
		}
	} else {
		if (1 != sscanf(&pBuf[2], "%x", &base)) {
			printf("Could not scan address '%s'\n", pBuf);
			bye("Failed to parse address..");
		}
	}

	return base;
}

// helper for the SerialBig function
void CatVal(char *szOut, int nBufLen, int nVal, int nRow) {
	if ((nRow < 0) || (nRow > 6)) {
		// out of range, return
		return;
	}

	if ((nVal < -2) || (nVal > 9)) {
		// out of range, clamp value
		nVal=0;
	}

	if ((signed)(strlen(szOut) + strlen(szNumDat[0])) > nBufLen-1) {
		// too long!
		return;
	}

	if (nVal == -1) {
		nVal=10;
	} else if (nVal == -2) {
		nVal=11;
	}

	// digit row
	strcat(szOut, szNumDat[nDigits[nVal][nRow]]);
}

// Looks at the data to determine the true base, length, and skip
// Pass in what you already know, values may be updated. Return
// is true if file type was recognized, false otherwise. Fields
// may still be changed even if false is returned!
// Will only set values to absolute settings (ie: it must be safe
// to call this function multiple times with updated values)
bool DetermineFileInfo(bool bMute, uchar *fdata, int *base, int *flen, int *skip) {
	bool ret=true;	// assume it will be true

	// Check file header
	if ((*flen > 0x2000) && (0x802000 == ENBIGEND(fdata+0x404))) {
		if (!bMute) {
			if ( (g_OptVerbose) || (!g_OptSilentConsole) ) {
				printf("Cart ROM:  ");
			}
		}
		if (!g_OptOverride) *base = 0x802000;
		*skip = 0x2000;
	} else if ((*flen > 0x2200) && (0x802000 == ENBIGEND(fdata+0x604))) {
		if (!bMute) {
			if ( (g_OptVerbose) || (!g_OptSilentConsole) ) {
				printf("Cart ROM + 512:  ");
			}
		}
		if (!g_OptOverride) *base = 0x802000;
		*skip = 0x2200;
	} else if ((*flen > 72) && (fdata[0] == 0x01) && (fdata[1] == 0x50)) {
		if (!bMute) {
			if ( (g_OptVerbose) || (!g_OptSilentConsole) ) {
				printf("COFF File:  ");
			}
		}
		if (!g_OptOverride) *base = ENBIGEND(fdata+56);
		*skip = ENBIGEND(fdata+68);
		if (*flen <= *skip) {
			bye("Detection error or corrupt file.\n");
		}
	} else if ((*flen > 0x30) && (fdata[0] == 0x7f) && (fdata[1] == 'E') && (fdata[2] == 'L') && (fdata[3] == 'F')) {
		int loadbase;
		int secs, seclen;
		uchar *img, *secptr;

		if (!bMute) {
			if ( (g_OptVerbose) || (!g_OptSilentConsole) ) {
				printf("ELF File:  ");
			}
		}

		if ((fdata[5] != 0x2) || (0x20004 != ENBIGEND(fdata+0x10))) {
			bye("Not 68K executable.");
		}
		*skip = loadbase = ENBIGEND(fdata+0x18);
		*flen = 0;

		// Map all the sections into a new memory image. Not necessarily entirely safe.
		secs = HALFBIGEND(fdata+0x30);
		seclen = HALFBIGEND(fdata+0x2e);
		img = (uchar*)malloc(RAMBUFSIZE);
		secptr = fdata+ENBIGEND(fdata+0x20);
		memset(img, 0, 2048*1024);
		while (secs-- >= 0) {
			int sadr = ENBIGEND(secptr+0xc), slen = ENBIGEND(secptr+0x14);
			uchar* fptr = fdata+ENBIGEND(secptr+0x10);
			if (0 != sadr) {		// 0 is debug info, so ignore it
				if (sadr < loadbase)
					bye("Section has base address below entry point.  See readelf for details.");
				if (sadr+slen > *flen)
					*flen = sadr+slen;
				if (*flen >= 2048*1024 || loadbase < 0)
					bye("Section falls outside Jaguar memory.  See readelf for details.");
				if (1 == ENBIGEND(secptr+0x4))	// Progbits, so copy them
					memcpy(img+sadr, fptr, slen);
			}
			secptr+=seclen;
		}
		// copy the data across to the true buffer
		memcpy(fdata, img, RAMBUFSIZE);
		free(img);
		if (!g_OptOverride) *base=loadbase;
	} else if ((*flen > 0x2e) && (fdata[0x1c] == 'J') && (fdata[0x1d] == 'A') && (fdata[0x1e] == 'G') && (fdata[0x1f] == 'R')) {
		if (!bMute) {
			if ( (g_OptVerbose) || (!g_OptSilentConsole) ) {
				printf("Jag Server Exe: ");
			}
		}
		if (!g_OptOverride) *base=ENBIGEND(fdata+0x22);
		*skip=0x2e;
	} else if ((*flen > 0x24) && (fdata[0] == 0x60) && (fdata[1] == 0x1b)) {
		if (!bMute) {
			if ( (g_OptVerbose) || (!g_OptSilentConsole) ) {
				printf("DRI ABS File:  ");
			}
		}
		*skip = 0x24;
		*base = ENBIGEND(fdata+0x16);
		*flen = ENBIGEND(fdata+0x6) + ENBIGEND(fdata+0x2) + *skip;
	} else if ((*flen > 0xa8) && (fdata[0] == 0x01) && (fdata[1] == 0x50)) {
		if (!bMute) {
			if ( (g_OptVerbose) || (!g_OptSilentConsole) ) {
				printf("Alcyon ABS File:  ");
			}
		}
		*skip = 0xa8;
		*base = ENBIGEND(fdata + 0x28);		// Right now, the code below assumes base = run address.
		// run = ENBIGEND(fdata + 0x24);	// But these files can have different run and base addresses.
		*flen = ENBIGEND(fdata+0x18) + ENBIGEND(fdata+0x1c) + *skip;
	} else {
		// check for headerless ROM padded with a solid value (normally would be FF)
		// skip the first 8 bytes as some vendors put data there
		int fPadded=false;
		int idx, nFirst;

		if (*flen > 0x2000) {
			nFirst=fdata[8];
			for (idx=9; idx<8192; idx++) {
				if (fdata[idx] != nFirst) break;
			}
			if ((idx>=8192) && (fdata[8192] != nFirst)) {	// first byte at $2000 in is actual data (could still misread with weird padding bytes)
				fPadded=true;
			}
		}
		if (fPadded) {
			if (!bMute) {
				if ( (g_OptVerbose) || (!g_OptSilentConsole) ) {
					printf("Headerless padded ROM: ");
				}
			}
			if (!g_OptOverride) *base=0x802000;
			*skip=0x2000;
		} else {
			// if all else failed, and the extension is .ROM, assume $802000 load address
			char *pTmp=strrchr(g_szFilename, '.');
			if (NULL != pTmp) {
				if (_stricmp(pTmp, ".rom")==0) {
					if ( (g_OptVerbose) || (!g_OptSilentConsole) ) {
						if (!bMute) {
							// check for exact multiple of 2MB or 4MB, and warn user if so
							if ((*flen == 1024*1024*2) || (*flen == 1024*1024*4)) {
								printf("Warning: ROM size is suspicious but no common header found. Full file being uploaded.\nIf it fails, consider adding '-h 8192' to skip a 2k header\n\n");
							}
							printf("Headerless ROM: ");
						}
					}
					if (!g_OptOverride) *base=0x802000;
					*skip=0;
				}
			}
			ret=false;		// either way, if we got here we are guessing
		}
	}

	if (!bMute) {
		if ( (g_OptVerbose) || (!g_OptSilentConsole) ) {
			printf("Skip %d bytes, base addr is $%X, length is %d bytes\n", *skip, *base, *flen-*skip);
		}
	}

	return ret;
}
