/*
 *  Copyright (C) 2004 aCaB <acab@clamav.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*
** upxdec.c
**
** 05/05/2k4 - 1st attempt
** 08/05/2k4 - Now works as a charm :D
** 09/05/2k4 - Moved code outta main(), got rid of globals for thread safety, added bound checking, minor cleaning
** 04/06/2k4 - Now we handle 2B, 2D and 2E :D
** 28/08/2k4 - PE rebuild for nested packers
*/

/*
** This code unpacks a dumped UPX1 section to a file.
** It was written reversing the loader found on some Win32 UPX compressed trojans; while porting
** it to C i've kinda followed the asm flow so it will probably be a bit hard to read.
** This code DOES NOT revert the uncompressed section to its original state as no E8/E9 fixup and
** of cause no IAT rebuild are performed.
**
** The Win32 asm unpacker is really a little programming jewel, pretty damn rare in these days of
** bloatness. My gratitude to whoever wrote it.
*/

#if HAVE_CONFIG_H
#include "clamav-config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

#include "cltypes.h"
#include "others.h"

#define HEADERS "\
\x4D\x5A\x90\x00\x02\x00\x00\x00\x04\x00\x0F\x00\xFF\xFF\x00\x00\
\xB0\x00\x00\x00\x00\x00\x00\x00\x40\x00\x1A\x00\x00\x00\x00\x00\
\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\
\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xD0\x00\x00\x00\
\x0E\x1F\xB4\x09\xBA\x0D\x00\xCD\x21\xB4\x4C\xCD\x21\x54\x68\x69\
\x73\x20\x66\x69\x6C\x65\x20\x77\x61\x73\x20\x63\x72\x65\x61\x74\
\x65\x64\x20\x62\x79\x20\x43\x6C\x61\x6D\x41\x56\x20\x66\x6F\x72\
\x20\x69\x6E\x74\x65\x72\x6E\x61\x6C\x20\x75\x73\x65\x20\x61\x6E\
\x64\x20\x73\x68\x6F\x75\x6C\x64\x20\x6E\x6F\x74\x20\x62\x65\x20\
\x72\x75\x6E\x2E\x0D\x0A\x43\x6C\x61\x6D\x41\x56\x20\x2D\x20\x41\
\x20\x47\x50\x4C\x20\x76\x69\x72\x75\x73\x20\x73\x63\x61\x6E\x6E\
\x65\x72\x20\x2D\x20\x68\x74\x74\x70\x3A\x2F\x2F\x77\x77\x77\x2E\
\x63\x6C\x61\x6D\x61\x76\x2E\x6E\x65\x74\x0D\x0A\x24\x00\x00\x00\
"

#if WORDS_BIGENDIAN == 0
#define EC32(v) (v)
#else
static inline uint32_t EC32(uint32_t v)
{
    return ((v >> 24) | ((v & 0x00FF0000) >> 8) | ((v & 0x0000FF00) << 8) | (v << 24));
}
#endif

#define cli_writeint32(offset,value) *(uint32_t *)(offset) = EC32(value)

/* PE from UPX */

int pefromupx (char *src, char *dst, int *dsize, uint32_t ep, uint32_t upx0, uint32_t upx1, uint32_t magic)
{
  char *imports, *sections, *pehdr, *newbuf;
  int sectcnt, upd=1, realstuffsz, align;
  int foffset=0xd0+0xf8;

  imports = dst + cli_readint32(src + ep - upx1 + magic);

  realstuffsz = imports-dst;
  
  if ( realstuffsz < 0 || realstuffsz > *dsize )
    return 0;
  
  pehdr = imports;
  while (pehdr+7 < dst+*dsize && cli_readint32(pehdr)) {
    pehdr+=8;
    while(pehdr+1 < dst+*dsize && *pehdr) {
      pehdr++;
      while (pehdr+1 < dst+*dsize && *pehdr)
	pehdr++;
      pehdr++;
    }
    pehdr++;
  }

  pehdr+=4;
  if (pehdr+0xf8 > dst+*dsize)
    return 0;
  
  if ( cli_readint32(pehdr) != 0x4550 )
    return 0;
  
  if (! (align = cli_readint32(pehdr+0x38)))
    return 0;
  
  sections = pehdr+0xf8;
  if ( ! (sectcnt = pehdr[6]+256*pehdr[7]))
    return 0;
  
  foffset+=0x28*sectcnt;
  
  if (pehdr + 0xf8 + 0x28*sectcnt >= dst + *dsize)
    return 0;
  
  for (upd = 0; upd <sectcnt ; upd++) {
    uint32_t vsize=cli_readint32(sections+8)-1;
    uint32_t rsize=cli_readint32(sections+16);
    uint32_t urva=cli_readint32(sections+12);
    
    vsize=(((vsize/0x1000)+1)*0x1000); /* FIXME: get bounds from header */
    
    /* Within bounds ? */
    if ( urva < upx0 || urva + vsize > upx0 + realstuffsz)
      return 0;
    
    /* Rsize -gt Vsize ? */
    if ( rsize > vsize )
      return 0;
    
    /* Am i been fooled? There are better ways ;) */
    if ( rsize+4 < vsize && cli_readint32(dst+urva-upx0+rsize) )
      return 0;
    
    cli_writeint32(sections+8, vsize);
    cli_writeint32(sections+20, foffset);
    foffset+=rsize;
    
    sections+=0x28;
  }

  cli_writeint32(pehdr+8, 0x4d414c43);

  if (!(newbuf = (char *) cli_malloc(foffset)))
    return 0;

  memcpy(newbuf, HEADERS, 0xd0);
  memcpy(newbuf+0xd0, pehdr,0xf8+0x28*sectcnt);
  sections = pehdr+0xf8;
  for (upd = 0; upd <sectcnt ; upd++) {
    memcpy(newbuf+cli_readint32(sections+20), dst+cli_readint32(sections+12)-upx0, cli_readint32(sections+16));
    sections+=0x28;
  }

  /* CBA restoring the imports they'll look different from the originals anyway... */
  /* ...and yeap i miss the icon too :P */

  memcpy(dst, newbuf, foffset);
  *dsize = foffset;
  free(newbuf);

  cli_dbgmsg("UPX: PE structure rebuilt from compressed file\n");
  return 1;
}


/* [doubleebx] */

static int doubleebx(char *src, int32_t *myebx, int *scur, int ssize)
{
  int32_t oldebx = *myebx;
#if WORDS_BIGENDIAN == 1
  char *pt;
  int32_t shift, i = 0;
#endif

  *myebx*=2;
  if ( !(oldebx & 0x7fffffff)) {
    if (*scur<0 || ssize-*scur<4)
      return -1;
#if WORDS_BIGENDIAN == 0
    oldebx = *(int*)(src+*scur);
#else
    oldebx = 0;
    pt = src + *scur;
    for(shift = 0; shift < 32; shift += 8) {
      oldebx |= (pt[i] & 0xff ) << shift;
      i++;
    }
#endif

    *myebx = oldebx*2+1;
    *scur+=4;
  }
  return (oldebx>>31)&1;
}

/* [inflate] */

int upx_inflate2b(char *src, int ssize, char *dst, int *dsize, uint32_t upx0, uint32_t upx1, uint32_t ep)
{
  int32_t backbytes, unp_offset = -1, myebx = 0;
  int scur=0, dcur=0, i, backsize,oob;

  while (1) {
    while ((oob = doubleebx(src, &myebx, &scur, ssize)) == 1) {
      if (scur<0 || scur>=ssize || dcur<0 || dcur>=*dsize)
	return -1;
      dst[dcur++] = src[scur++];
    }

    if ( oob == -1 )
      return -1;
    
    backbytes = 1;

    while (1) {
      if ( (oob = doubleebx(src, &myebx, &scur, ssize)) == -1 )
        return -1;
      backbytes = backbytes*2+oob;
      if ( (oob = doubleebx(src, &myebx, &scur, ssize)) == -1 )
	return -1;
      if (oob)
        break;
    }

    backsize = 0;	
    backbytes-=3;
  
    if ( backbytes >= 0 ) {

      if (scur<0 || scur>=ssize)
	return -1;
      backbytes<<=8;
      backbytes+=(unsigned char)(src[scur++]);
      backbytes^=0xffffffff;

      if (!backbytes)
	break;
      unp_offset = backbytes;
    }

    if ( (oob = doubleebx(src, &myebx, &scur, ssize)) == -1)
      return -1;
    backsize = oob;
    if ( (oob = doubleebx(src, &myebx, &scur, ssize)) == -1)
      return -1;
    backsize = backsize*2 + oob;
    if (!backsize) {
      backsize++;
      do {
        if ( (oob = doubleebx(src, &myebx, &scur, ssize)) == -1)
          return -1;
	backsize = backsize*2 + oob;
      } while ((oob = doubleebx(src, &myebx, &scur, ssize)) == 0);
      if ( oob == -1 )
        return -1;
      backsize+=2;
    }

    if ( (unsigned int)unp_offset < 0xfffff300 )
      backsize++;

    backsize++;

    for (i = 0; i < backsize; i++) {
      if (dcur+i<0 || dcur+i>=*dsize || dcur+unp_offset+i<0 || dcur+unp_offset+i>=*dsize)
	return -1;
      dst[dcur + i] = dst[dcur + unp_offset + i];
    }
    dcur+=backsize;
  }


  if ( ep - upx1 + 0x14f <= ssize-5  &&    /* Wondering how we got so far?! */
       src[ep - upx1 + 0x14f] == '\xe9' && /* JMP OldEip                    */
       src[ep - upx1 + 0x106] == '\x8d' && /* lea edi, ...                  */
       src[ep - upx1 + 0x107] == '\xbe' )  /* ... [esi + offset]          */
    return pefromupx (src, dst, dsize, ep, upx0, upx1, 0x108);

  return 0;
}

int upx_inflate2d(char *src, int ssize, char *dst, int *dsize, uint32_t upx0, uint32_t upx1, uint32_t ep)
{
  int32_t backbytes, unp_offset = -1, myebx = 0;
  int scur=0, dcur=0, i, backsize, oob;

  while (1) {
    while ( (oob = doubleebx(src, &myebx, &scur, ssize)) == 1) {
      if (scur<0 || scur>=ssize || dcur<0 || dcur>=*dsize)
	return -1;
      dst[dcur++] = src[scur++];
    }

    if ( oob == -1 )
      return -1;

    backbytes = 1;

    while (1) {
      if ( (oob = doubleebx(src, &myebx, &scur, ssize)) == -1 )
        return -1;
      backbytes = backbytes*2+oob;
      if ( (oob = doubleebx(src, &myebx, &scur, ssize)) == -1 )
        return -1;
      if (oob)
	break;
      backbytes--;
      if ( (oob = doubleebx(src, &myebx, &scur, ssize)) == -1 )
        return -1;
      backbytes=backbytes*2+oob;
    }

    backsize = 0;
    backbytes-=3;
  
    if ( backbytes >= 0 ) {

      if (scur<0 || scur>=ssize)
	return -1;
      backbytes<<=8;
      backbytes+=(unsigned char)(src[scur++]);
      backbytes^=0xffffffff;

      if (!backbytes)
	break;
      backsize = backbytes & 1;
      backbytes>>=1;
      unp_offset = backbytes;
    }
    else {
      if ( (backsize = doubleebx(src, &myebx, &scur, ssize)) == -1 )
        return -1;
    }
 
    if ( (oob = doubleebx(src, &myebx, &scur, ssize)) == -1 )
      return -1;
    backsize = backsize*2 + oob;
    if (!backsize) {
      backsize++;
      do {
        if ( (oob = doubleebx(src, &myebx, &scur, ssize)) == -1 )
          return -1;
	backsize = backsize*2 + oob;
      } while ( (oob = doubleebx(src, &myebx, &scur, ssize)) == 0);
      if ( oob == -1 )
        return -1;
      backsize+=2;
    }

    if ( (unsigned int)unp_offset < 0xfffffb00 ) 
      backsize++;

    backsize++;
    for (i = 0; i < backsize; i++) {
      if (dcur+i<0 || dcur+i>=*dsize || dcur+unp_offset+i<0 || dcur+unp_offset+i>=*dsize)
	return -1;
      dst[dcur + i] = dst[dcur + unp_offset + i];
    }
    dcur+=backsize;
  }

  if ( ep - upx1 + 0x139 <= ssize-5  &&    /* Wondering how we got so far?! */
       src[ep - upx1 + 0x139] == '\xe9' && /* JMP OldEip                    */
       src[ep - upx1 + 0xe7] == '\x8d' && /* lea edi, ...                  */
       src[ep - upx1 + 0xe8] == '\xbe' )  /* ... [esi + offset]          */
    return pefromupx (src, dst, dsize, ep, upx0, upx1, 0xe9);

  return 0;
}

int upx_inflate2e(char *src, int ssize, char *dst, int *dsize, uint32_t upx0, uint32_t upx1, uint32_t ep)
{
  int32_t backbytes, unp_offset = -1, myebx = 0;
  int scur=0, dcur=0, i, backsize, oob;

  while (1) {
    while ( (oob = doubleebx(src, &myebx, &scur, ssize)) ) {
      if (oob == -1)
        return -1;
      if (scur<0 || scur>=ssize || dcur<0 || dcur>=*dsize)
	return -1;
      dst[dcur++] = src[scur++];
    }

    backbytes = 1;

    while (1) {
      if ( (oob = doubleebx(src, &myebx, &scur, ssize)) == -1 )
        return -1;
      backbytes = backbytes*2+oob;
      if ( (oob = doubleebx(src, &myebx, &scur, ssize)) == -1 )
        return -1;
      if ( oob )
	break;
      backbytes--;
      if ( (oob = doubleebx(src, &myebx, &scur, ssize)) == -1 )
        return -1;
      backbytes=backbytes*2+oob;
    }

    backsize = 0;
    backbytes-=3;
  
    if ( backbytes >= 0 ) {

      if (scur<0 || scur>=ssize)
	return -1;
      backbytes<<=8;
      backbytes+=(unsigned char)(src[scur++]);
      backbytes^=0xffffffff;

      if (!backbytes)
	break;
      backsize = backbytes & 1; /* Using backsize to carry on the shifted out bit (UPX uses CF) */
      backbytes>>=1;
      unp_offset = backbytes;
    }
    else {
      if ( (backsize = doubleebx(src, &myebx, &scur, ssize)) == -1 )
        return -1;
    } /* Using backsize to carry on the doubleebx result (UPX uses CF) */

    if (backsize) { /* i.e. IF ( last sar shifted out 1 bit || last doubleebx()==1 ) */
      if ( (backsize = doubleebx(src, &myebx, &scur, ssize)) == -1 )
        return -1;
    }
    else {
      backsize = 1;
      if ((oob = doubleebx(src, &myebx, &scur, ssize)) == -1)
        return -1;
      if (oob) {
	if ((oob = doubleebx(src, &myebx, &scur, ssize)) == -1)
	  return -1;
	  backsize = 2 + oob;
	}
      else {
	do {
          if ((oob = doubleebx(src, &myebx, &scur, ssize)) == -1)
          return -1;
	  backsize = backsize * 2 + oob;
	} while ((oob = doubleebx(src, &myebx, &scur, ssize)) == 0);
	if (oob == -1)
          return -1;
	backsize+=2;
      }
    }
 
    if ( (unsigned int)unp_offset < 0xfffffb00 ) 
      backsize++;

    backsize+=2;
    for (i = 0; i < backsize; i++) {
      if (dcur+i<0 || dcur+i>=*dsize || dcur+unp_offset+i<0 || dcur+unp_offset+i>=*dsize)
	return -1;
      dst[dcur + i] = dst[dcur + unp_offset + i];
    }
    dcur+=backsize;
  }

  if ( ep - upx1 + 0x145 <= ssize-5  &&    /* Wondering how we got so far?! */
       src[ep - upx1 + 0x145] == '\xe9' && /* JMP OldEip                    */
       src[ep - upx1 + 0xf3] == '\x8d' && /* lea edi, ...                  */
       src[ep - upx1 + 0xf4] == '\xbe' )  /* ... [esi + offset]          */
    return pefromupx (src, dst, dsize, ep, upx0, upx1, 0xf5);

  return 0;
}
