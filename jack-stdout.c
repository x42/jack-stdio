/** jack-stdout  - write JACK audio data to stdout.
 *
 * This tool is based on capture_client.c from the jackaudio.org examples
 * and modified by Robin Gareus <robin@gareus.org>
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
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 * 
 * Copyright (C) 2001 Paul Davis
 * Copyright (C) 2003 Jack O'Quin
 * Copyright (C) 2008, 2011 Robin Gareus
 * 
 * compile with
 *   gcc -o jack-stdout jack-stdout.c -ljack -lm -lpthread
 * 
 * example use: 
 *   jack-stdout xmms_0:out_1 xmms_0:out_2 \
 *   | mono  ~/Desktop/Downloads/JustePort.exe - 10.0.1.6 0
 * 
 *   jack-stdout system:capture_1 system:capture_2 \
 *   | oggenc -r -R 48000 -B 16 -C 2 - \
 *   > /tmp/my.ogg
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <getopt.h>
#include <signal.h>
#include <math.h>
#include <jack/jack.h>
#include <jack/ringbuffer.h>

typedef struct _thread_info {
	pthread_t thread_id;
	pthread_t mesg_thread_id;
	jack_nframes_t duration;
	jack_nframes_t rb_size;
	jack_client_t *client;
	unsigned int channels;
	volatile int can_capture;
	volatile int can_process;
	int format; 
	/**format:
	 * bit0,1: 16/24/8/32(float)
	 * bit8:   signed/unsiged (0x10)
	 * bit9:   int/float      (0x20) - dup -- 0x03
	 * bit10:  little/big endian  (0x40)
	 */
} jack_thread_info_t;

#define IS_FMTFLT ((info->format&0x20))
#define IS_BIGEND (info->format&0x40)
#define IS_FMT32B ((info->format&0x23)==3)
#define IS_FMT08B ((info->format&3)==2)
#define IS_FMT24B ((info->format&3)==1)
#define IS_FMT16B ((info->format&3)==0)
#define IS_SIGNED (!(info->format&0x10))

#define SAMPLESIZE ((info->format&2)?((info->format&1)?4:1):((info->format&1)?3:2))
#define POWHX ((info->format&2)?((info->format&1)?2147483647:127):((info->format&1)? 8388607:32767))
#define POWHS ((info->format&2)?((info->format&1)?2147483648.0:128.0):((info->format&1)? 8388608.0:32768.0))

#define FMTOFF ((IS_SIGNED)?0.0:POWHX)
#define FMTMLT (POWHS)

#ifdef __BIG_ENDIAN__
#define BE(i) (!(IS_BIGEND)?(SAMPLESIZE-i-1):i)
#else
#define BE(i) ( (IS_BIGEND)?(SAMPLESIZE-i-1):i)
#endif

/* JACK data */
jack_port_t **ports;
jack_default_audio_sample_t **in;
jack_nframes_t nframes;

/* Synchronization between process thread and disk thread. */
jack_ringbuffer_t *rb;
pthread_mutex_t io_thread_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  data_ready = PTHREAD_COND_INITIALIZER;

/* global options/status */
int want_quiet = 0;
int run = 1;
long overruns = 0;

void * io_thread (void *arg) {
	jack_thread_info_t *info = (jack_thread_info_t *) arg;

	jack_nframes_t total_captured = 0;
	const size_t bytes_per_frame = info->channels * SAMPLESIZE;
	void *framebuf = malloc (bytes_per_frame);

	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
	pthread_mutex_lock(&io_thread_lock);

	int writerrors =0;

	while (run) {
		/* Write the data one frame at a time.  This is
		 * inefficient, but makes things simpler. */
		while (info->can_capture &&
		       (jack_ringbuffer_read_space (rb) >= bytes_per_frame)) {

			if (info->duration > 0 && total_captured >= info->duration) {
				if (!want_quiet)
					fprintf(stderr, "io thread finished\n");
				goto done;
			}

			#if 0 /* wait (indefinitley) for write-ready */
			fd_set fd;
			FD_ZERO(&fd);
			FD_SET(fileno(stdout), &fd);
			select(fileno(stdout), &fd, NULL, NULL, NULL);
			#endif

			jack_ringbuffer_read(rb, framebuf, bytes_per_frame);
			while (write(fileno(stdout) , framebuf, bytes_per_frame) != bytes_per_frame)
			{
				if (++writerrors>5) 
				{ 
					if (!want_quiet)
						fprintf(stderr, "FATAL: write error.\n");
						writerrors=0;
						break;
				}
				/* retry */
				//fprintf(stderr, "buffer not emptied: %i|%i\n", jack_ringbuffer_read_space(rb), jack_ringbuffer_write_space(rb));
				
				#if 0
				/* heck this thread can just block.. */
				fd_set fd;
				struct timeval tv = { 0, 0 };
				tv.tv_sec = 0; tv.tv_usec = 1000; // 1ms
				FD_ZERO(&fd);
				FD_SET(fileno(stdout), &fd);
				select(fileno(stdout), &fd, NULL, NULL, &tv);
				#endif
			}
			++total_captured;
		}
		pthread_cond_wait(&data_ready, &io_thread_lock);
	}

done:
	pthread_mutex_unlock(&io_thread_lock);
	free(framebuf);
	return 0;
}
	
int process (jack_nframes_t nframes, void *arg) {
	int chn;
	size_t i;
	jack_thread_info_t *info = (jack_thread_info_t *) arg;
	const size_t bytes_per_frame = info->channels * SAMPLESIZE;

	/* Do nothing until we're ready to begin. */
	if ((!info->can_process) || (!info->can_capture))
		return 0;

	for (chn = 0; chn < info->channels; ++chn)
		in[chn] = jack_port_get_buffer(ports[chn], nframes);

	/* queue interleaved samples to a single ringbuffer. */
	for (i = 0; i < nframes; i++) {
		/* only queue samples if a whole frame (all channels) can be stored */
		if (jack_ringbuffer_write_space(rb) < bytes_per_frame ) {
			overruns++;
			break;
		}

		for (chn = 0; chn < info->channels; ++chn) {
			const jack_default_audio_sample_t js = in[chn][i];
#if 0
			/* convert to 16 bit signed LE */
			int16_t d = (int16_t) rintf(js*32767.0);
			char twobyte[2];
			twobyte[0] = (unsigned char) (d&0xff);
			twobyte[1] = (unsigned char) (((d&0xff00)>>8)&0xff);
			jack_ringbuffer_write (rb, (void *) &twobyte, SAMPLESIZE);
#else
			if (IS_FMTFLT) { 
				/* 32 bit float */
				float d;
				if (IS_BIGEND) {
					/* swap float endianess */
					char *flin = (char*) & js;
					char *fout = (char*) & d;
					fout[0] = flin[3];
					fout[1] = flin[2];
					fout[2] = flin[1];
					fout[3] = flin[0];
				} else {
				  d = js;
				}
				jack_ringbuffer_write(rb, (void *) &d, SAMPLESIZE);
			} else {
				/* 8/16/24 LE/BE signed/unsigned */
				const int32_t d = (int32_t) rintf(js*FMTMLT) + FMTOFF;
				char bytes[4];
				bytes[BE(0)] = (unsigned char) (d&0xff);
				if (IS_FMT16B || IS_FMT24B || IS_FMT32B)
					bytes[BE(1)] = (unsigned char) (((d&0xff00)>>8)&0xff);
				if (IS_FMT24B || IS_FMT32B)
					bytes[BE(2)] = (unsigned char) (((d&0xff0000)>>16)&0xff);
				if (IS_FMT32B)
					bytes[BE(3)] = (unsigned char) (((d&0xff000000)>>24)&0xff);
				jack_ringbuffer_write (rb, (void *) &bytes, SAMPLESIZE);
			}
#endif
		}
	}
	/* Tell the io thread there is work to do. */ 
	if (pthread_mutex_trylock(&io_thread_lock) == 0) {
	    pthread_cond_signal(&data_ready);
	    pthread_mutex_unlock(&io_thread_lock);
	}

	return 0;
}

void jack_shutdown (void *arg) {
	fprintf(stderr, "JACK shutdown\n");
	abort();
}

void setup_ports (int nports, char *source_names[], jack_thread_info_t *info) {
	unsigned int i;
	const size_t in_size =  nports * sizeof(jack_default_audio_sample_t *);

	/* Allocate data structures that depend on the number of ports. */
	ports = (jack_port_t **) malloc(sizeof(jack_port_t *) * nports);
	in = (jack_default_audio_sample_t **) malloc(in_size);
	rb = jack_ringbuffer_create(nports * SAMPLESIZE * info->rb_size);

	/* When JACK is running realtime, jack_activate() will have
	 * called mlockall() to lock our pages into memory.  But, we
	 * still need to touch any newly allocated pages before
	 * process() starts using them.  Otherwise, a page fault could
	 * create a delay that would force JACK to shut us down. */
	memset(in, 0, in_size);
	memset(rb->buf, 0, rb->size);

	for (i = 0; i < nports; i++) {
		char name[64];

		sprintf(name, "input%d", i+1);

		if ((ports[i] = jack_port_register(info->client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0)) == 0) {
			fprintf(stderr, "cannot register input port \"%s\"!\n", name);
			jack_client_close(info->client);
			exit(1);
		}
	}

	for (i = 0; i < nports; i++) {
		if (jack_connect(info->client, source_names[i], jack_port_name(ports[i]))) {
			fprintf(stderr, "cannot connect input port %s to %s\n", jack_port_name(ports[i]), source_names[i]);
#if 0 /* not fatal - connect manually */
			jack_client_close(info->client);
			exit(1);
#endif
		} 
	}

	/* process() can start, now */
	info->can_process = 1;
}

void catchsig (int sig) {
#ifndef _WIN32
	signal(SIGHUP, catchsig); /* reset signal */
#endif
	if (!want_quiet)
		fprintf(stderr,"\n CAUGHT SIGNAL - shutting down.\n");
	run=0;
	/* signal writer thread */
	pthread_mutex_lock(&io_thread_lock);
	pthread_cond_signal(&data_ready);
	pthread_mutex_unlock(&io_thread_lock);
}


static void usage (const char *name, int status) {
	fprintf(status?stderr:stdout, 
		"usage: %s [ OPTIONS ] port1 [ port2 ... ]\n", name);
	fprintf(status?stderr:stdout, 
		"jack-stdout captures audio-data from JACK and writes it to standard-output.\n");
	fprintf(status?stderr:stdout, 
	  "OPTIONS:\n"
	  " -h, --help               print this message\n"
	  " -q, --quiet              inhibit usual output\n"
	  " -b, --bitdepth {bits}    choose integer bit depth: 16, 24 (default: 16)\n"
	  " -d, --duration {sec}     terminate after given time, <1: unlimited (default:0)\n"
	  " -e, --encoding {format}  set output format: (default: signed)\n"
		"                          signed-integer, unsigned-integer, float\n"
	  " -L, --little-endian      write little-endian integers or\n"
		"                          native-byte-order floats (default)\n"
	  " -B, --big-endian         write big-endian integers or swapped-order floats\n"
	  " -S, --bufsize {samples}  set buffer size (default: 64k)\n"
		);
	exit(status);
}

int main (int argc, char **argv) {
	jack_client_t *client;
	jack_thread_info_t thread_info;
	jack_status_t jstat;
	int c;

	memset(&thread_info, 0, sizeof(thread_info));
	thread_info.rb_size = 16384 * 4; //< make this an option
	thread_info.channels = 2;
	thread_info.duration = 0;
	thread_info.format = 0;

	const char *optstring = "d:e:b:S:BLhq";
	struct option long_options[] = {
		{ "help", 0, 0, 'h' },
		{ "quiet", 0, 0, 'q' },
		{ "duration", 1, 0, 'd' },
		{ "encoding", 1, 0, 'e' },
		{ "little-endian", 0, 0, 'L' },
		{ "big-endian", 0, 0, 'B' },
		{ "bitdepth", 1, 0, 'b' },
		{ "bufsize", 1, 0, 'S' },
		{ 0, 0, 0, 0 }
	};

	while ((c = getopt_long(argc, argv, optstring, long_options, NULL)) != -1) {
		switch (c) {
			case 'h':
				usage(argv[0], 0);
				break;
			case 'q':
				want_quiet = 1;
				break;
			case 'd':
				thread_info.duration = atoi(optarg);
				break;
			case 'e':
				thread_info.format&=~0x30;
				if (!strncmp(optarg, "floating-point", strlen(optarg)))
					thread_info.format|=0x23;
				else if (!strncmp(optarg, "unsigned-integer", strlen(optarg)))
					thread_info.format|=0x10;
				else if (!strncmp(optarg, "signed-integer", strlen(optarg))) 
					;
				else {
					fprintf(stderr, "invalid encoding.\n");
					usage(argv[0], 1);
				}
				break;
			case 'b':
				thread_info.format&=~3;
				if (atoi(optarg) == 24) 
					thread_info.format|=1;
				else if (atoi(optarg) == 8) 
					thread_info.format|=2;
				else if (atoi(optarg) == 32) 
					thread_info.format|=3;
				else if (atoi(optarg) != 16) {
					fprintf(stderr, "invalid integer bit-depth. valid values: 8, 16, 24, 32.\n");
					usage(argv[0], 1);
				}
				break;
			case 'L':
				thread_info.format&=~0x40;
				break;
				thread_info.rb_size = atoi(optarg);
			case 'B':
				thread_info.format|=0x40;
				break;
			case 'S':
				thread_info.rb_size = atoi(optarg);
				break;
			default:
				fprintf(stderr, "invalid argument.\n");
				usage(argv[0], 0);
			break;
		}
	}

	/* sanity checks */
	if (thread_info.format & 0x20) {
		/* float is always 32 bit */
		thread_info.format|=3; 
	}

	if (thread_info.rb_size < 16) {
		fprintf(stderr, "Ringbuffer size needs to be at least 16 samples\n");
		usage(argv[0], 1);
	}

	if (argc <= optind) {
		fprintf(stderr, "At least one port/audio-channel must be given.\n");
		usage(argv[0], 1);
	}

	/* set up JACK client */
	if ((client = jack_client_open("jstdout", JackNoStartServer, &jstat)) == 0) {
		fprintf(stderr, "Can not connect to JACK.\n");
		exit(1);
	}

	thread_info.client = client;
	thread_info.can_process = 0;
	thread_info.channels = argc - optind;

	if (thread_info.duration > 0) {
		thread_info.duration *= jack_get_sample_rate(thread_info.client);
	}

	if (!want_quiet) {
		fprintf(stderr, "%i channel%s, %s %sbit %s%s %s @%iSPS.\n",
			thread_info.channels, 
			(thread_info.channels>1)?"s":"",
			(thread_info.channels>1)?"interleaved":"",
			(thread_info.format&2)?(thread_info.format&1?"32":"8"):(thread_info.format&1?"24":"16"),
			(thread_info.format&0x20)?"":(thread_info.format&0x10?"unsigned-":"signed-"),
			(thread_info.format&0x20)?"float":"integer",
			(thread_info.format&0x20)?
				(thread_info.format&0x40?"non-native-endian":"native-endian"):
				(thread_info.format&0x40?"big-endian":"little-endian"),
		  jack_get_sample_rate(thread_info.client)
				);
	}

	jack_set_process_callback(client, process, &thread_info);
	jack_on_shutdown(client, jack_shutdown, &thread_info);

	if (jack_activate(client)) {
		fprintf(stderr, "cannot activate client");
	}

	setup_ports(thread_info.channels, &argv[optind], &thread_info);

	/* set up i/o thread */
	pthread_create(&thread_info.thread_id, NULL, io_thread, &thread_info);
#ifndef _WIN32
	signal (SIGHUP, catchsig);                                                                                                                   
#endif

	/* all systems go - run the i/o thread */
	thread_info.can_capture = 1;
	pthread_join(thread_info.thread_id, NULL);

	/* end - clean up */
	if (overruns > 0 && !want_quiet) {
		fprintf(stderr, "Note: there were %ld buffer overruns.\n", overruns);
	}
	jack_client_close(client);
	jack_ringbuffer_free(rb);
	return(0);
}
