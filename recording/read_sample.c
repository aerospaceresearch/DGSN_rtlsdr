#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>

#include "rtl-sdr.h"
#include "convenience/convenience.h"


#define MAX_LINE_SIZE 40
#define MAX_ARG_SIZE 20
#define DEFAULT_SAMPLE_RATE             2048000
#define DEFAULT_ASYNC_BUF_NUMBER        32
#define DEFAULT_BUF_LENGTH              (16 * 16384)
#define MINIMAL_BUF_LENGTH              512
#define MAXIMAL_BUF_LENGTH              (256 * 16384)

static int do_exit = 0;
static uint32_t bytes_to_read = 0;
static rtlsdr_dev_t *dev = NULL;


#ifdef _WIN32
BOOL WINAPI
sighandler(int signum)
{
        if (CTRL_C_EVENT == signum) {
                fprintf(stderr, "Signal caught, exiting!\n");
                do_exit = 1;
                rtlsdr_cancel_async(dev);
                return TRUE;
        }
        return FALSE;
}
#else
static void sighandler(int signum)
{
        fprintf(stderr, "Signal caught, exiting!\n");
        do_exit = 1;
        rtlsdr_cancel_async(dev);
}
#endif

static void rtlsdr_callback(unsigned char *buf, uint32_t len, void *ctx)
{
        if (ctx) {
                if (do_exit)
                        return;

                if ((bytes_to_read > 0) && (bytes_to_read < len)) {
                        len = bytes_to_read;
                        do_exit = 1;
                        rtlsdr_cancel_async(dev);
                }

                if (fwrite(buf, 1, len, (FILE*)ctx) != len) {
                        fprintf(stderr, "Short write, samples lost, exiting!\n");
                        rtlsdr_cancel_async(dev);
                }

                if (bytes_to_read > 0)
                        bytes_to_read -= len;
        }
}


int main(int argc, char *argv[]){
	

#ifndef _WIN32
	struct sigaction sigact;
#endif
	FILE *fp;
	char line[MAX_LINE_SIZE];
	char arg[MAX_ARG_SIZE];
	char equal_sign = '=';

	if (argc == 2) fp = fopen(argv[1], "r");
	else fp = fopen("data/sample_.txt", "r");

	if (fp == NULL){
		perror("Error while opening file.\n");
		exit(EXIT_FAILURE);
	}

	int str_offset;

	int index = 0;

	char *filename = NULL;
        int n_read;
        int r, opt;
        int i, gain = 0;
        int ppm_error = 0;
        int sync_mode = 0;
        int direct_sampling = 0;
        FILE *file;
        uint8_t *buffer;
        int dev_index = 0;
        int dev_given = 0;
        uint32_t frequency = 100000000;
        uint32_t samp_rate = DEFAULT_SAMPLE_RATE;
        uint32_t out_block_size = DEFAULT_BUF_LENGTH;
	int recording_time;

	while (!feof(fp)){
		// NEED TO ADD COMMENT LINE
		fgets(line, MAX_LINE_SIZE, fp);
		str_offset = strchr(line, equal_sign) - line;
		strncpy(arg, line + str_offset + 1, strlen(line) - str_offset);
		strtok(arg, "\n");
		if (strlen(arg) > 1){
			switch (index){
				printf("argument = %s\n", arg);
				case 1: 
					frequency = (uint32_t)atofs(arg);
					break;
				case 2: 
					samp_rate = (uint32_t)atofs(arg);
					break;
				case 3:
					dev_index = verbose_device_search(arg);
					dev_given = 1;
					break;
				case 4:
					gain = (int)(atof(arg) * 10); /* tenths of a dB */
					break;
				case 5: 
					ppm_error = atoi(arg);
					break;
				case 6:
					out_block_size = (uint32_t)atof(arg);
					break;
				case 7:
					bytes_to_read = (uint32_t)atof(arg) * 2;
					break;
				case 8:
					sync_mode = 1;
					break;
				case 9:
					direct_sampling = atoi(arg);
					break;
				case 10:
					filename = arg;
					break;
				case 0:
					recording_time = (uint32_t)atofs(arg);
					break;
			}
		}

		else filename = "-";
		index++;
	}


	if(out_block_size < MINIMAL_BUF_LENGTH ||
           out_block_size > MAXIMAL_BUF_LENGTH ){
                fprintf(stderr,
                        "Output block size wrong value, falling back to default\n");
                fprintf(stderr,
                        "Minimal length: %u\n", MINIMAL_BUF_LENGTH);
                fprintf(stderr,
                        "Maximal length: %u\n", MAXIMAL_BUF_LENGTH);
                out_block_size = DEFAULT_BUF_LENGTH;
        }


	buffer = malloc(out_block_size * sizeof(uint8_t));

        if (!dev_given) {
                dev_index = verbose_device_search("0");
        }

        if (dev_index < 0) {
                exit(1);
        }

        r = rtlsdr_open(&dev, (uint32_t)dev_index);
        if (r < 0) {
                fprintf(stderr, "Failed to open rtlsdr device #%d.\n", dev_index);
                exit(1);
        }
#ifndef _WIN32
        sigact.sa_handler = sighandler;
        sigemptyset(&sigact.sa_mask);
        sigact.sa_flags = 0;
        sigaction(SIGINT, &sigact, NULL);
        sigaction(SIGTERM, &sigact, NULL);
        sigaction(SIGQUIT, &sigact, NULL);
        sigaction(SIGPIPE, &sigact, NULL);
#else
        SetConsoleCtrlHandler( (PHANDLER_ROUTINE) sighandler, TRUE );
#endif


	if (direct_sampling) {
                verbose_direct_sampling(dev, direct_sampling);
        }

        /* Set the sample rate */
        verbose_set_sample_rate(dev, samp_rate);

        /* Set the frequency */
        verbose_set_frequency(dev, frequency);

        if (0 == gain) {
                 /* Enable automatic gain */
                verbose_auto_gain(dev);
        } else {
                /* Enable manual gain */
                gain = nearest_gain(dev, gain);
                verbose_gain_set(dev, gain);
        }

        verbose_ppm_set(dev, ppm_error);

        if(strcmp(filename, "-") == 0) { /* Write samples to stdout */
                file = stdout;
#ifdef _WIN32
                _setmode(_fileno(stdin), _O_BINARY);
#endif
        } else {
                file = fopen(filename, "wb");
		printf("%s\n", filename);
                if (!file) {
                        fprintf(stderr, "Failed to open %s\n", filename);
                        goto out;
                }
        }

        /* Reset endpoint before we start reading from it (mandatory) */
        verbose_reset_buffer(dev);

        if (sync_mode) {
                fprintf(stderr, "Reading samples in sync mode...\n");
                while (!do_exit) {
                        r = rtlsdr_read_sync(dev, buffer, out_block_size, &n_read);
                        if (r < 0) {
                                fprintf(stderr, "WARNING: sync read failed.\n");
                                break;
                        }

                        if ((bytes_to_read > 0) && (bytes_to_read < (uint32_t)n_read)) {
                                n_read = bytes_to_read;
                                do_exit = 1;
                        }

                        if (fwrite(buffer, 1, n_read, file) != (size_t)n_read) {
                                fprintf(stderr, "Short write, samples lost, exiting!\n");
                                break;
                        }

                        if ((uint32_t)n_read < out_block_size) {
                                fprintf(stderr, "Short read, samples lost, exiting!\n");
                                break;
                        }

                        if (bytes_to_read > 0)
                                bytes_to_read -= n_read;
                }
        } else {
                fprintf(stderr, "Reading samples in async mode...\n");
                r = rtlsdr_read_async_timeout(dev, rtlsdr_callback, (void *)file,
                                      DEFAULT_ASYNC_BUF_NUMBER, out_block_size);
		printf("finishes recording...\n");
        }

        if (do_exit)
                fprintf(stderr, "\nUser cancel, exiting...\n");
        else if (r != 5)
                fprintf(stderr, "\nLibrary error %d, exiting...\n", r);

        if (file != stdout)
                fclose(file);


        //rtlsdr_close(dev);
        free (buffer);


out:
	
	//printf("Test ended\n");
	return r >= 0 ? r : -r;
	
}
