/*
 * avrdude - A Downloader/Uploader for AVR device programmers
 * Copyright (C) 2005 Joerg Wunsch <j@uriah.heep.sax.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* $Id$ */

/*
 * avrdude interface for Atmel JTAG ICE (mkI) programmer
 */

#include "ac_cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>

#include "avr.h"
#include "crc16.h"
#include "pgm.h"
#include "jtagmkI_private.h"
#include "serial.h"
extern int    verbose;
extern char * progname;
extern int do_cycles;

/*
 * XXX There should really be a programmer-specific private data
 * pointer in struct PROGRAMMER.
 */
static long initial_baudrate;

/*
 * See jtagmkI_read_byte() for an explanation of the flash and
 * EEPROM page caches.
 */
static unsigned char *flash_pagecache;
static unsigned long flash_pageaddr;
static unsigned int flash_pagesize;

static unsigned char *eeprom_pagecache;
static unsigned long eeprom_pageaddr;
static unsigned int eeprom_pagesize;

static int prog_enabled;	/* Cached value of PROGRAMMING status. */
/*
 * The OCDEN fuse is bit 7 of the high fuse (hfuse).  In order to
 * perform memory operations on MTYPE_SPM and MTYPE_EEPROM, OCDEN
 * needs to be programmed.
 *
 * OCDEN should probably rather be defined via the configuration, but
 * if this ever changes to a different fuse byte for one MCU, quite
 * some code here needs to be generalized anyway.
 */
#define OCDEN (1 << 7)

/*
 * Table of baud rates supported by the mkI ICE, accompanied by their
 * internal parameter value.
 *
 * 19200 is the initial value of the ICE after powerup, and virtually
 * all connections then switch to 115200.  As the table is also used
 * to try connecting at startup, we keep these two entries on top to
 * speedup the program start.
 */
const static struct {
  long baud;
  unsigned char val;
} baudtab[] = {
  {  19200L, 0xfa },
  { 115200L, 0xff },
  {   9600L, 0xf4 },
  {  38400L, 0xfd },
  {  57600L, 0xfe },
/*  {  14400L, 0xf8 }, */ /* not supported by serial driver */
};

static int jtagmkI_read_byte(PROGRAMMER * pgm, AVRPART * p, AVRMEM * mem,
			      unsigned long addr, unsigned char * value);
static int jtagmkI_write_byte(PROGRAMMER * pgm, AVRPART * p, AVRMEM * mem,
			       unsigned long addr, unsigned char data);
static int jtagmkI_set_sck_period(PROGRAMMER * pgm, double v);
static int jtagmkI_getparm(PROGRAMMER * pgm, unsigned char parm,
			    unsigned char * value);
static int jtagmkI_setparm(PROGRAMMER * pgm, unsigned char parm,
			    unsigned char value);
static void jtagmkI_print_parms1(PROGRAMMER * pgm, char * p);

static int jtagmkI_resync(PROGRAMMER *pgm, int maxtries);

static void
u32_to_b3(unsigned char *b, unsigned long l)
{
  b[2] = l & 0xff;
  b[1] = (l >> 8) & 0xff;
  b[0] = (l >> 16) & 0xff;
}

static void
u16_to_b2(unsigned char *b, unsigned short l)
{
  b[0] = l & 0xff;
  b[1] = (l >> 8) & 0xff;
}

static void jtagmkI_prmsg(PROGRAMMER * pgm, unsigned char * data, size_t len)
{
  int i;

  if (verbose >= 4) {
    fprintf(stderr, "Raw message:\n");

    for (i = 0; i < len; i++) {
      fprintf(stderr, "0x%02x ", data[i]);
      if (i % 16 == 15)
	putc('\n', stderr);
      else
	putchar(' ');
    }
    if (i % 16 != 0)
      putc('\n', stderr);
  }

  switch (data[0]) {
  case RESP_OK:
    fprintf(stderr, "OK\n");
    break;

  case RESP_FAILED:
    fprintf(stderr, "FAILED\n");
    break;

  case RESP_BREAK:
    fprintf(stderr, "breakpoint hit\n");
    break;

  case RESP_INFO:
    fprintf(stderr, "IDR dirty\n");
    break;

  case RESP_SYNC_ERROR:
    fprintf(stderr, "Synchronization lost\n");
    break;

  case RESP_SLEEP:
    fprintf(stderr, "sleep instruction hit\n");
    break;

  case RESP_POWER:
    fprintf(stderr, "target power lost\n");

  default:
    fprintf(stderr, "unknown message 0x%02x\n", data[0]);
  }

  putc('\n', stderr);
}


static int jtagmkI_send(PROGRAMMER * pgm, unsigned char * data, size_t len)
{
  unsigned char *buf;

  if (verbose >= 3)
    fprintf(stderr, "\n%s: jtagmkI_send(): sending %zd bytes\n",
	    progname, len);

  if ((buf = malloc(len + 2)) == NULL)
    {
      fprintf(stderr, "%s: jtagmkI_send(): out of memory",
	      progname);
      exit(1);
    }

  memcpy(buf, data, len);
  buf[len] = ' ';		/* "CRC" */
  buf[len + 1] = ' ';		/* EOP */

  if (serial_send(pgm->fd, buf, len + 2) != 0) {
    fprintf(stderr,
	    "%s: jtagmkI_send(): failed to send command to serial port\n",
	    progname);
    exit(1);
  }

  free(buf);

  return 0;
}

static void jtagmkI_recv(PROGRAMMER * pgm, unsigned char * buf, size_t len)
{
  if (serial_recv(pgm->fd, buf, len) != 0) {
    fprintf(stderr,
	    "\n%s: jtagmkI_recv(): failed to send command to serial port\n",
	    progname);
    exit(1);
  }
  if (verbose >= 3) {
    putc('\n', stderr);
    jtagmkI_prmsg(pgm, buf, len);
  }
}


static int jtagmkI_drain(PROGRAMMER * pgm, int display)
{
  return serial_drain(pgm->fd, display);
}


static int jtagmkI_resync(PROGRAMMER * pgm, int maxtries)
{
  int tries;
  unsigned char buf[1], resp[1];
  long otimeout = serial_recv_timeout;

  serial_recv_timeout = 200;

  if (verbose >= 3)
    fprintf(stderr, "%s: jtagmkI_resync()\n", progname);

  jtagmkI_drain(pgm, 0);

  for (tries = 0; tries < maxtries; tries++) {

    /* Get the sign-on information. */
    buf[0] = CMD_GET_SYNC;
    if (verbose >= 2)
      fprintf(stderr, "%s: jtagmkI_resync(): Sending sync command: ",
	      progname);

    if (serial_send(pgm->fd, buf, 1) != 0) {
      fprintf(stderr,
	      "\n%s: jtagmkI_resync(): failed to send command to serial port\n",
	      progname);
      serial_recv_timeout = otimeout;
      return -1;
    }
    if (serial_recv(pgm->fd, resp, 1) == 0 && resp[0] == RESP_OK) {
      if (verbose >= 2)
	fprintf(stderr, "got RESP_OK\n");
      break;
    }
  }
  if (tries >= maxtries) {
    if (verbose >= 2)
      fprintf(stderr,
	      "%s: jtagmkI_resync(): "
	      "timeout/error communicating with programmer\n",
	      progname);
    serial_recv_timeout = otimeout;
    return -1;
  }

  serial_recv_timeout = otimeout;
  return 0;
}

static int jtagmkI_getsync(PROGRAMMER * pgm)
{
  unsigned char buf[1], resp[9];

  if (jtagmkI_resync(pgm, 5) < 0)
    return -1;

  if (verbose >= 2)
    fprintf(stderr, "%s: jtagmkI_getsync(): Sending sign-on command: ",
	    progname);

  buf[0] = CMD_GET_SIGNON;
  jtagmkI_send(pgm, buf, 1);
  jtagmkI_recv(pgm, resp, 9);
  if (verbose >= 2) {
    resp[8] = '\0';
    fprintf(stderr, "got %s\n", resp + 1);
  }

  return 0;
}

static int jtagmkI_cmd(PROGRAMMER * pgm, unsigned char cmd[4],
                        unsigned char res[4])
{

  fprintf(stderr, "%s: jtagmkI_command(): no direct SPI supported for JTAG\n",
	  progname);
  return -1;
}


/*
 * issue the 'chip erase' command to the AVR device
 */
static int jtagmkI_chip_erase(PROGRAMMER * pgm, AVRPART * p)
{
  unsigned char buf[1], resp[2];

  buf[0] = CMD_CHIP_ERASE;
  if (verbose >= 2)
    fprintf(stderr, "%s: jtagmkI_chip_erase(): Sending chip erase command: ",
	    progname);
  jtagmkI_send(pgm, buf, 1);
  jtagmkI_recv(pgm, resp, 2);
  if (resp[0] != RESP_OK) {
    if (verbose >= 2)
      putc('\n', stderr);
    fprintf(stderr,
	    "%s: jtagmkI_chip_erase(): "
	    "timeout/error communicating with programmer (resp %c)\n",
	    progname, resp[0]);
    return -1;
  } else {
    if (verbose == 2)
      fprintf(stderr, "OK\n");
  }

  pgm->initialize(pgm, p);

  return 0;
}

static void jtagmkI_set_devdescr(PROGRAMMER * pgm, AVRPART * p)
{
  unsigned char resp[2];
  LNODEID ln;
  AVRMEM * m;
  struct {
    unsigned char cmd;
    struct device_descriptor dd;
  } sendbuf;

  memset(&sendbuf, 0, sizeof sendbuf);
  sendbuf.cmd = CMD_SET_DEVICE_DESCRIPTOR;
  sendbuf.dd.ucSPMCRAddress = p->spmcr;
  sendbuf.dd.ucRAMPZAddress = p->rampz;
  sendbuf.dd.ucIDRAddress = p->idr;
  for (ln = lfirst(p->mem); ln; ln = lnext(ln)) {
    m = ldata(ln);
    if (strcmp(m->desc, "flash") == 0) {
      flash_pagesize = m->page_size;
      u16_to_b2(sendbuf.dd.uiFlashPageSize, flash_pagesize);
    } else if (strcmp(m->desc, "eeprom") == 0) {
      sendbuf.dd.ucEepromPageSize = eeprom_pagesize = m->page_size;
    }
  }

  if (verbose >= 2)
    fprintf(stderr, "%s: jtagmkI_set_devdescr(): "
	    "Sending set device descriptor command: ",
	    progname);
  jtagmkI_send(pgm, (unsigned char *)&sendbuf, sizeof(sendbuf));

  jtagmkI_recv(pgm, resp, 2);
  if (resp[0] != RESP_OK) {
    if (verbose >= 2)
      putc('\n', stderr);
    fprintf(stderr,
	    "%s: jtagmkI_set_devdescr(): "
	    "timeout/error communicating with programmer (resp %c)\n",
	    progname, resp[0]);
  } else {
    if (verbose == 2)
      fprintf(stderr, "OK\n");
  }
}

/*
 * Reset the target.
 */
static int jtagmkI_reset(PROGRAMMER * pgm)
{
  unsigned char buf[1], resp[2];

  buf[0] = CMD_RESET;
  if (verbose >= 2)
    fprintf(stderr, "%s: jtagmkI_reset(): Sending reset command: ",
	    progname);
  jtagmkI_send(pgm, buf, 1);

  jtagmkI_recv(pgm, resp, 2);
  if (resp[0] != RESP_OK) {
    if (verbose >= 2)
      putc('\n', stderr);
    fprintf(stderr,
	    "%s: jtagmkI_reset(): "
	    "timeout/error communicating with programmer (resp %c)\n",
	    progname, resp[0]);
    return -1;
  } else {
    if (verbose == 2)
      fprintf(stderr, "OK\n");
  }

  return 0;
}

static int jtagmkI_program_enable_dummy(PROGRAMMER * pgm, AVRPART * p)
{

  return 0;
}

static int jtagmkI_program_enable(PROGRAMMER * pgm)
{
  unsigned char buf[1], resp[2];

  if (prog_enabled)
    return 0;

  buf[0] = CMD_ENTER_PROGMODE;
  if (verbose >= 2)
    fprintf(stderr, "%s: jtagmkI_program_enable(): "
	    "Sending enter progmode command: ",
	    progname);
  jtagmkI_send(pgm, buf, 1);

  jtagmkI_recv(pgm, resp, 2);
  if (resp[0] != RESP_OK) {
    if (verbose >= 2)
      putc('\n', stderr);
    fprintf(stderr,
	    "%s: jtagmkI_program_enable(): "
	    "timeout/error communicating with programmer (resp %c)\n",
	    progname, resp[0]);
    return -1;
  } else {
    if (verbose == 2)
      fprintf(stderr, "OK\n");
  }

  prog_enabled = 1;

  return 0;
}

static int jtagmkI_program_disable(PROGRAMMER * pgm)
{
  unsigned char buf[1], resp[2];

  if (!prog_enabled)
    return 0;

  if (pgm->fd != -1) {
    buf[0] = CMD_LEAVE_PROGMODE;
    if (verbose >= 2)
      fprintf(stderr, "%s: jtagmkI_program_disable(): "
              "Sending leave progmode command: ",
              progname);
    jtagmkI_send(pgm, buf, 1);

    jtagmkI_recv(pgm, resp, 2);
    if (resp[0] != RESP_OK) {
      if (verbose >= 2)
        putc('\n', stderr);
      fprintf(stderr,
              "%s: jtagmkI_program_disable(): "
              "timeout/error communicating with programmer (resp %c)\n",
              progname, resp[0]);
      return -1;
    } else {
      if (verbose == 2)
        fprintf(stderr, "OK\n");
    }

    (void)jtagmkI_reset(pgm);
  }
  prog_enabled = 0;

  return 0;
}

static unsigned char jtagmkI_get_baud(long baud)
{
  int i;

  for (i = 0; i < sizeof baudtab / sizeof baudtab[0]; i++)
    if (baud == baudtab[i].baud)
      return baudtab[i].val;

  return 0;
}

/*
 * initialize the AVR device and prepare it to accept commands
 */
static int jtagmkI_initialize(PROGRAMMER * pgm, AVRPART * p)
{
  AVRMEM hfuse;
  char cmd[1], resp[2];
  unsigned char b;

  if (!(p->flags & AVRPART_HAS_JTAG)) {
    fprintf(stderr, "%s: jtagmkI_initialize(): part %s has no JTAG interface\n",
	    progname, p->desc);
    return -1;
  }

  jtagmkI_drain(pgm, 0);

  if (initial_baudrate != pgm->baudrate) {
    if ((b = jtagmkI_get_baud(pgm->baudrate)) == 0) {
      fprintf(stderr, "%s: jtagmkI_initialize(): unsupported baudrate %d\n",
              progname, pgm->baudrate);
    } else {
      if (verbose >= 2)
        fprintf(stderr, "%s: jtagmkI_initialize(): "
	      "trying to set baudrate to %d\n",
                progname, pgm->baudrate);
      if (jtagmkI_setparm(pgm, PARM_BITRATE, b) == 0) {
        initial_baudrate = pgm->baudrate; /* don't adjust again later */
        serial_setspeed(pgm->fd, pgm->baudrate);
      }
    }
  }

  if (pgm->bitclock != 0.0) {
    if (verbose >= 2)
      fprintf(stderr, "%s: jtagmkI_initialize(): "
	      "trying to set JTAG clock period to %.1f us\n",
	      progname, pgm->bitclock);
    if (jtagmkI_set_sck_period(pgm, pgm->bitclock) != 0)
      return -1;
  }

  cmd[0] = CMD_STOP;
  jtagmkI_send(pgm, cmd, 1);
  jtagmkI_recv(pgm, resp, 5);
  if (resp[0] != RESP_OK) {
    if (verbose >= 2)
      putc('\n', stderr);
    fprintf(stderr,
	    "%s: jtagmkI_initialize(): "
	    "timeout/error communicating with programmer (resp %c)\n",
	    progname, resp[0]);
  } else {
    if (verbose == 2)
      fprintf(stderr, "OK\n");
  }

  /*
   * Must set the device descriptor before entering programming mode.
   */
  jtagmkI_set_devdescr(pgm, p);

  jtagmkI_setparm(pgm, PARM_FLASH_PAGESIZE_LOW, flash_pagesize & 0xff);
  jtagmkI_setparm(pgm, PARM_FLASH_PAGESIZE_HIGH, flash_pagesize >> 8);
  jtagmkI_setparm(pgm, PARM_EEPROM_PAGESIZE, eeprom_pagesize & 0xff);

  free(flash_pagecache);
  free(eeprom_pagecache);
  if ((flash_pagecache = malloc(flash_pagesize)) == NULL) {
    fprintf(stderr, "%s: jtagmkI_initialize(): Out of memory\n",
	    progname);
    return -1;
  }
  if ((eeprom_pagecache = malloc(eeprom_pagesize)) == NULL) {
    fprintf(stderr, "%s: jtagmkI_initialize(): Out of memory\n",
	    progname);
    free(flash_pagecache);
    return -1;
  }
  flash_pageaddr = eeprom_pageaddr = (unsigned long)-1L;

  if (jtagmkI_reset(pgm) < 0)
    return -1;

  strcpy(hfuse.desc, "hfuse");
  if (jtagmkI_read_byte(pgm, p, &hfuse, 1, &b) < 0)
    return -1;
  if ((b & OCDEN) != 0)
    fprintf(stderr,
	    "%s: jtagmkI_initialize(): warning: OCDEN fuse not programmed, "
	    "single-byte EEPROM updates not possible\n",
	    progname);

  return 0;
}


static void jtagmkI_disable(PROGRAMMER * pgm)
{

  free(flash_pagecache);
  flash_pagecache = NULL;
  free(eeprom_pagecache);
  eeprom_pagecache = NULL;

  (void)jtagmkI_program_disable(pgm);
}

static void jtagmkI_enable(PROGRAMMER * pgm)
{
  return;
}


static int jtagmkI_open(PROGRAMMER * pgm, char * port)
{
  size_t i;

  if (verbose >= 2)
    fprintf(stderr, "%s: jtagmkI_open()\n", progname);

  strcpy(pgm->port, port);
  initial_baudrate = -1L;

  for (i = 0; i < sizeof(baudtab) / sizeof(baudtab[0]); i++) {
    if (verbose >= 2)
      fprintf(stderr,
              "%s: jtagmkI_open(): trying to sync at baud rate %ld:\n",
              progname, baudtab[i].baud);
    pgm->fd = serial_open(port, baudtab[i].baud);

    /*
     * drain any extraneous input
     */
    jtagmkI_drain(pgm, 0);

    if (jtagmkI_getsync(pgm) == 0) {
      initial_baudrate = baudtab[i].baud;
      if (verbose >= 2)
        fprintf(stderr, "%s: jtagmkI_open(): succeeded\n", progname);
      return 0;
    }

    serial_close(pgm->fd);
  }

  fprintf(stderr,
          "%s: jtagmkI_open(): failed to synchronize to ICE\n",
          progname);
  pgm->fd = -1;

  return -1;
}


static void jtagmkI_close(PROGRAMMER * pgm)
{
  unsigned char buf[1], resp[2];

  if (verbose >= 2)
    fprintf(stderr, "%s: jtagmkI_close()\n", progname);

  if (pgm->fd != -1) {
    buf[0] = CMD_GO;
    if (verbose >= 2)
      fprintf(stderr, "%s: jtagmkI_close(): Sending GO command: ",
              progname);
    jtagmkI_send(pgm, buf, 1);
    jtagmkI_recv(pgm, resp, 1);
    if (resp[0] != RESP_OK) {
      if (verbose >= 2)
        putc('\n', stderr);
      fprintf(stderr,
              "%s: jtagmkI_close(): "
              "timeout/error communicating with programmer (resp %c)\n",
              progname, resp[0]);
      exit(1);
    } else {
      if (verbose == 2)
        fprintf(stderr, "OK\n");
    }

    serial_close(pgm->fd);
  }

  pgm->fd = -1;
}


static int jtagmkI_paged_write(PROGRAMMER * pgm, AVRPART * p, AVRMEM * m,
				int page_size, int n_bytes)
{
  int addr, block_size, send_size, tries;
  unsigned char cmd[6], *datacmd;
  unsigned char resp[2];
  int is_flash = 0;
  long otimeout = serial_recv_timeout;
#define MAXTRIES 3

  if (verbose >= 2)
    fprintf(stderr, "%s: jtagmkI_paged_write(.., %s, %d, %d)\n",
	    progname, m->desc, page_size, n_bytes);

  if (jtagmkI_program_enable(pgm) < 0)
    return -1;

  if (page_size == 0) page_size = 256;

  if (page_size > 256) {
    fprintf(stderr, "%s: jtagmkI_paged_write(): page size %d too large\n",
	    progname, page_size);
    return -1;
  }

  if ((datacmd = malloc(page_size + 1)) == NULL) {
    fprintf(stderr, "%s: jtagmkI_paged_write(): Out of memory\n",
	    progname);
    return -1;
  }

  cmd[0] = CMD_WRITE_MEM;
  if (strcmp(m->desc, "flash") == 0) {
    cmd[1] = MTYPE_FLASH_PAGE;
    flash_pageaddr = (unsigned long)-1L;
    page_size = flash_pagesize;
    is_flash = 1;
  } else if (strcmp(m->desc, "eeprom") == 0) {
    cmd[1] = MTYPE_EEPROM_PAGE;
    eeprom_pageaddr = (unsigned long)-1L;
    page_size = eeprom_pagesize;
  }
  datacmd[0] = CMD_DATA;

  serial_recv_timeout = 1000;
  for (addr = 0; addr < n_bytes; addr += page_size) {
    report_progress(addr, n_bytes,NULL);

    tries = 0;
    again:

    if (tries != 0 && jtagmkI_resync(pgm, 2000) < 0) {
      fprintf(stderr,
	      "%s: jtagmkI_paged_write(): sync loss, retries exhausted\n",
	      progname);
      return -1;
    }

    if ((n_bytes-addr) < page_size)
      block_size = n_bytes - addr;
    else
      block_size = page_size;
    if (verbose >= 3)
      fprintf(stderr, "%s: jtagmkI_paged_write(): "
	      "block_size at addr %d is %d\n",
	      progname, addr, block_size);

    /* We always write full pages. */
    send_size = page_size;
    if (is_flash) {
      cmd[2] = send_size / 2 - 1;
      u32_to_b3(cmd + 3, addr / 2);
    } else {
      cmd[2] = send_size - 1;
      u32_to_b3(cmd + 3, addr);
    }

    if (verbose >= 2)
      fprintf(stderr, "%s: jtagmkI_paged_write(): "
	      "Sending write memory command: ",
	      progname);

    /* First part, send the write command. */
    jtagmkI_send(pgm, cmd, 6);
    jtagmkI_recv(pgm, resp, 1);
    if (resp[0] != RESP_OK) {
      if (verbose >= 2)
        putc('\n', stderr);
      fprintf(stderr,
              "%s: jtagmkI_paged_write(): "
              "timeout/error communicating with programmer (resp %c)\n",
              progname, resp[0]);
      if (tries++ < MAXTRIES)
	goto again;
      serial_recv_timeout = otimeout;
      return -1;
    } else {
      if (verbose == 2)
        fprintf(stderr, "OK\n");
    }

    /*
     * The JTAG ICE will refuse to write anything but a full page, at
     * least for the flash ROM.  If a partial page has been requested,
     * set the remainder to 0xff.  (Maybe we should rather read back
     * the existing contents instead before?  Doesn't matter much, as
     * bits cannot be written to 1 anyway.)
     */
    memset(datacmd + 1, 0xff, page_size);
    memcpy(datacmd + 1, m->buf + addr, block_size);

    /* Second, send the data command. */
    jtagmkI_send(pgm, datacmd, send_size + 1);
    jtagmkI_recv(pgm, resp, 2);
    if (resp[1] != RESP_OK) {
      if (verbose >= 2)
        putc('\n', stderr);
      fprintf(stderr,
              "%s: jtagmkI_paged_write(): "
              "timeout/error communicating with programmer (resp %c)\n",
              progname, resp[0]);
      if (tries++ < MAXTRIES)
	goto again;
      serial_recv_timeout = otimeout;
      return -1;
    } else {
      if (verbose == 2)
        fprintf(stderr, "OK\n");
    }
  }

  free(datacmd);
  serial_recv_timeout = otimeout;

#undef MAXTRIES
  return n_bytes;
}

static int jtagmkI_paged_load(PROGRAMMER * pgm, AVRPART * p, AVRMEM * m,
			       int page_size, int n_bytes)
{
  int addr, block_size, read_size, is_flash = 0, tries;
  unsigned char cmd[6], resp[256 * 2 + 3];
  long otimeout = serial_recv_timeout;
#define MAXTRIES 3

  if (verbose >= 2)
    fprintf(stderr, "%s: jtagmkI_paged_load(.., %s, %d, %d)\n",
	    progname, m->desc, page_size, n_bytes);

  if (jtagmkI_program_enable(pgm) < 0)
    return -1;

  page_size = m->readsize;

  cmd[0] = CMD_READ_MEM;
  if (strcmp(m->desc, "flash") == 0) {
    cmd[1] = MTYPE_FLASH_PAGE;
    is_flash = 1;
  } else if (strcmp(m->desc, "eeprom") == 0) {
    cmd[1] = MTYPE_EEPROM_PAGE;
  }

  if (page_size > (is_flash? 512: 256)) {
    fprintf(stderr, "%s: jtagmkI_paged_load(): page size %d too large\n",
	    progname, page_size);
    return -1;
  }

  serial_recv_timeout = 1000;
  for (addr = 0; addr < n_bytes; addr += page_size) {
    report_progress(addr, n_bytes,NULL);

    tries = 0;
    again:
    if (tries != 0 && jtagmkI_resync(pgm, 2000) < 0) {
      fprintf(stderr,
	      "%s: jtagmkI_paged_load(): sync loss, retries exhausted\n",
	      progname);
      return -1;
    }

    if ((n_bytes-addr) < page_size)
      block_size = n_bytes - addr;
    else
      block_size = page_size;
    if (verbose >= 3)
      fprintf(stderr, "%s: jtagmkI_paged_load(): "
	      "block_size at addr %d is %d\n",
	      progname, addr, block_size);

    if (is_flash) {
      read_size = 2 * ((block_size + 1) / 2); /* round up */
      cmd[2] = read_size / 2 - 1;
      u32_to_b3(cmd + 3, addr / 2);
    } else {
      read_size = page_size;
      cmd[2] = page_size - 1;
      u32_to_b3(cmd + 3, addr);
    }

    if (verbose >= 2)
      fprintf(stderr, "%s: jtagmkI_paged_load(): Sending read memory command: ",
	      progname);

    jtagmkI_send(pgm, cmd, 6);
    jtagmkI_recv(pgm, resp, read_size + 3);

    if (resp[read_size + 3 - 1] != RESP_OK) {
      if (verbose >= 2)
        putc('\n', stderr);
      fprintf(stderr,
              "%s: jtagmkI_paged_load(): "
              "timeout/error communicating with programmer (resp %c)\n",
              progname, resp[read_size + 3 - 1]);
      if (tries++ < MAXTRIES)
	goto again;

      serial_recv_timeout = otimeout;
      return -1;
    } else {
      if (verbose == 2)
        fprintf(stderr, "OK\n");
    }

    memcpy(m->buf + addr, resp + 1, block_size);
  }
  serial_recv_timeout = otimeout;

#undef MAXTRIES
  return n_bytes;
}

static int jtagmkI_read_byte(PROGRAMMER * pgm, AVRPART * p, AVRMEM * mem,
			      unsigned long addr, unsigned char * value)
{
  unsigned char cmd[6];
  unsigned char resp[256 * 2 + 3], *cache_ptr = NULL;
  unsigned long paddr = 0UL, *paddr_ptr = NULL;
  unsigned int pagesize = 0;
  int respsize = 3 + 1;
  int is_flash = 0;

  if (verbose >= 2)
    fprintf(stderr, "%s: jtagmkI_read_byte(.., %s, 0x%lx, ...)\n",
	    progname, mem->desc, addr);

  if (jtagmkI_program_enable(pgm) < 0)
    return -1;

  cmd[0] = CMD_READ_MEM;

  if (strcmp(mem->desc, "flash") == 0) {
    cmd[1] = MTYPE_FLASH_PAGE;
    pagesize = mem->page_size;
    paddr = addr & ~(pagesize - 1);
    paddr_ptr = &flash_pageaddr;
    cache_ptr = flash_pagecache;
    is_flash = 1;
  } else if (strcmp(mem->desc, "eeprom") == 0) {
    cmd[1] = MTYPE_EEPROM_PAGE;
    pagesize = mem->page_size;
    paddr = addr & ~(pagesize - 1);
    paddr_ptr = &eeprom_pageaddr;
    cache_ptr = eeprom_pagecache;
  } else if (strcmp(mem->desc, "lfuse") == 0) {
    cmd[1] = MTYPE_FUSE_BITS;
    addr = 0;
  } else if (strcmp(mem->desc, "hfuse") == 0) {
    cmd[1] = MTYPE_FUSE_BITS;
    addr = 1;
  } else if (strcmp(mem->desc, "efuse") == 0) {
    cmd[1] = MTYPE_FUSE_BITS;
    addr = 2;
  } else if (strcmp(mem->desc, "lock") == 0) {
    cmd[1] = MTYPE_LOCK_BITS;
  } else if (strcmp(mem->desc, "calibration") == 0) {
    cmd[1] = MTYPE_OSCCAL_BYTE;
  } else if (strcmp(mem->desc, "signature") == 0) {
    cmd[1] = MTYPE_SIGN_JTAG;
  }

  /*
   * To improve the read speed, we used paged reads for flash and
   * EEPROM, and cache the results in a page cache.
   *
   * Page cache validation is based on "{flash,eeprom}_pageaddr"
   * (holding the base address of the most recent cache fill
   * operation).  This variable is set to (unsigned long)-1L when the
   * cache needs to be invalidated.
   */
  if (pagesize && paddr == *paddr_ptr) {
    *value = cache_ptr[addr & (pagesize - 1)];
    return 0;
  }

  if (pagesize) {
    if (is_flash) {
      cmd[2] = pagesize / 2 - 1;
      u32_to_b3(cmd + 3, paddr / 2);
    } else {
      cmd[2] = pagesize - 1;
      u32_to_b3(cmd + 3, paddr);
    }
    respsize = 3 + pagesize;
  } else {
    if (cmd[1] == MTYPE_FUSE_BITS) {
      /*
       * The mkI ICE has a bug where it doesn't read efuse correctly
       * when reading it as a single byte @offset 2, while reading all
       * fuses at once does work.
       */
      cmd[2] = 3 - 1;
      u32_to_b3(cmd + 3, 0);
      respsize = 3 + 3;
    } else {
      cmd[2] = 1 - 1;
      u32_to_b3(cmd + 3, addr);
    }
  }

  jtagmkI_send(pgm, cmd, 6);
  jtagmkI_recv(pgm, resp, respsize);

  if (resp[respsize - 1] != RESP_OK) {
    if (verbose >= 2)
      putc('\n', stderr);
    fprintf(stderr,
	    "%s: jtagmkI_read_byte(): "
	    "timeout/error communicating with programmer (resp %c)\n",
	    progname, resp[respsize - 1]);
    exit(1);
  } else {
    if (verbose == 2)
      fprintf(stderr, "OK\n");
  }

  if (pagesize) {
    *paddr_ptr = paddr;
    memcpy(cache_ptr, resp + 1, pagesize);
    *value = cache_ptr[addr & (pagesize - 1)];
  } else if (cmd[1] == MTYPE_FUSE_BITS) {
    /* extract the desired fuse */
    *value = resp[1 + addr];
  } else
    *value = resp[1];

  return 0;
}

static int jtagmkI_write_byte(PROGRAMMER * pgm, AVRPART * p, AVRMEM * mem,
			       unsigned long addr, unsigned char data)
{
  unsigned char cmd[6], datacmd[1 * 2 + 1];
  unsigned char resp[1], writedata;
  int len, need_progmode = 1;

  if (verbose >= 2)
    fprintf(stderr, "%s: jtagmkI_write_byte(.., %s, 0x%lx, ...)\n",
	    progname, mem->desc, addr);

  writedata = data;
  cmd[0] = CMD_WRITE_MEM;
  if (strcmp(mem->desc, "flash") == 0) {
    cmd[1] = MTYPE_SPM;
    need_progmode = 0;
    flash_pageaddr = (unsigned long)-1L;
  } else if (strcmp(mem->desc, "eeprom") == 0) {
    cmd[1] = MTYPE_EEPROM;
    need_progmode = 0;
    eeprom_pageaddr = (unsigned long)-1L;
  } else if (strcmp(mem->desc, "lfuse") == 0) {
    cmd[1] = MTYPE_FUSE_BITS;
    addr = 0;
  } else if (strcmp(mem->desc, "hfuse") == 0) {
    cmd[1] = MTYPE_FUSE_BITS;
    addr = 1;
  } else if (strcmp(mem->desc, "efuse") == 0) {
    cmd[1] = MTYPE_FUSE_BITS;
    addr = 2;
  } else if (strcmp(mem->desc, "lock") == 0) {
    cmd[1] = MTYPE_LOCK_BITS;
  } else if (strcmp(mem->desc, "calibration") == 0) {
    cmd[1] = MTYPE_OSCCAL_BYTE;
  } else if (strcmp(mem->desc, "signature") == 0) {
    cmd[1] = MTYPE_SIGN_JTAG;
  }

  if (need_progmode) {
    if (jtagmkI_program_enable(pgm) < 0)
      return -1;
  } else {
    if (jtagmkI_program_disable(pgm) < 0)
      return -1;
  }

  cmd[2] = 1;
  if (cmd[1] == MTYPE_SPM) {
    /*
     * Flash is word-addressed, but we cannot handle flash anyway
     * here, as it needs to be written one page at a time...
     */
    u32_to_b3(cmd + 3, addr / 2);
  } else {
    u32_to_b3(cmd + 3, addr);
  }
  /* First part, send the write command. */
  jtagmkI_send(pgm, cmd, 6);
  jtagmkI_recv(pgm, resp, 1);
  if (resp[0] != RESP_OK) {
    if (verbose >= 2)
      putc('\n', stderr);
    fprintf(stderr,
	    "%s: jtagmkI_write_byte(): "
	    "timeout/error communicating with programmer (resp %c)\n",
	    progname, resp[0]);
    return -1;
  } else {
    if (verbose == 2)
      fprintf(stderr, "OK\n");
  }

  /* Now, send the data buffer. */
  datacmd[0] = CMD_DATA;
  if (cmd[1] == MTYPE_SPM) {
    len = 3;
    if ((addr & 1) != 0) {
      datacmd[1] = 0;
      datacmd[2] = writedata;
    } else {
      datacmd[1] = writedata;
      datacmd[2] = 0;
    }
  } else {
    len = 2;
    datacmd[1] = writedata;
  }
  jtagmkI_send(pgm, datacmd, len);
  jtagmkI_recv(pgm, resp, 1);
  if (resp[0] != RESP_OK) {
    if (verbose >= 2)
      putc('\n', stderr);
    fprintf(stderr,
	    "%s: jtagmkI_write_byte(): "
	    "timeout/error communicating with programmer (resp %c)\n",
	    progname, resp[0]);
    return -1;
  } else {
    if (verbose == 2)
      fprintf(stderr, "OK\n");
  }

  return 0;
}


/*
 * Set the JTAG clock.  The actual frequency is quite a bit of
 * guesswork, based on the values claimed by AVR Studio.  Inside the
 * JTAG ICE, the value is the delay count of a delay loop between the
 * JTAG clock edges.  A count of 0 bypasses the delay loop.
 *
 * As the STK500 expresses it as a period length (and we actualy do
 * program a period length as well), we rather call it by that name.
 */
static int jtagmkI_set_sck_period(PROGRAMMER * pgm, double v)
{
  unsigned char dur;

  v = 1 / v;			/* convert to frequency */
  if (v >= 1e6)
    dur = JTAG_BITRATE_1_MHz;
  else if (v >= 499e3)
    dur = JTAG_BITRATE_500_kHz;
  else if (v >= 249e3)
    dur = JTAG_BITRATE_250_kHz;
  else
    dur = JTAG_BITRATE_125_kHz;

  return jtagmkI_setparm(pgm, PARM_CLOCK, dur);
}


/*
 * Read an emulator parameter.  The result is exactly one byte,
 * multi-byte parameters get two different parameter names for
 * their components.
 */
static int jtagmkI_getparm(PROGRAMMER * pgm, unsigned char parm,
			    unsigned char * value)
{
  unsigned char buf[2], resp[3];

  if (verbose >= 2)
    fprintf(stderr, "%s: jtagmkI_getparm()\n", progname);

  buf[0] = CMD_GET_PARAM;
  buf[1] = parm;
  if (verbose >= 2)
    fprintf(stderr, "%s: jtagmkI_getparm(): "
	    "Sending get parameter command (parm 0x%02x): ",
	    progname, parm);
  jtagmkI_send(pgm, buf, 2);

  jtagmkI_recv(pgm, resp, 3);
  if (resp[0] != RESP_OK) {
    if (verbose >= 2)
      putc('\n', stderr);
    fprintf(stderr,
	    "%s: jtagmkI_getparm(): "
	    "timeout/error communicating with programmer (resp %c)\n",
	    progname, resp[0]);
    return -1;
  } else if (resp[2] != RESP_OK) {
    if (verbose >= 2)
      putc('\n', stderr);
    fprintf(stderr,
	    "%s: jtagmkI_getparm(): "
	    "unknown parameter 0x%02x\n",
	    progname, parm);
    return -1;
  } else {
    if (verbose == 2)
      fprintf(stderr, "OK, value 0x%02x\n", resp[1]);
  }

  *value = resp[1];

  return 0;
}

/*
 * Write an emulator parameter.
 */
static int jtagmkI_setparm(PROGRAMMER * pgm, unsigned char parm,
			    unsigned char value)
{
  unsigned char buf[3], resp[2];

  if (verbose >= 2)
    fprintf(stderr, "%s: jtagmkI_setparm()\n", progname);

  buf[0] = CMD_SET_PARAM;
  buf[1] = parm;
  buf[2] = value;
  if (verbose >= 2)
    fprintf(stderr, "%s: jtagmkI_setparm(): "
	    "Sending set parameter command (parm 0x%02x): ",
	    progname, parm);
  jtagmkI_send(pgm, buf, 3);
  jtagmkI_recv(pgm, resp, 2);
  if (resp[0] != RESP_OK) {
    if (verbose >= 2)
      putc('\n', stderr);
    fprintf(stderr,
	    "%s: jtagmkI_setparm(): "
	    "timeout/error communicating with programmer (resp %c)\n",
	    progname, resp[0]);
    return -1;
  } else {
    if (verbose == 2)
      fprintf(stderr, "OK\n");
  }

  return 0;
}


static void jtagmkI_display(PROGRAMMER * pgm, char * p)
{

  unsigned char hw, fw;

  if (jtagmkI_getparm(pgm, PARM_HW_VERSION, &hw) < 0 ||
      jtagmkI_getparm(pgm, PARM_SW_VERSION, &fw) < 0)
    return;

  fprintf(stderr, "%sICE hardware version: 0x%02x\n", p, hw);
  fprintf(stderr, "%sICE firmware version: 0x%02x\n", p, fw);

  jtagmkI_print_parms1(pgm, p);

  return;
}


static void jtagmkI_print_parms1(PROGRAMMER * pgm, char * p)
{
  unsigned char vtarget, jtag_clock;
  const char *clkstr;
  double clk;

  if (jtagmkI_getparm(pgm, PARM_OCD_VTARGET, &vtarget) < 0 ||
      jtagmkI_getparm(pgm, PARM_CLOCK, &jtag_clock) < 0)
    return;

  switch ((unsigned)jtag_clock) {
  case JTAG_BITRATE_1_MHz:
    clkstr = "1 MHz";
    clk = 1e6;
    break;

  case JTAG_BITRATE_500_kHz:
    clkstr = "500 kHz";
    clk = 500e3;
    break;

  case JTAG_BITRATE_250_kHz:
    clkstr = "250 kHz";
    clk = 250e3;
    break;

  case JTAG_BITRATE_125_kHz:
    clkstr = "125 kHz";
    clk = 125e3;
    break;

  default:
    clkstr = "???";
    clk = 1e6;
  }

  fprintf(stderr, "%sVtarget         : %.1f V\n", p,
	  6.25 * (unsigned)vtarget / 255.0);
  fprintf(stderr, "%sJTAG clock      : %s (%.1f us)\n", p, clkstr,
	  1.0e6 / clk);

  return;
}


static void jtagmkI_print_parms(PROGRAMMER * pgm)
{
  jtagmkI_print_parms1(pgm, "");
}


void jtagmkI_initpgm(PROGRAMMER * pgm)
{
  strcpy(pgm->type, "JTAGMKI");

  /*
   * mandatory functions
   */
  pgm->initialize     = jtagmkI_initialize;
  pgm->display        = jtagmkI_display;
  pgm->enable         = jtagmkI_enable;
  pgm->disable        = jtagmkI_disable;
  pgm->program_enable = jtagmkI_program_enable_dummy;
  pgm->chip_erase     = jtagmkI_chip_erase;
  pgm->cmd            = jtagmkI_cmd;
  pgm->open           = jtagmkI_open;
  pgm->close          = jtagmkI_close;

  /*
   * optional functions
   */
  pgm->paged_write    = jtagmkI_paged_write;
  pgm->paged_load     = jtagmkI_paged_load;
  pgm->read_byte      = jtagmkI_read_byte;
  pgm->write_byte     = jtagmkI_write_byte;
  pgm->print_parms    = jtagmkI_print_parms;
  pgm->set_sck_period = jtagmkI_set_sck_period;
  pgm->page_size      = 256;
}
