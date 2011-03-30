/** jack-stdin  - write raw audio data from stdin to JACK
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
 *   gcc -o jack-stdin jack-stdin.c -ljack -lm -lpthread
 * 
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
	int readfd;
	int format; 
	/**format:
	 * bit0: 16/24 ; bit1: signed/unsiged, bit2: little/big endian || 8: float 
	 * IOW: 
	 * little-endian: 0: S16, 1: S24, 2: U16, 3: U24,
	 * big-endian:    4:S16, 5: S24, 6: U16, 7: U24,
	 *                8: float (machine native endianess) , 12: float (swapped endianness)
	 */
} jack_thread_info_t;

#define BE(i) ((info->format&4)?((((info->format&1))?2:1)-i):i)
#define FMTMLT ((info->format&1)?8388608.0:32767.0)
#define FMTOFF ((info->format&2)?((info->format&1)?4194304:16384):0)
#define SAMPLESIZE ((info->format&8)?4:((info->format&1)?3:2))

#define IS_FMT32B (info->format&8)
#define IS_BIGEND (info->format&4)

/* JACK data */
jack_port_t **ports;
jack_default_audio_sample_t **out;
jack_nframes_t nframes;

/* Synchronization between process thread and disk thread. */
jack_ringbuffer_t *rb;
pthread_mutex_t io_thread_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  data_ready = PTHREAD_COND_INITIALIZER;

/* global options/status */
int want_quiet = 0;
int run = 1;
long underruns = 0;

void * io_thread (void *arg) {
	jack_thread_info_t *info = (jack_thread_info_t *) arg;

	jack_nframes_t total_captured = 0;
	const size_t bytes_per_frame = info->channels * SAMPLESIZE;
	void *framebuf = malloc (bytes_per_frame);

	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
	pthread_mutex_lock(&io_thread_lock);

	int readerror =0;

	while (run && !readerror) {
		/* Read the data one frame at a time.  This is
		 * inefficient, but makes things simpler. */
		while (info->can_capture &&
		       (jack_ringbuffer_write_space (rb) >= bytes_per_frame)) {

			if (info->duration > 0 && total_captured >= info->duration) {
				printf("disk thread finished\n");
				goto done;
			}

			#if 0 /* wait (indefinitley) for read-ready */
			fd_set fd;
			FD_ZERO(&fd);
			FD_SET(fileno(info->fd), &fd);
			select(fileno(info->fd), &fd, NULL, NULL, NULL);
			#endif

			int rv = read(info->readfd, framebuf, bytes_per_frame);

			if (rv < 0) {readerror=1; break;} // Error
			if (rv == 0) {readerror=1; break;} // EOF
			if (rv != bytes_per_frame) {readerror=1; break;} // XXX queue ...
			//fprintf(stderr, "read..\n");

			jack_ringbuffer_write(rb, framebuf, bytes_per_frame);
			++total_captured;
		}
		pthread_cond_wait(&data_ready, &io_thread_lock);
	}

	fprintf(stderr, "EOF..\n");
	// wait until all data is processed
	while (jack_ringbuffer_read_space(rb)> 1024 * bytes_per_frame) usleep(1000); // XX

done:
	// TODO close readfd if it's not stdin
	pthread_mutex_unlock(&io_thread_lock);
	free(framebuf);
	return 0;
}
	
int process (jack_nframes_t nframes, void *arg) {
	int chn;
	size_t i;
	jack_thread_info_t *info = (jack_thread_info_t *) arg;
	const size_t bytes_per_frame = info->channels * SAMPLESIZE;

	for (chn = 0; chn < info->channels; ++chn)
		out[chn] = jack_port_get_buffer(ports[chn], nframes);

	const jack_nframes_t rbrs = jack_ringbuffer_read_space(rb);

	/* Do nothing until we're ready to begin. */
	if (   (!info->can_process)
			|| (!info->can_capture)
		     /* only dequeue samples if a whole period is avail. */
		  || (rbrs < bytes_per_frame * nframes)
		 ) {
		/* silence */
		for (chn = 0; chn < info->channels; ++chn) {
			memset(out[chn], 0, nframes * sizeof(jack_default_audio_sample_t));
		}
		
		/* count underruns */
		if (info->can_process && info->can_capture && rbrs < bytes_per_frame * nframes) {
			underruns++;
		}
		return 0;
	}

	/* dequeue interleaved samples to a single ringbuffer. */
	for (i = 0; i < nframes; i++) {

		for (chn = 0; chn < info->channels; ++chn) {
#if 1
			/* convert from 16 bit signed LE */
			char twobyte[2];
			jack_ringbuffer_read (rb, (void *) &twobyte, SAMPLESIZE);
			const int16_t d = (twobyte[0]&0xff) + ((twobyte[1]&0xff)<<8);
			out[chn][i] = (jack_default_audio_sample_t) (d / 32767.0);
			//out[chn][i] = (jack_default_audio_sample_t) ( ((int16_t) twobyte) / 32767.0);
#else /* TODO  - invert direction */
			//jack_default_audio_sample_t *js = out[chn];
			if (IS_FMT32B) { 
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
				/* 16/24 LE/BE signed/unsigned */
				const int32_t d = (int32_t) rintf(js*FMTMLT) + FMTOFF;
				char bytes[3];
				bytes[BE(0)] = (unsigned char) (d&0xff);
				bytes[BE(1)] = (unsigned char) (((d&0xff00)>>8)&0xff);
				if (info->format&1)
					bytes[BE(2)] = (unsigned char) (((d&0xff0000)>>16)&0xff);
				jack_ringbuffer_write (rb, (void *) &bytes, SAMPLESIZE);
			}
#endif
		}
	}
	/* Tell the io thread there that frames have been dequeued. */ 
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
	out = (jack_default_audio_sample_t **) malloc(in_size);
	rb = jack_ringbuffer_create(nports * SAMPLESIZE * info->rb_size);

	/* When JACK is running realtime, jack_activate() will have
	 * called mlockall() to lock our pages into memory.  But, we
	 * still need to touch any newly allocated pages before
	 * process() starts using them.  Otherwise, a page fault could
	 * create a delay that would force JACK to shut us down. */
	memset(out, 0, in_size);
	memset(rb->buf, 0, rb->size);

	for (i = 0; i < nports; i++) {
		char name[64];

		sprintf(name, "input%d", i+1);

		if ((ports[i] = jack_port_register(info->client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0)) == 0) {
			fprintf(stderr, "cannot register input port \"%s\"!\n", name);
			jack_client_close(info->client);
			exit(1);
		}
	}

	for (i = 0; i < nports; i++) {
		if (jack_connect(info->client, jack_port_name(ports[i]), source_names[i])) {
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
		"jack-stdin reads raw audio-data from standard-input and writes it to JACK.\n");
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
	thread_info.readfd = fileno(stdin);

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
				thread_info.format&=~10;
				if (!strncmp(optarg, "floating-point", strlen(optarg)))
					thread_info.format|=8;
				else if (!strncmp(optarg, "unsigned-integer", strlen(optarg)))
					thread_info.format|=2;
				else if (!strncmp(optarg, "signed-integer", strlen(optarg))) 
					;
				else {
					fprintf(stderr, "invalid encoding.\n");
					usage(argv[0], 1);
				}
				break;
			case 'b':
				thread_info.format&=~1;
				if (atoi(optarg) == 24) 
					thread_info.format|=1;
				else if (atoi(optarg) != 16) {
					fprintf(stderr, "invalid integer bit-depth. valid values: 16,i 24.\n");
					usage(argv[0], 1);
				}
				break;
			case 'L':
				thread_info.format&=~4;
				break;
				thread_info.rb_size = atoi(optarg);
			case 'B':
				thread_info.format|=4;
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
	if (thread_info.rb_size < 16) {
		fprintf(stderr, "Ringbuffer size needs to be at least 16 samples\n");
		usage(argv[0], 1);
	}

	if (argc <= optind) {
		fprintf(stderr, "At least one port/audio-channel must be given.\n");
		usage(argv[0], 1);
	}

	/* set up JACK client */
	if ((client = jack_client_open("jstdin", JackNoStartServer, &jstat)) == 0) {
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
			(thread_info.format&8)?"32":(thread_info.format&1?"24":"16"),
			(thread_info.format&8)?"":(thread_info.format&2?"unsigned-":"signed-"),
			(thread_info.format&8)?"float":"integer",
			(thread_info.format&8)?
				(thread_info.format&4?"native-endian":"non-native-endian"):
				(thread_info.format&4?"big-endian":"little-endian"),
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
	if (underruns > 0 && !want_quiet) {
		fprintf(stderr, "Note: there were %ld buffer underruns.\n", underruns);
	}
	jack_client_close(client);
	jack_ringbuffer_free(rb);
	return(0);
}
