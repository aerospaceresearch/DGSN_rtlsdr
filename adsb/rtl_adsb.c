/*
 * rtl-sdr, turns your Realtek RTL2832 based DVB dongle into a SDR receiver
 * Copyright (C) 2012 by Steve Markgraf <steve@steve-m.de>
 * Copyright (C) 2012 by Hoernchen <la@tfc-server.de>
 * Copyright (C) 2012 by Kyle Keen <keenerd@gmail.com>
 * Copyright (C) 2012 by Youssef Touil <youssef@sdrsharp.com>
 * Copyright (C) 2012 by Ian Gilmour <ian@sdrsharp.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#ifndef _WIN32
#include <unistd.h>
#else
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include "getopt/getopt.h"
#endif

#include <pthread.h>
#include <libusb.h>

#include "rtl-sdr.h"
#include "convenience/convenience.h"

#ifdef _WIN32
#define sleep Sleep
#define round(x) (x > 0.0 ? floor(x + 0.5): ceil(x - 0.5))
#endif

#define ADSB_RATE			2000000
#define ADSB_FREQ			1090000000
#define DEFAULT_ASYNC_BUF_NUMBER	12
#define DEFAULT_BUF_LENGTH		(16 * 16384)
#define AUTO_GAIN			-100

#define MESSAGEGO    253
#define OVERWRITE    254
#define BADSAMPLE    255

#define MODES_NOTUSED(V) ((void) V)

static pthread_t demod_thread;
static pthread_cond_t ready;
static pthread_mutex_t ready_m;
static volatile int do_exit = 0;

uint16_t squares[256];

/* todo, bundle these up in a struct */
uint8_t *buffer;  /* also abused for uint16_t */
int verbose_output = 0;
int short_output = 0;
int quality = 10;
int allowed_errors = 5;
FILE *file;
int adsb_frame[14];
int fd;
#define preamble_len		16
#define long_frame		112
#define short_frame		56

/* signals are not threadsafe by default */
#define safe_cond_signal(n, m) pthread_mutex_lock(m); pthread_cond_signal(n); pthread_mutex_unlock(m)
#define safe_cond_wait(n, m) pthread_mutex_lock(m); pthread_cond_wait(n, m); pthread_mutex_unlock(m)

void usage(void)
{
        fprintf(stderr,
                "rtl_adsb, a simple ADS-B decoder for pre-recorded IQ data\n\n"
                "Usage:\t\n"
		"\t[-h display this message]\n"
                "\t[-infile input file (default: rtl_out)]\n"
                "\t[-V verbose output (default: off)]\n"
                "\t[-outfile output file (default: stdout) ]\n\n"
	);
}  

void display(int *frame, int len)
{

        int i, df;
        if (!short_output && len <= short_frame) {
                return;}
	
        df = (frame[0] >> 3) & 0x1f;
	
        if (quality == 0 && !(df==11 || df==17 || df==18 || df==19)) {
                return;}
	
        fprintf(file, "*");
	
        for (i=0; i<((len+7)/8); i++) {
                fprintf(file, "%02x", frame[i]);}
        fprintf(file, ";\r\n");
        if (!verbose_output) {
                return;}
        fprintf(file, "DF=%i CA=%i\n", df, frame[0] & 0x07);
        fprintf(file, "ICAO Address=%06x\n", frame[1] << 16 | frame[2] << 8 | frame[3]);
        if (len <= short_frame) {
                return;}
	printf("???\n");
        fprintf(file, "PI=0x%06x\n",  frame[11] << 16 | frame[12] << 8 | frame[13]);
        fprintf(file, "Type Code=%i S.Type/Ant.=%x\n", (frame[4] >> 3) & 0x1f, frame[4] & 0x07);
        fprintf(file, "--------------\n");
}

int abs8(int x)
/* do not subtract 127 from the raw iq, this handles it */
{
        if (x >= 127) {
                return x - 127;}
        return 127 - x;
}

void squares_precompute(void)
/* equiv to abs(x-128) ^ 2 */
{
        int i, j;
        // todo, check if this LUT is actually any faster
        for (i=0; i<256; i++) {
                j = abs8(i);
                squares[i] = (uint16_t)(j*j);
        }
}

int magnitute(uint8_t *buf, int len)
/* takes i/q, changes buf in place (16 bit), returns new len (16 bit) */
{

        int i;
        uint16_t *m;
        for (i=0; i<len; i+=2) {
                m = (uint16_t*)(&buf[i]);
                *m = squares[buf[i]] + squares[buf[i+1]];
        }

	return len/2;
}


inline uint16_t single_manchester(uint16_t a, uint16_t b, uint16_t c, uint16_t d)
/* takes 4 consecutive real samples, return 0 or 1, BADSAMPLE on error */
{
        int bit, bit_p;
        bit_p = a > b;
        bit   = c > d;

        if (quality == 0) {
                return bit;}

        if (quality == 5) {
                if ( bit &&  bit_p && b > c) {
                        return BADSAMPLE;}
                if (!bit && !bit_p && b < c) {
                        return BADSAMPLE;}
                return bit;
        }

        if (quality == 10) {
                if ( bit &&  bit_p && c > b) {
                        return 1;}
                if ( bit && !bit_p && d < b) {
                        return 1;}
                if (!bit &&  bit_p && d > b) {
                        return 0;}
                if (!bit && !bit_p && c < b) {
                        return 0;}
                return BADSAMPLE;
        }

        if ( bit &&  bit_p && c > b && d < a) {
                return 1;}
        if ( bit && !bit_p && c > a && d < b) {
                return 1;}
        if (!bit &&  bit_p && c < a && d > b) {
                return 0;}
        if (!bit && !bit_p && c < b && d > a) {
                return 0;}
        return BADSAMPLE;
}



inline uint16_t min16(uint16_t a, uint16_t b)
{
        return a<b ? a : b;
}

inline uint16_t max16(uint16_t a, uint16_t b)
{
        return a>b ? a : b;
}

inline int preamble(uint16_t *buf, int i)
/* returns 0/1 for preamble at index i */
{
        int i2;
        uint16_t low  = 0;
        uint16_t high = 65535;
        for (i2=0; i2<preamble_len; i2++) {
                switch (i2) {
                        case 0:
                        case 2:
                        case 7:
                        case 9:
                                //high = min16(high, buf[i+i2]);
                                high = buf[i+i2];
                                break;
                        default:
                                //low  = max16(low,  buf[i+i2]);
                                low = buf[i+i2];
                                break;
                }
                if (high <= low) {
                        return 0;}
        }
        return 1;
}



void manchester(uint16_t *buf, int len)
/* overwrites magnitude buffer with valid bits (BADSAMPLE on errors) */
{
        /* a and b hold old values to verify local manchester */
        uint16_t a=0, b=0;
        uint16_t bit;
        int i, i2, start, errors;
        int maximum_i = len - 1;        // len-1 since we look at i and i+1
        // todo, allow wrap across buffers
        i = 0;
        while (i < maximum_i) {
                /* find preamble */
                for ( ; i < (len - preamble_len); i++) {
                        if (!preamble(buf, i)) {
                                continue;}
                        a = buf[i];
                        b = buf[i+1];
                        for (i2=0; i2<preamble_len; i2++) {
                                buf[i+i2] = MESSAGEGO;}
                        i += preamble_len;
                        break;
                }
                i2 = start = i;
                errors = 0;
                /* mark bits until encoding breaks */
                for ( ; i < maximum_i; i+=2, i2++) {
                        bit = single_manchester(a, b, buf[i], buf[i+1]);
                        a = buf[i];
                        b = buf[i+1];
                        if (bit == BADSAMPLE) {
                                errors += 1;
                                if (errors > allowed_errors) {
                                        buf[i2] = BADSAMPLE;
                                        break;
                                } else {
                                        bit = a > b;
                                        /* these don't have to match the bit */
                                        a = 0;
                                        b = 65535;
                                }
                        }
                        buf[i] = buf[i+1] = OVERWRITE;
                        buf[i2] = bit;
                }
        }
}


void messages(uint16_t *buf, int len)
{
        int i, i2, start;
        int data_i, index, shift, frame_len;
        // todo, allow wrap across buffers
        for (i=0; i<len; i++) {
                if (buf[i] > 1) {
                        continue;}
                frame_len = long_frame;
                data_i = 0;
                for (index=0; index<14; index++) {
                        adsb_frame[index] = 0;}
                for(; i<len && buf[i]<=1 && data_i<frame_len; i++, data_i++) {
                        if (buf[i]) {
                                index = data_i / 8;
                                shift = 7 - (data_i % 8);
                                adsb_frame[index] |= (uint8_t)(1<<shift);
                        }
                        if (data_i == 7) {
                                if (adsb_frame[0] == 0) {
                                    break;}
                                if (adsb_frame[0] & 0x80) {
                                        frame_len = long_frame;}
                                else {
                                        frame_len = short_frame;}
                        }
                }
		//printf("data_i = %d, frame_len - 1 = %d\n", data_i, frame_len - 1);
                if (data_i < (frame_len-1)) {
                        continue;}
		//printf("gets here?\n");
                display(adsb_frame, frame_len);
                fflush(file);
        }
}

void readDataFromFile(void) {
	int len;
        while (do_exit == 0){
                int status = read(fd, buffer, DEFAULT_BUF_LENGTH);
                if (status <= 0) do_exit = 1;
		len = magnitute(buffer, DEFAULT_BUF_LENGTH);
                manchester((uint16_t *)buffer, len);
                messages((uint16_t *)buffer, len);
        }
}

void *readerThreadEntryPoint(void *arg) {

        pthread_mutex_lock(&ready_m);
        while (!do_exit){
           	readDataFromFile();
        }
        pthread_mutex_unlock(&ready_m);
}


int main(int argc, char **argv)
{
    int opt;
    file = stdout;

    usage();

    for (opt = 1; opt < argc; opt++){
	int more = opt + 1 < argc;
	
	if (!strcmp(argv[opt], "-h")){
		usage();
		exit(0);
	}
	else if (!strcmp(argv[opt], "-infile") && more){
		fd = open(argv[++opt], O_RDONLY);
	}
	else if (!strcmp(argv[opt], "-V")){
		verbose_output = 1;
	}
	else if (!strcmp(argv[opt], "-outfile") && more){
		file = fopen(argv[++opt], "wb");
	}
    }

    if (fd == -1){
	fprintf(stderr, "error opening input file\n");
	exit(1);
    }

    if (file == NULL){
	fprintf(stderr, "error opening output file\n");
	exit(1);
    }

    squares_precompute();
    buffer = malloc( DEFAULT_BUF_LENGTH * sizeof(unsigned char));
    pthread_cond_init(&ready, NULL);
    pthread_mutex_init(&ready_m, NULL);

    fd = open("../data/rtl_out", O_RDONLY);

    pthread_create(&demod_thread, NULL, readerThreadEntryPoint, NULL);

    pthread_mutex_lock(&ready_m);

    pthread_cond_signal(&ready);
    pthread_mutex_unlock(&ready_m);

    pthread_cond_destroy(&ready);     // Thread cleanup
    pthread_mutex_destroy(&ready_m);
    
#ifndef _WIN32
    pthread_exit(0);
#else
    return (0);
#endif
}


