/*
 * Amiga build stub for <pgmspace.h>.
 *
 * On AVR/ESP8266, pgmspace.h provides macros that read from program-flash
 * separately from RAM.  On Amiga, all data is in normal RAM, so every
 * pgm_read_* macro is just a regular pointer dereference and PROGMEM is empty.
 */
#ifndef _AMIGA_AAC_PGMSPACE_STUB_H
#define _AMIGA_AAC_PGMSPACE_STUB_H

#define PROGMEM
#define PSTR(s)              (s)
#define PGM_P                const char *
#define PGM_VOID_P           const void *

#define pgm_read_byte(addr)  (*(const unsigned char  *)(addr))
#define pgm_read_word(addr)  (*(const unsigned short *)(addr))
#define pgm_read_dword(addr) (*(const unsigned long  *)(addr))
#define pgm_read_float(addr) (*(const float          *)(addr))
#define pgm_read_ptr(addr)   (*(const void * const   *)(addr))

#define memcpy_P   memcpy
#define strcpy_P   strcpy
#define strncpy_P  strncpy
#define strcmp_P   strcmp
#define strncmp_P  strncmp
#define strlen_P   strlen
#define strcat_P   strcat
#define sprintf_P  sprintf

#endif /* _AMIGA_AAC_PGMSPACE_STUB_H */
