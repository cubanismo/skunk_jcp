//
// Data file FLASHST.COF - Sep  9 2005
//
// This program and associated binaries is public domain, released 2016 by Mike Brent
//

unsigned char FLASHSTUB[] = {
	0x01,0x50,0x00,0x03,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xC8,0x00,0x00,0x00,0x03,	// .P.............. //
	0x00,0x1C,0x00,0x03,0x00,0x00,0x01,0x07,0x00,0x00,0x00,0x20,0x00,0x00,0x00,0x00,	// ........... .... //
	0x00,0x00,0x00,0x00,0x00,0x00,0x41,0x00,0x00,0x00,0x41,0x00,0x00,0x00,0x41,0x20,	// ......A...A...A  //
	0x2E,0x74,0x65,0x78,0x74,0x00,0x00,0x00,0x00,0x00,0x41,0x00,0x00,0x00,0x41,0x00,	// .text.....A...A. //
	0x00,0x00,0x00,0x20,0x00,0x00,0x00,0xA8,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,	// ... ............ //
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x20,0x2E,0x64,0x61,0x74,0x61,0x00,0x00,0x00,	// ....... .data... //
	0x00,0x00,0x41,0x20,0x00,0x00,0x41,0x20,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xC8,	// ..A ..A ........ //
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x40,	// ...............@ //
	0x2E,0x62,0x73,0x73,0x00,0x00,0x00,0x00,0x00,0x00,0x40,0x00,0x00,0x00,0x40,0x00,	// .bss......@...@. //
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xC8,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,	// ................ //
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x80,0x22,0x3C,0x0A,0xBC,0xDE,0xF0,0x21,0xC1,	// ........"<....!. //
	0x3F,0xF0,0x0A,0x81,0x00,0x00,0xFF,0xFF,0x21,0xC1,0x3F,0xF4,0x22,0x79,0x00,0x80,	// ?.......!.?."y.. //
	0x08,0x04,0x4E,0xD1,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x04,0x05,0x00,0x00,0x00,	// ..N............. //
	0x00,0x00,0x41,0x20,0x00,0x00,0x00,0x0C,0x07,0x00,0x00,0x00,0x00,0x00,0x41,0x20,	// ..A ..........A  //
	0x00,0x00,0x00,0x14,0x09,0x00,0x00,0x00,0x00,0x00,0x40,0x00,0x00,0x00,0x00,0x1B,	// ..........@..... //
	0x5F,0x54,0x45,0x58,0x54,0x5F,0x45,0x00,0x5F,0x44,0x41,0x54,0x41,0x5F,0x45,0x00,	// _TEXT_E._DATA_E. //
	0x5F,0x42,0x53,0x53,0x5F,0x45,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,	// _BSS_E.......... //
};

// Size of data in above array (Array may be padded but this will be correct)
#define SIZE_OF_FLASHSTUB 263
