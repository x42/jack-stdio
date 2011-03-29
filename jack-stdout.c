/** jack-stdout  - writes jack audio data to stdout.
    
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

    This program is based on capture_client.c from the jackaudio.org examples
    it was modified (C) 2008 Robin Gareus <robin@gareus.org>

    Copyright (C) 2001 Paul Davis
    Copyright (C) 2003 Jack O'Quin
    

    compile with
	gcc -o jack-stdout jack-stdout.c -ljack -lm -lpthread
    
    example use: 
	gcc -o jack-stdout jack-stdout.c  -l jack -lpthread -Wall -g && \
	./jack-stdout xmms_0:out_1 xmms_0:out_2 | \
	mono  ~/Desktop/Downloads/JustePort.exe - 10.0.1.6 0

*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sndfile.h>
#include <pthread.h>
#include <getopt.h>
#include <math.h>
#include <jack/jack.h>
#include <jack/ringbuffer.h>

typedef struct _thread_info {
    pthread_t thread_id;
    pthread_t mesg_thread_id;
    SNDFILE *sf;
    jack_nframes_t rb_size;
    jack_client_t *client;
    unsigned int channels;
    volatile int can_capture;
    volatile int can_process;
    volatile int status;
} jack_thread_info_t;

/* JACK data */
unsigned int nports;
jack_port_t **ports;
jack_default_audio_sample_t **in;
jack_nframes_t nframes;
const size_t sample_size = 2 ; //< was sizeof(jack_default_audio_sample_t);  now 16 bit unsigned

/* Synchronization between process thread and disk thread. */
jack_ringbuffer_t *rb;
pthread_mutex_t disk_thread_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mesg_thread_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  data_ready = PTHREAD_COND_INITIALIZER;
pthread_cond_t  mesg_queue = PTHREAD_COND_INITIALIZER;

long overruns = 0;
struct timespec last_overrun;
struct timespec last_message;

void *
message_thread (void *arg)
{
	jack_thread_info_t *info = (jack_thread_info_t *) arg;
	pthread_mutex_lock (&mesg_thread_lock);
	while (info->status==0) { 
		
		double diff = ((double) (last_overrun.tv_sec-last_message.tv_sec)) + ((double) (last_overrun.tv_nsec-last_message.tv_nsec)) / 1000000000.0;
		if (diff > 2.0) {
			fprintf(stderr, " %i buffer overruns - bytes in buffer: %lu\n",overruns, jack_ringbuffer_read_space(rb));
			clock_gettime(CLOCK_REALTIME, &last_message); // use jack-monotonic clock ?

		}
		pthread_cond_wait (&mesg_queue, &mesg_thread_lock);
	}
	pthread_mutex_unlock (&mesg_thread_lock);
	fprintf(stderr, "msg thread exiting.\n");
	
}


void *
disk_thread (void *arg)
{
	jack_thread_info_t *info = (jack_thread_info_t *) arg;
	jack_nframes_t samples_per_frame = info->channels;
	size_t bytes_per_frame = samples_per_frame * sample_size;
	void *framebuf = malloc (bytes_per_frame);

	pthread_setcanceltype (PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
	pthread_mutex_lock (&disk_thread_lock);

	info->status = 0;
	int writerrors =0;

	while (1) {
		/* Write the data one frame at a time.  This is
		 * inefficient, but makes things simpler. */
		while (info->can_capture &&
		       (jack_ringbuffer_read_space (rb) >= bytes_per_frame)) {

			jack_ringbuffer_read (rb, framebuf, bytes_per_frame);
			//TODO: resample ?!
			while (write(fileno(stdout) , framebuf, bytes_per_frame) != bytes_per_frame)
			{
				if (++writerrors>5) 
				{ 
					//TODO: push back the current frame. and resume at the correct channel
					fprintf(stderr, "FATAL: write error.\n");
					break;
				}

				// retry
				fprintf(stderr, "buffer not emptied: %i|%i\n", jack_ringbuffer_read_space(rb), jack_ringbuffer_write_space(rb));
				// heck this thread can just block..
				#if 0
				int usec= 1000; // 1ms
				fd_set fd;
				int max_fd=fileno(stdout);
				struct timeval tv = { 0, 0 };
				tv.tv_sec = 0; tv.tv_usec = usec;

				FD_ZERO(&fd);
				FD_SET(fileno(stdout), &fd);
				select(max_fd, &fd, NULL, NULL, &tv);
				#endif
			}
		}
		pthread_cond_wait (&data_ready, &disk_thread_lock);
	}

	pthread_mutex_unlock (&disk_thread_lock);
	free (framebuf);
	return 0;
}
	
int
process (jack_nframes_t nframes, void *arg)
{
	int chn;
	size_t i;
	jack_thread_info_t *info = (jack_thread_info_t *) arg;

	/* Do nothing until we're ready to begin. */
	if ((!info->can_process) || (!info->can_capture))
		return 0;

	for (chn = 0; chn < nports; chn++)
		in[chn] = jack_port_get_buffer (ports[chn], nframes);

	int err=0;
	/* Sndfile requires interleaved data.  It is simpler here to
	 * just queue interleaved samples to a single ringbuffer. */
	for (i = 0; i < nframes; i++) {
		for (chn = 0; chn < nports; chn++) {
			jack_default_audio_sample_t js = in[chn][i];
			if (jack_ringbuffer_write_space(rb) < 2) {
				overruns++;
				err=1;
				clock_gettime(CLOCK_REALTIME, &last_overrun); // use jack-monotonic clock ?
				continue;
			}
			// convert to 16 bit
			int d = (int) rint(js*32767.0);
			char twobyte[2];
			twobyte[0] = (unsigned char) (d&0xff);
			twobyte[1] = (unsigned char) (((d&0xff00)>>8)&0xff);
			if (jack_ringbuffer_write (rb, (void *) &twobyte, 2) < 2) {
				clock_gettime(CLOCK_REALTIME, &last_overrun); // use jack-monotonic clock ?
				err=1;
				overruns++;
			}
		}
	}

	if (pthread_mutex_trylock (&disk_thread_lock) == 0) {
	    pthread_cond_signal (&data_ready);
	    pthread_mutex_unlock (&disk_thread_lock);
	}

	if (err) {
		if (pthread_mutex_trylock (&mesg_thread_lock) == 0) {
			pthread_cond_signal (&mesg_queue);
			pthread_mutex_unlock (&mesg_thread_lock);
		}
	}

	return 0;
}

void
jack_shutdown (void *arg)
{
	fprintf (stderr, "JACK shutdown\n");
	// exit (0);
	abort();
}

void
setup_disk_thread (jack_thread_info_t *info)
{
	// samplerate = jack_get_sample_rate (info->client);
	// channels = info->channels;
	pthread_create (&info->thread_id, NULL, disk_thread, info);
}

void
run_disk_thread (jack_thread_info_t *info)
{
	info->can_capture = 1;

	fprintf(stderr, "starting output..\n");
	pthread_join (info->thread_id, NULL);

	//TODO set status to terminate message_thread and wake it up..
	pthread_join (info->mesg_thread_id, NULL);
}

void
setup_ports (int sources, char *source_names[], jack_thread_info_t *info)
{
	unsigned int i;
	size_t in_size;

	/* Allocate data structures that depend on the number of ports. */
	nports = sources;
	ports = (jack_port_t **) malloc (sizeof (jack_port_t *) * nports);
	in_size =  nports * sizeof (jack_default_audio_sample_t *);
	in = (jack_default_audio_sample_t **) malloc (in_size);
	rb = jack_ringbuffer_create (nports * sample_size * info->rb_size);

	/* When JACK is running realtime, jack_activate() will have
	 * called mlockall() to lock our pages into memory.  But, we
	 * still need to touch any newly allocated pages before
	 * process() starts using them.  Otherwise, a page fault could
	 * create a delay that would force JACK to shut us down. */
	memset(in, 0, in_size);
	memset(rb->buf, 0, rb->size);

	for (i = 0; i < nports; i++) {
		char name[64];

		sprintf (name, "input%d", i+1);

		if ((ports[i] = jack_port_register (info->client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0)) == 0) {
			fprintf (stderr, "cannot register input port \"%s\"!\n", name);
			jack_client_close (info->client);
			exit (1);
		}
	}

	for (i = 0; i < nports; i++) {
		if (jack_connect (info->client, source_names[i], jack_port_name (ports[i]))) {
			fprintf (stderr, "cannot connect input port %s to %s\n", jack_port_name (ports[i]), source_names[i]);
			jack_client_close (info->client);
			exit (1);
		} 
	}

	info->can_process = 1;		/* process() can start, now */
}

int
main (int argc, char *argv[])
{
	jack_client_t *client;
	jack_thread_info_t thread_info;

	memset (&thread_info, 0, sizeof (thread_info));
	thread_info.rb_size = 16384 * 4; //< make this an option

	if ((client = jack_client_open ("jstdout", JackNullOption, NULL)) == 0) {
		fprintf (stderr, "jack server not running?\n");
		exit (1);
	}

	thread_info.client = client;
	thread_info.channels = 2;
	thread_info.can_process = 0;

	setup_disk_thread (&thread_info);
	//optional: setup_message_thread:
	pthread_create (&thread_info.mesg_thread_id, NULL, message_thread, &thread_info);

	jack_set_process_callback (client, process, &thread_info);
	jack_on_shutdown (client, jack_shutdown, &thread_info);

	if (jack_activate (client)) {
		fprintf (stderr, "cannot activate client");
	}

	setup_ports (2, &argv[1], &thread_info); // TODO (NULL)
	run_disk_thread (&thread_info);

	jack_client_close (client);
	jack_ringbuffer_free (rb);
	return(0);
}
