#define _GNU_SOURCE

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <math.h>
#include <gtk/gtk.h>

GtkWidget *window;
GtkWidget *fxbox;
GtkWidget *confbox;
GtkWidget *frameconf;
GtkWidget *inputdevlabel;
GtkWidget *comboinputdev;
GtkWidget *frameslabel;
GtkWidget *spinbutton1;
GtkWidget *outputdevlabel;
GtkWidget *combooutputdev;

GtkWidget *framehaas1;
GtkWidget *haasenable;
GtkWidget *haasbox1;
GtkWidget *haaslabel1;
GtkWidget *spinbutton2;

GtkWidget *framedelay1;
GtkWidget *dlybox1;
GtkWidget *dlyenable;
GtkWidget *delaylabel1;
GtkWidget *spinbutton5;
GtkWidget *delaytypelabel;
GtkWidget *combodelaytype;
GtkWidget *feedbacklabel1;
GtkWidget *spinbutton6;

GtkWidget *statusbar;
gint context_id;

long long usecs; // microseconds
long diff1;

long get_first_time_microseconds()
{
	long long micros;
    struct timespec spec;

    clock_gettime(CLOCK_REALTIME, &spec);

	micros = spec.tv_sec * 1.0e6 + round(spec.tv_nsec / 1000); // Convert nanoseconds to microseconds
	usecs = micros;
	return(0L);
}

long get_next_time_microseconds()
{
    long delta;
    long long micros;
    struct timespec spec;

    clock_gettime(CLOCK_REALTIME, &spec);

    micros = spec.tv_sec * 1.0e6 + round(spec.tv_nsec / 1000); // Convert nanoseconds to microseconds
    delta = micros - usecs;
    usecs = micros;
    return(delta);
}

static void push_message(GtkWidget *widget, gint cid, char *msg)
{
  gchar *buff = g_strdup_printf("%s", msg);
  gtk_statusbar_push(GTK_STATUSBAR(statusbar), cid, buff);
  g_free(buff);
}

static void pop_message(GtkWidget *widget, gint cid)
{
  gtk_statusbar_pop(GTK_STATUSBAR(widget), cid);
}

// Delay Processor

enum dly_type /* Delay types */
{
	DLY_ECHO,
	DLY_DELAY,
	DLY_REVERB,
	DLY_LATE,
};

struct sounddelay
{
	snd_pcm_format_t format; // SND_PCM_FORMAT_S16
	unsigned int rate; // sampling rate
	unsigned int channels; // channels
	float feedback; // feedback level 0.0 .. 1.0
	float millisec; // delay in milliseconds
	int N; // parallel delays
	int enabled;
	pthread_mutex_t delaymutex; // = PTHREAD_MUTEX_INITIALIZER;

	int physicalwidth; // bits per sample
	char *fbuffer;
	int fbuffersize;
	int fbuffersamples;
	int delaysamples;
	int delaybytes;
	int insamples;

	int front, rear, readfront;
	signed short *fshort;

	enum dly_type delaytype;
};

struct sounddelay snddly; // Delay

void sounddelay_init(int N, enum dly_type delaytype, float millisec, float feedback, snd_pcm_format_t format, unsigned int rate, unsigned int channels, struct sounddelay *s)
{
	s->N = N;
	s->delaytype = delaytype;
	s->format = format;
	s->rate = rate;
	s->channels = channels;
	s->feedback = feedback;
	s->millisec = millisec;
	s->fbuffer = NULL;
	//printf("Delay initialized, type %d, %5.2f ms, %5.2f feedback, %d rate, %d channels\n", s->delaytype, s->millisec, s->feedback, s->rate, s->channels);
}

void sounddelay_add(char* inbuffer, int inbuffersize, struct sounddelay *s)
{
	int i;
	float prescale;
	signed short *inshort;

	pthread_mutex_lock(&(s->delaymutex));
	if (s->enabled)
	{
		if (!s->fbuffer)
		{
			s->physicalwidth = snd_pcm_format_width(s->format);
			s->insamples = inbuffersize * 8 / s->physicalwidth;
			s->delaysamples = ceil((s->millisec / 1000.0) * (float)s->rate) * s->channels;
			s->delaybytes = s->delaysamples * s->physicalwidth / 8;

			s->fbuffersize = s->delaybytes + inbuffersize;
			s->fbuffer = malloc(s->fbuffersize);
			memset(s->fbuffer, 0, s->fbuffersize);
			s->fbuffersamples = s->insamples + s->delaysamples;
			s->fshort = (signed short *)s->fbuffer;

			s->rear = s->delaysamples;
			s->front = 0;
			s->readfront = 0;
		}
		inshort = (signed short *)inbuffer;

		switch (s->delaytype)
		{
			case DLY_ECHO: // Repeating echo added to original
				prescale = sqrt(1 - s->feedback*s->feedback); // prescale=sqrt(sum(r^2n)), n=0..infinite
				for(i=0; i<s->insamples; i++)
				{
					inshort[i]*=prescale;
					s->fshort[s->rear++] = inshort[i] += s->fshort[s->front++]*s->feedback;
					s->front%=s->fbuffersamples;
					s->rear%=s->fbuffersamples;
				}
				break;
			case DLY_DELAY: // Single delayed signal added to original
				prescale = 1 / sqrt(1 + s->feedback*s->feedback); // prescale = 1/sqrt(1 + r^2)
				for(i=0;i<s->insamples; i++)
				{
					inshort[i]*=prescale;
					s->fshort[s->rear++] = inshort[i];
					inshort[i] += s->fshort[s->front++]*s->feedback;
					s->front%=s->fbuffersamples;
					s->rear%=s->fbuffersamples;
				}
				break;
			case DLY_REVERB: // Only repeating echo, no original
				//prescale = sqrt(1 - s->feedback*s->feedback); // prescale=sqrt(sum(r^2n)), n=0..infinite
				prescale = sqrt((1.0-s->feedback*s->feedback)/((s->N-1)*s->feedback*s->feedback+1.0)); // prescale=sqrt(sum(r^2n)-1), for all channels, n=0..infinite
				for(i=0; i<s->insamples; i++)
				{
					//s->fshort[s->rear++] = inshort[i]*prescale + s->fshort[s->front++]*s->feedback;
					s->fshort[s->rear++] = (inshort[i]*prescale + s->fshort[s->front++])*s->feedback;
					s->front%=s->fbuffersamples;
					s->rear%=s->fbuffersamples;
				}
				break;
			case DLY_LATE: // Single delayed signal, no original
				for(i=0;i<s->insamples; i++)
				{
					s->fshort[s->rear++] = inshort[i]*s->feedback;
					s->front++;
					s->front%=s->fbuffersamples;
					s->rear%=s->fbuffersamples;
				}
				break;
			default:
				break;
		}
	}
	pthread_mutex_unlock(&(s->delaymutex));
}

void sounddelay_close(struct sounddelay *s)
{
	if (s->fbuffer)
	{
		free(s->fbuffer);
		s->fbuffer = NULL;
	}
}

void Delay_initAll(snd_pcm_format_t format, float rate, unsigned int channels, struct sounddelay *s)
{
	gchar *strval;

	pthread_mutex_lock(&(s->delaymutex));
	g_object_get((gpointer)combodelaytype, "active-id", &strval, NULL);
	//printf("Selected id %s\n", strval);
	s->delaytype = atoi((const char *)strval);
	sounddelay_init(1, atoi((const char *)strval), (float)gtk_spin_button_get_value(GTK_SPIN_BUTTON(spinbutton5)), 
					(float)gtk_spin_button_get_value(GTK_SPIN_BUTTON(spinbutton6)), format, rate, channels, s);
	g_free(strval);
	pthread_mutex_unlock(&(s->delaymutex));
}

void Delay_closeAll(struct sounddelay *s)
{
	pthread_mutex_lock(&(s->delaymutex));
	sounddelay_close(s);
	pthread_mutex_unlock(&(s->delaymutex));
}


// Audio circular queue
pthread_t tid[3];
cpu_set_t cpu[4];
int retval_thread0, retval_thread1, retval_thread2;

snd_pcm_format_t samplingformat = SND_PCM_FORMAT_S16;
int samplingrate = 44100;
int mono = 1, stereo = 2;
int queueLength = 4;

int frames_default = 128;

enum aqstatus
{
	AQ_RUNNING = 0,
	AQ_STOPPING,
	AQ_STOPPED
};

struct audioqueue
{
	int aqLength; // Audio circular queue length
	int front, rear;
	snd_pcm_format_t format;
	int rate;
	int channels;
	int buffersize, buffersamples, bufferframes;
	char** buffer;
	int status;
};
pthread_mutex_t aqmutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t aqlowcond = PTHREAD_COND_INITIALIZER;
pthread_cond_t aqhighcond = PTHREAD_COND_INITIALIZER;
struct audioqueue aq;

void aq_init(struct audioqueue *q, snd_pcm_format_t format, int frames, int rate, int channels, int qLength)
{
	q->front = q->rear = 0;
	q->aqLength = qLength+1; // allocate N+1 buffers
	q->format = format;
	q->rate = rate;
	q->channels = channels;
	q->bufferframes = frames;

	q->buffersamples = q->bufferframes * q->channels;
	q->buffersize = q->buffersamples * ( snd_pcm_format_width(q->format) / 8 );

	//printf("allocating buffer %d\n", q->aqLength * sizeof(char*));
	q->buffer = malloc(q->aqLength * sizeof(char*));
	int i;
	for(i=0;i<q->aqLength;i++)
	{
		//printf("allocating buffer[%d] %d\n", i, q->buffersize * sizeof(char));
		q->buffer[i] = malloc(q->buffersize * sizeof(char));
	}
	q->status = AQ_RUNNING;
}

void aq_add(struct audioqueue *q, char *inbuffer)
{
	pthread_mutex_lock(&aqmutex);
	while ((q->rear+1)%q->aqLength == q->front) // queue full
	{
		//printf("sleeping, audio queue full\n");
		pthread_cond_wait(&aqhighcond, &aqmutex);
	}

	//printf("adding %d bytes at %d\n", q->buffersize, q->rear);
	memcpy(q->buffer[q->rear], inbuffer, q->buffersize);

	q->rear++;
	q->rear%=q->aqLength;
	pthread_cond_signal(&aqlowcond); // Should wake up *one* thread
	pthread_mutex_unlock(&aqmutex);
}

/*
char* aq_frontget(struct audioqueue *q)
{
	char* frontbuffer;

	pthread_mutex_lock(&aqmutex);
	while(q->front == q->rear) // queue empty
	{
		//printf("sleeping, audio queue empty\n");
		pthread_cond_wait(&aqlowcond, &aqmutex);
	}
	frontbuffer = q->buffer[q->front];
	pthread_mutex_unlock(&aqmutex);
	return(frontbuffer);
}

enum aqstatus aq_frontnext(struct audioqueue *q)
{
	pthread_mutex_lock(&aqmutex);
	while(q->front == q->rear) // queue empty
	{
		//printf("sleeping, audio queue empty\n");
		pthread_cond_wait(&aqlowcond, &aqmutex);
	}
	//printf("front next %d\n", q->front);
	q->front++;
	q->front%=q->aqLength;
	enum aqstatus s = q->status;
	pthread_cond_signal(&aqhighcond); // Should wake up *one* thread
	pthread_mutex_unlock(&aqmutex);
	return(s);
}
*/

enum aqstatus aq_remove(struct audioqueue *q, char *outbuffer)
{
	enum aqstatus s;

	pthread_mutex_lock(&aqmutex);
	while(q->front == q->rear) // queue empty
	{
		if (q->status != AQ_STOPPED)
		{
			//printf("sleeping, audio queue empty\n");
			pthread_cond_wait(&aqlowcond, &aqmutex);
		}
		else
		{
			s = q->status;
			pthread_mutex_unlock(&aqmutex);
			return(s);
		}
	}
	//printf("removing %d bytes at %d\n", q->buffersize, q->front);
	memcpy(outbuffer, q->buffer[q->front], q->buffersize);
	q->front++;
	q->front%=q->aqLength;
	s = q->status;
	pthread_cond_signal(&aqhighcond); // Should wake up *one* thread
	pthread_mutex_unlock(&aqmutex);
	return(s);
}

void aq_requeststop(struct audioqueue *q)
{
	pthread_mutex_lock(&aqmutex);
	q->status = AQ_STOPPING;
	pthread_mutex_unlock(&aqmutex);
}

void aq_signalstop(struct audioqueue *q)
{
	pthread_mutex_lock(&aqmutex);
	q->status = AQ_STOPPED;
	pthread_cond_signal(&aqlowcond); // Should wake up *one* thread
	pthread_mutex_unlock(&aqmutex);
}

void aq_destroy(struct audioqueue *q)
{
	int i;
	for(i=0;i<q->aqLength;i++)
	{
		//printf("freeing q->buffer[%d]\n", i);
		free(q->buffer[i]);
		q->buffer[i]=NULL;
	}
	//printf("freeing q->buffer\n");
	free(q->buffer);
	q->buffer = NULL;
}

float aq_getdelay(struct audioqueue *q)
{
	return((float)q->bufferframes/(float)q->rate);
}

// recorder thread
struct microphone
{
	char device[32];
	snd_pcm_format_t format;
	unsigned int rate;
	unsigned int channels;
	snd_pcm_t *capture_handle;
	snd_pcm_hw_params_t *hw_params;
};
struct microphone mic;

int init_audio_mic(struct microphone *m)
{
	int err;

	snd_pcm_hw_params_alloca(&(m->hw_params));
	//snd_pcm_sw_params_alloca(&sw_params);

	if ((err = snd_pcm_open(&(m->capture_handle), m->device, SND_PCM_STREAM_CAPTURE, 0)) < 0)
	{
		printf("cannot open audio device %s (%s)\n", m->device, snd_strerror(err));
		return(err);
	}

	if ((err = snd_pcm_hw_params_any(m->capture_handle, m->hw_params)) < 0)
	{
		printf("cannot initialize hardware parameter structure (%s)\n", snd_strerror(err));
		return(err);
  }

	if ((err = snd_pcm_hw_params_set_access(m->capture_handle, m->hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
	{
		printf("cannot set access type (%s)\n", snd_strerror(err));
		return(err);
	}

	if ((err = snd_pcm_hw_params_set_format(m->capture_handle, m->hw_params, m->format)) < 0)
	{
		printf("cannot set sample format (%s)\n", snd_strerror(err));
		return(err);
	}

	if ((err = snd_pcm_hw_params_set_rate_near(m->capture_handle, m->hw_params, &(m->rate), 0)) < 0)
	{
		printf("cannot set sample rate (%s)\n", snd_strerror(err));
		return(err);
	}

	if ((err = snd_pcm_hw_params_set_channels(m->capture_handle, m->hw_params, m->channels)) < 0)
	{
		printf("cannot set channel count (%s)\n", snd_strerror(err));
		return(err);
	}

	if ((err = snd_pcm_hw_params(m->capture_handle, m->hw_params)) < 0)
	{
		printf("cannot set parameters (%s)\n", snd_strerror(err));
		return(err);
	}

	if ((err = snd_pcm_prepare(m->capture_handle)) < 0)
	{
		printf("cannot prepare audio interface for use (%s)\n", snd_strerror(err));
		return(err);
	}

//printf("initialized format %d rate %d channels %d\n", m->format, m->rate, m->channels);
	return(err);
}

void close_audio_mic(struct microphone *m)
{
	snd_pcm_close(m->capture_handle);
}

static gpointer recorderthread(gpointer args)
{
	int ctype = PTHREAD_CANCEL_ASYNCHRONOUS;
	int ctype_old;
	pthread_setcanceltype(ctype, &ctype_old);

	int err;

	//mic.device = "hw:1,0";
	gchar *strval;
	g_object_get((gpointer)comboinputdev, "active-id", &strval, NULL);
	strcpy(mic.device, strval);
	g_free(strval);

	mic.format = SND_PCM_FORMAT_S16_LE;
	mic.rate = samplingrate;
	mic.channels = aq.channels;
	if ((err=init_audio_mic(&mic)))
	{
		printf("Init mic error %d\n", err);
	}
	else
	{
		char *buffer = malloc(aq.bufferframes * aq.channels * ( snd_pcm_format_width(mic.format) / 8 ));
		while (aq.status==AQ_RUNNING)
		{
//get_first_time_microseconds();
			if ((err = snd_pcm_readi(mic.capture_handle, buffer, aq.bufferframes)) != aq.bufferframes)
			{
				printf("read from audio interface failed (err code %d) %s\n", err, snd_strerror(err));
				break;
			}
			else
			{
//printf("adding %d\n", aq.rear);
				aq_add(&aq, buffer);
			}
//diff1 = get_next_time_microseconds();
//printf("read time %ld\n", diff1);
		}
		aq_signalstop(&aq);
		//printf("freeing buffer\n");
		free(buffer);
		close_audio_mic(&mic);
	}

//printf("exiting 1\n");
	retval_thread1 = 0;
	pthread_exit(&retval_thread1);
}

// player thread
int persize = 1024;	// 
int bufsize = 10240; // persize * 10; // 10 periods

struct speaker
{
	snd_pcm_t *handle;
	char device[32];	// playback device
	unsigned int rate;	// stream rate
	snd_pcm_format_t format; // = SND_PCM_FORMAT_S16, sample format
	unsigned int channels;		// count of channels
	int resample;	// = 1, enable alsa-lib resampling
	int period_event;	// = 0, produce poll event after each period
	snd_pcm_access_t access; // = SND_PCM_ACCESS_RW_INTERLEAVED
	snd_pcm_sframes_t buffer_size;
	snd_pcm_sframes_t period_size;
	snd_pcm_hw_params_t *hwparams;
	snd_pcm_sw_params_t *swparams;
};
struct speaker spk;

int set_hwparams(struct speaker *s)
{
	unsigned int rrate;
	snd_pcm_uframes_t size;
	int err, dir;

	/* choose all parameters */
	if ((err = snd_pcm_hw_params_any(s->handle, s->hwparams)) < 0)
	{
		printf("Broken configuration for playback: no configurations available: %s\n", snd_strerror(err));
		return(err);
	}

	/* set hardware resampling */
	if ((err = snd_pcm_hw_params_set_rate_resample(s->handle, s->hwparams, s->resample)) < 0)
	{
		printf("Resampling setup failed for playback: %s\n", snd_strerror(err));
		return(err);
	}

	/* set the interleaved read/write format */
	
	if ((err = snd_pcm_hw_params_set_access(s->handle, s->hwparams, s->access)) < 0)
	{
		printf("Access type not available for playback: %s\n", snd_strerror(err));
		return(err);
	}

	/* set the sample format */
	if ((err = snd_pcm_hw_params_set_format(s->handle, s->hwparams, s->format)) < 0)
	{
		printf("Sample format not available for playback: %s\n", snd_strerror(err));
		return(err);
	}

	/* set the count of channels */
	if ((err = snd_pcm_hw_params_set_channels(s->handle, s->hwparams, s->channels)) < 0)
	{
		printf("Channels count (%i) not available for playbacks: %s\n", s->channels, snd_strerror(err));
		return(err);
	}

	/* set the stream rate */
	rrate = s->rate;
	if ((err = snd_pcm_hw_params_set_rate_near(s->handle, s->hwparams, &rrate, 0)) < 0)
	{
		printf("Rate %iHz not available for playback: %s\n", s->rate, snd_strerror(err));
		return(err);
	}
	if (rrate != s->rate)
	{
		printf("Rate doesn't match (requested %iHz, get %iHz)\n", s->rate, rrate);
		return(-EINVAL);
	}

	if ((err = snd_pcm_hw_params_set_buffer_size(s->handle, s->hwparams, bufsize)) < 0)
	{
		printf("Unable to set buffer size %d for playback: %s\n", bufsize, snd_strerror(err));
		return err;
	}

	if ((err = snd_pcm_hw_params_get_buffer_size(s->hwparams, &size)) < 0)
	{
		printf("Unable to get buffer size for playback: %s\n", snd_strerror(err));
		return(err);
	}
	s->buffer_size = size;

	if ((err = snd_pcm_hw_params_set_period_size(s->handle, s->hwparams, persize, 0)) < 0)
	{
		printf("Unable to set period size %i for playback: %s\n", persize, snd_strerror(err));
		return(err);
	}

	if ((err = snd_pcm_hw_params_get_period_size(s->hwparams, &size, &dir)) < 0)
	{
		printf("Unable to get period size for playback: %s\n", snd_strerror(err));
		return err;
	}
	s->period_size = size;

	/* write the parameters to device */
	if ((err = snd_pcm_hw_params(s->handle, s->hwparams)) < 0)
	{
		printf("Unable to set hw params for playback: %s\n", snd_strerror(err));
		return err;
	}
	return(0);
}

int set_swparams(struct speaker *s)
{
	int err;

	/* get the current swparams */
	if ((err = snd_pcm_sw_params_current(s->handle, s->swparams)) < 0)
	{
		printf("Unable to determine current swparams for playback: %s\n", snd_strerror(err));
		return(err);
	}

	/* start the transfer when the buffer is almost full: */
	/* (buffer_size / avail_min) * avail_min */
	//err = snd_pcm_sw_params_set_start_threshold(handle, swparams, (buffer_size / period_size) * period_size);

	// start transfer when first period arrives
	if ((err = snd_pcm_sw_params_set_start_threshold(s->handle, s->swparams, s->period_size)) < 0)
	{
		printf("Unable to set start threshold mode for playback: %s\n", snd_strerror(err));
		return(err);
	}

	/* allow the transfer when at least period_size samples can be processed */
	/* or disable this mechanism when period event is enabled (aka interrupt like style processing) */
	if ((err = snd_pcm_sw_params_set_avail_min(s->handle, s->swparams, s->period_event ? s->buffer_size : s->period_size)) < 0)
	{
		printf("Unable to set avail min for playback: %s\n", snd_strerror(err));
		return(err);
	}

	/* enable period events when requested */
	if (s->period_event)
	{
		if ((err = snd_pcm_sw_params_set_period_event(s->handle, s->swparams, 1)) < 0)
		{
			printf("Unable to set period event: %s\n", snd_strerror(err));
			return(err);
		}
	}

	/* write the parameters to the playback device */
	if ((err = snd_pcm_sw_params(s->handle, s->swparams)) < 0)
	{
		printf("Unable to set sw params for playback: %s\n", snd_strerror(err));
		return(err);
	}

	return(0);
}

int init_audio_spk(struct speaker *s)
{
	int err;

	snd_pcm_hw_params_alloca(&(s->hwparams));
	snd_pcm_sw_params_alloca(&(s->swparams));

	if ((err = snd_pcm_open(&(s->handle), s->device, SND_PCM_STREAM_PLAYBACK, 0)) < 0)
	{
		printf("Playback open error: %s\n", snd_strerror(err));
		return(err);
	}
	if ((err = set_hwparams(s)) < 0)
	{
		printf("Setting of hwparams failed: %s\n", snd_strerror(err));
		return(err);
	}
	if ((err = set_swparams(s)) < 0)
	{
		printf("Setting of swparams failed: %s\n", snd_strerror(err));
		return(err);
	}

	return(0);
}

void close_audio_spk(struct speaker *s)
{
	snd_pcm_close(s->handle);
}

/* Underrun and suspend recovery */
static int xrun_recovery(snd_pcm_t *handle, int err)
{
	if (err == -EPIPE)	// under-run
	{
		err = snd_pcm_prepare(handle);
		if (err < 0)
			printf("Can't recover from underrun, prepare failed: %s\n", snd_strerror(err));
		return(0);
	}
	else if (err == -ESTRPIPE)
	{
		while ((err = snd_pcm_resume(handle)) == -EAGAIN)
			sleep(1);	/* wait until the suspend flag is released */
		if (err < 0)
		{
			err = snd_pcm_prepare(handle);
			if (err < 0)
				printf("Can't recover from suspend, prepare failed: %s\n", snd_strerror(err));
		}
		return(0);
	}
	return(err);
}

pthread_mutex_t haasmutex = PTHREAD_MUTEX_INITIALIZER;
int haasenabled = TRUE;
float millisec = 20.0;
char* delayed = NULL;
int delaybytes = 0;
signed short *delayedbuffer = NULL;

void haas_init(struct audioqueue *a)
{
	int delayframes = millisec /1000.0 * a->rate;
	delaybytes = delayframes * ( snd_pcm_format_width(a->format) / 8 );
	delayed = malloc((a->bufferframes+delayframes) * ( snd_pcm_format_width(a->format) / 8 ));
	memset(delayed+a->buffersize, 0, delaybytes);
	delayedbuffer = (signed short *)delayed;
	//printf("%f, %d, %d\n", millisec, delayframes, delaybytes);
}

void haas_close()
{
	if (!delayed)
	{
		free(delayed);
		delayed = NULL;
		delaybytes = 0;
		delayedbuffer = NULL;
	}
}

static gpointer playerthread(gpointer args)
{
	int ctype = PTHREAD_CANCEL_ASYNCHRONOUS;
	int ctype_old;
	pthread_setcanceltype(ctype, &ctype_old);

	//spk.device = "plughw:0,0";
	gchar *strval;
	g_object_get((gpointer)combooutputdev, "active-id", &strval, NULL);
	strcpy(spk.device, strval);
	g_free(strval);

	spk.rate = aq.rate;
	spk.format = samplingformat;
	spk.channels = stereo;
	spk.resample = 1;
	spk.period_event = 0;
	spk.access = SND_PCM_ACCESS_RW_INTERLEAVED;
	init_audio_spk(&spk);

	int i, j, err;
	int sbuffersize = aq.bufferframes * stereo * ( snd_pcm_format_width(aq.format) / 8 );
	char *sbuffer = malloc(sbuffersize);
	signed short *stereobuffer = (signed short *)sbuffer;

	char *mbuffer = malloc(aq.buffersize * sizeof(char));
	signed short *monobuffer = (signed short *)mbuffer;

	haas_init(&aq);
	while(aq_remove(&aq, mbuffer)!=AQ_STOPPED)
	{
		pthread_mutex_lock(&haasmutex);
		memcpy(delayed, delayed+aq.buffersize, delaybytes); // R
		memcpy(delayed+delaybytes, (char *)monobuffer, aq.buffersize);

		for(i=j=0;i<aq.bufferframes;i++)
		{
			if (haasenabled)
			{
				// Haas
				stereobuffer[j++] = monobuffer[i] * 0.7; // L
				stereobuffer[j++] = delayedbuffer[i];
			}
			else
			{
				// Dry, mono -> mono on both sides of stereo
				stereobuffer[j++] = monobuffer[i]; // L
				stereobuffer[j++] = monobuffer[i]; // R
			}
		}
		pthread_mutex_unlock(&haasmutex);

		// process stereo frames here
		sounddelay_add(sbuffer, sbuffersize, &snddly);

		err = snd_pcm_writei(spk.handle, stereobuffer, aq.bufferframes);
		if (err == -EAGAIN) printf("EAGAIN\n");
		if (err < 0)
		{
			if (xrun_recovery(spk.handle, err) < 0)
			{
				printf("Write error: %s\n", snd_strerror(err));
			}
		}
	}
	haas_close();
	free(mbuffer);
	free(sbuffer);
	close_audio_spk(&spk);

//printf("exiting 2\n");
	retval_thread2 = 0;
	pthread_exit(&retval_thread2);
}

static gpointer thread0(gpointer args)
{
	struct audioqueue *a = (struct audioqueue *)args;
	char delayms[50];

	int ctype = PTHREAD_CANCEL_ASYNCHRONOUS;
	int ctype_old;
	pthread_setcanceltype(ctype, &ctype_old);

	int frames = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(spinbutton1));
	aq_init(a, samplingformat, frames, samplingrate, mono, queueLength);

	sprintf(delayms, "%5.2f ms delay, rate %d fps, %d queues", aq_getdelay(a)*1000.0, a->rate, queueLength);
	push_message(statusbar, context_id, delayms);

	Delay_initAll(samplingformat, samplingrate, stereo, &snddly);

	int err;
	err = pthread_create(&(tid[1]), NULL, &recorderthread, NULL);
	if (err)
	{}
	CPU_ZERO(&(cpu[1]));
	CPU_SET(1, &(cpu[1]));
	err = pthread_setaffinity_np(tid[1], sizeof(cpu_set_t), &(cpu[1]));
	if (err)
	{}

	err = pthread_create(&(tid[2]), NULL, &playerthread, NULL);
	if (err)
	{}
	CPU_ZERO(&(cpu[2]));
	CPU_SET(2, &(cpu[2]));
	err = pthread_setaffinity_np(tid[2], sizeof(cpu_set_t), &(cpu[2]));
	if (err)
	{}

	int i;
	if ((i=pthread_join(tid[1], NULL)))
		printf("pthread_join error, tid[0], %d\n", i);
	if ((i=pthread_join(tid[2], NULL)))
		printf("pthread_join error, tid[1], %d\n", i);

	Delay_closeAll(&snddly);

//printf("exiting 0\n");
	retval_thread0 = 0;
	pthread_exit(&retval_thread0);
}

int create_thread0(struct audioqueue *q)
{
	int err;

	err = pthread_create(&(tid[0]), NULL, &thread0, (gpointer)q);
	if (err)
	{}
	CPU_ZERO(&(cpu[0]));
	CPU_SET(0, &(cpu[0]));
	err = pthread_setaffinity_np(tid[0], sizeof(cpu_set_t), &(cpu[0]));
	if (err)
	{}

	return(0);
}

void terminate_thread0(struct audioqueue *q)
{
	int i;

	aq_requeststop(q);
	if ((i=pthread_join(tid[0], NULL)))
		printf("pthread_join error, tid[0], %d\n", i);
	aq_destroy(q);

	pop_message(statusbar, context_id);
}

static gboolean delete_event(GtkWidget *widget, GdkEvent *event, gpointer data)
{
	struct audioqueue *a = (struct audioqueue *)data;

	terminate_thread0(a);
	return FALSE; // return FALSE to emit destroy signal
}

static void destroy(GtkWidget *widget, gpointer data)
{
	gtk_main_quit();
}

static void realize_cb(GtkWidget *widget, gpointer data)
{
	g_object_set((gpointer)combodelaytype, "active-id", "0", NULL);
}

static void inputdev_changed(GtkWidget *combo, gpointer data)
{
	gchar *strval;
	struct audioqueue *a = (struct audioqueue*)data;

	terminate_thread0(a);
	g_object_get((gpointer)combo, "active-id", &strval, NULL);
	//printf("Selected id %s\n", strval);
	create_thread0(a);
	g_free(strval);
}

static void frames_changed(GtkWidget *widget, gpointer data)
{
	struct audioqueue *a = (struct audioqueue*)data;

	terminate_thread0(a);
	create_thread0(a);
}

static void outputdev_changed(GtkWidget *combo, gpointer data)
{
	gchar *strval;
	struct audioqueue *a = (struct audioqueue*)data;

	terminate_thread0(a);
	g_object_get((gpointer)combo, "active-id", &strval, NULL);
	//printf("Selected id %s\n", strval);
	create_thread0(a);
	g_free(strval);
}

static void haas_toggled(GtkWidget *togglebutton, gpointer data)
{
	pthread_mutex_lock(&haasmutex);
	haasenabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(togglebutton));
	pthread_mutex_unlock(&haasmutex);
	//printf("toggle state %d\n", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(modenable)));
}

static void haasdly_changed(GtkWidget *widget, gpointer data)
{
	pthread_mutex_lock(&haasmutex);
	millisec = (float)gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget));
	if (haasenabled)
	{
		haas_close();
		haas_init(&aq);
	}
	pthread_mutex_unlock(&haasmutex);
}

static void dly_toggled(GtkWidget *togglebutton, gpointer data)
{
	gchar *strval;

	pthread_mutex_lock(&(snddly.delaymutex));

	g_object_get((gpointer)combodelaytype, "active-id", &strval, NULL);
	//printf("Selected id %s\n", strval);
	snddly.delaytype = atoi((const char *)strval);
	g_free(strval);
	snddly.millisec = (float)gtk_spin_button_get_value(GTK_SPIN_BUTTON(spinbutton5));
	snddly.feedback = (float)gtk_spin_button_get_value(GTK_SPIN_BUTTON(spinbutton6));

	snddly.enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(togglebutton));
	if (snddly.enabled)
	{
		sounddelay_init(1, snddly.delaytype, snddly.millisec, snddly.feedback, snddly.format, snddly.rate, snddly.channels, &snddly);
	}
	else
	{
		sounddelay_close(&snddly);
	}

	pthread_mutex_unlock(&(snddly.delaymutex));
	//printf("toggle state %d\n", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(dlyenable)));
}

static void delay_changed(GtkWidget *widget, gpointer data)
{
	pthread_mutex_lock(&(snddly.delaymutex));
	float newvalue = (float)gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget));
	if (snddly.enabled)
	{
		sounddelay_close(&snddly);
		sounddelay_init(1, snddly.delaytype, newvalue, snddly.feedback, snddly.format, snddly.rate, snddly.channels, &snddly);
	}
	pthread_mutex_unlock(&(snddly.delaymutex));
}

void select_delay_types()
{
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combodelaytype), "0", "Echo");
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combodelaytype), "1", "Delay");
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combodelaytype), "2", "Reverb");
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combodelaytype), "3", "Late");
}

static void delaytype_changed(GtkWidget *combo, gpointer data)
{
	gchar *strval;

	pthread_mutex_lock(&(snddly.delaymutex));
	if (snddly.enabled)
	{
		sounddelay_close(&snddly);
		g_object_get((gpointer)combo, "active-id", &strval, NULL);
		//printf("Selected id %s\n", strval);
		snddly.delaytype = atoi((const char *)strval);
		g_free(strval);
		sounddelay_init(1, snddly.delaytype, snddly.millisec, snddly.feedback, snddly.format, snddly.rate, snddly.channels, &snddly);
	}
	pthread_mutex_unlock(&(snddly.delaymutex));
}

static void feedback_changed(GtkWidget *widget, gpointer data)
{
	pthread_mutex_lock(&(snddly.delaymutex));
	float newvalue = (float)gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget));
	if (snddly.enabled)
	{
		sounddelay_close(&snddly);
		sounddelay_init(1, snddly.delaytype, snddly.millisec, newvalue, snddly.format, snddly.rate, snddly.channels, &snddly);
	}
	pthread_mutex_unlock(&(snddly.delaymutex));
}

void print_card_list(void)
{
	int status, count = 0;
	int card = -1;  // use -1 to prime the pump of iterating through card list
	char* longname  = NULL;
	char* shortname = NULL;
	char name[32];
	char devicename[32];
	//const char* subname;
	snd_ctl_t *ctl;
	snd_pcm_info_t *info;
	int device;
	int sub, foundsub, devicepreset;

	do
	{
		if ((status = snd_card_next(&card)) < 0)
		{
			printf("cannot determine card number: %s\n", snd_strerror(status));
			break;
		}
		if (card<0) break;
		//printf("Card %d:", card);
		if ((status = snd_card_get_name(card, &shortname)) < 0)
		{
			printf("cannot determine card shortname: %s\n", snd_strerror(status));
			break;
		}
		if ((status = snd_card_get_longname(card, &longname)) < 0)
		{
			printf("cannot determine card longname: %s\n", snd_strerror(status));
			break;
		}
		//printf("\tLONG NAME:  %s\n", longname);
		//printf("\tSHORT NAME: %s\n", shortname);

		sprintf(name, "hw:%d", card);
		if ((status = snd_ctl_open(&ctl, name, 0)) < 0)
		{
			printf("cannot open control for card %d: %s\n", card, snd_strerror(status));
			return;
		}

		device = -1;
		do
		{
			status = snd_ctl_pcm_next_device(ctl, &device);
			if (status < 0)
			{
				printf("cannot determine device number: %s\n", snd_strerror(status));
				break;
			}
			if (device<0) break;
			//printf("Device %s,%d\n", name, device);
			snd_pcm_info_malloc(&info);
			snd_pcm_info_set_device(info, (unsigned int)device);

			snd_pcm_info_set_stream(info, SND_RAWMIDI_STREAM_INPUT);
			snd_ctl_pcm_info(ctl, info);
			int subs_in = snd_pcm_info_get_subdevices_count(info);
			//printf("Input subdevices : %d\n", subs_in);
			for(sub=0,foundsub=0,devicepreset=0;sub<subs_in;sub++)
			{
				snd_pcm_info_set_subdevice(info, sub);
				if ((status = snd_ctl_pcm_info(ctl, info)) < 0)
				{
					//printf("cannot get pcm information %d:%d:%d: %s\n", card, device, sub, snd_strerror(status));
					continue;
				}
				//subname = snd_pcm_info_get_subdevice_name(info);
				//printf("Subdevice %d name : %s\n", sub, subname);
				foundsub = 1;
			}
			if (foundsub)
			{
				sprintf(devicename, "hw:%d,%d", card, device);
				gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(comboinputdev), devicename, shortname);
				if (!devicepreset)
				{
					devicepreset = 1;
					g_object_set((gpointer)comboinputdev, "active-id", devicename, NULL);
				}
			}

			snd_pcm_info_set_stream(info, SND_RAWMIDI_STREAM_OUTPUT);
			snd_ctl_pcm_info(ctl, info);
			int subs_out = snd_pcm_info_get_subdevices_count(info);
			//printf("Output subdevices : %d\n", subs_out);
			for(sub=0,foundsub=0,devicepreset=0;sub<subs_out;sub++)
			{
				snd_pcm_info_set_subdevice(info, sub);
				if ((status = snd_ctl_pcm_info(ctl, info)) < 0)
				{
					//printf("cannot get pcm information %d:%d:%d: %s\n", card, device, sub, snd_strerror(status));
					continue;
				}
				//subname = snd_pcm_info_get_subdevice_name(info);
				//printf("Subdevice %d name : %s\n", sub, subname);
				foundsub = 1;
			}
			if (foundsub)
			{
				sprintf(devicename, "hw:%d,%d", card, device);
				gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combooutputdev), devicename, shortname);
				if (!devicepreset)
				{
					devicepreset = 1;
					g_object_set((gpointer)combooutputdev, "active-id", devicename, NULL);
				}
			}

			snd_pcm_info_free(info);
		}
		while(1);
		//printf("\n");
		count++;
	}
	while (1);
	//printf("%d cards found\n", count);
}

int main(int argc, char *argv[])
{
	int ret;

	/* This is called in all GTK applications. Arguments are parsed
	 * from the command line and are returned to the application. */
	gtk_init(&argc, &argv);

	/* create a new window */
	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
	gtk_container_set_border_width(GTK_CONTAINER (window), 2);
	gtk_widget_set_size_request(window, 100, 100);
	gtk_window_set_title(GTK_WINDOW(window), "Audio Effects");
	gtk_window_set_resizable(GTK_WINDOW(window), FALSE);

	/* When the window is given the "delete-event" signal (this is given
	* by the window manager, usually by the "close" option, or on the
	* titlebar), we ask it to call the delete_event () function
	* as defined above. The data passed to the callback
	* function is NULL and is ignored in the callback function. */
	g_signal_connect (window, "delete-event", G_CALLBACK (delete_event), (gpointer)&aq);
    
	/* Here we connect the "destroy" event to a signal handler.  
	* This event occurs when we call gtk_widget_destroy() on the window,
	* or if we return FALSE in the "delete-event" callback. */
	g_signal_connect(window, "destroy", G_CALLBACK (destroy), NULL);

	g_signal_connect(window, "realize", G_CALLBACK (realize_cb), NULL);

// vertical box
	fxbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
	gtk_container_add(GTK_CONTAINER(window), fxbox);

// config frame
	frameconf = gtk_frame_new("Configuration");
	gtk_container_add(GTK_CONTAINER(fxbox), frameconf);
	gtk_widget_set_size_request(frameconf, 300, 50);

// horizontal box
	confbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
	gtk_container_add(GTK_CONTAINER(frameconf), confbox);

// input devices combobox
	inputdevlabel = gtk_label_new("Input Device");
	gtk_widget_set_size_request(inputdevlabel, 100, 30);
	gtk_container_add(GTK_CONTAINER(confbox), inputdevlabel);
	comboinputdev = gtk_combo_box_text_new();
	gtk_container_add(GTK_CONTAINER(confbox), comboinputdev);

// frames
	frameslabel = gtk_label_new("Frames");
	gtk_widget_set_size_request(frameslabel, 100, 30);
	gtk_container_add(GTK_CONTAINER(confbox), frameslabel);
	spinbutton1 = gtk_spin_button_new_with_range(32.0, 1024.0 , 1.0);
	gtk_widget_set_size_request(spinbutton1, 120, 30);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(spinbutton1), (float)frames_default);
	g_signal_connect(GTK_SPIN_BUTTON(spinbutton1), "value-changed", G_CALLBACK(frames_changed), (gpointer)&aq);
	gtk_container_add(GTK_CONTAINER(confbox), spinbutton1);

// output devices combobox
	outputdevlabel = gtk_label_new("Output Device");
	gtk_widget_set_size_request(outputdevlabel, 100, 30);
	gtk_container_add(GTK_CONTAINER(confbox), outputdevlabel);
	combooutputdev = gtk_combo_box_text_new();
	gtk_container_add(GTK_CONTAINER(confbox), combooutputdev);

	print_card_list(); // Fill input/output devices combo boxes
	g_signal_connect(GTK_COMBO_BOX(comboinputdev), "changed", G_CALLBACK(inputdev_changed), (gpointer)&aq);
	g_signal_connect(GTK_COMBO_BOX(combooutputdev), "changed", G_CALLBACK(outputdev_changed), (gpointer)&aq);


// haas frame
	framehaas1 = gtk_frame_new("Haas");
	gtk_container_add(GTK_CONTAINER(fxbox), framehaas1);

// horizontal box
	haasbox1 = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 5);
	gtk_container_add(GTK_CONTAINER(framehaas1), haasbox1);

	haasenable = gtk_check_button_new_with_label("Haas");
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(haasenable), haasenabled);
	g_signal_connect(GTK_TOGGLE_BUTTON(haasenable), "toggled", G_CALLBACK(haas_toggled), NULL);
	gtk_container_add(GTK_CONTAINER(haasbox1), haasenable);

// haas delay
	haaslabel1 = gtk_label_new("Haas Delay (ms)");
	gtk_widget_set_size_request(haaslabel1, 100, 30);
	gtk_container_add(GTK_CONTAINER(haasbox1), haaslabel1);
	spinbutton2 = gtk_spin_button_new_with_range(1.0, 30.0 , 0.1);
	gtk_widget_set_size_request(spinbutton2, 120, 30);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(spinbutton2), millisec);
	g_signal_connect(GTK_SPIN_BUTTON(spinbutton2), "value-changed", G_CALLBACK(haasdly_changed), NULL);
	gtk_container_add(GTK_CONTAINER(haasbox1), spinbutton2);


// delay frame
	framedelay1 = gtk_frame_new("Delay");
	gtk_container_add(GTK_CONTAINER(fxbox), framedelay1);

// horizontal box
    dlybox1 = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_container_add(GTK_CONTAINER(framedelay1), dlybox1);

// checkbox
	if ((ret=pthread_mutex_init(&(snddly.delaymutex), NULL)))
	{
		printf("Delay mutex init failed, %d\n", ret);
	}
	snddly.enabled = FALSE;
	dlyenable = gtk_check_button_new_with_label("Enable");
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dlyenable), snddly.enabled);
	g_signal_connect(GTK_TOGGLE_BUTTON(dlyenable), "toggled", G_CALLBACK(dly_toggled), NULL);
	gtk_container_add(GTK_CONTAINER(dlybox1), dlyenable);

// combobox
	delaytypelabel = gtk_label_new("Delay Type");
	gtk_widget_set_size_request(delaytypelabel, 100, 30);
	gtk_container_add(GTK_CONTAINER(dlybox1), delaytypelabel);
	combodelaytype = gtk_combo_box_text_new();
	select_delay_types();
	g_signal_connect(GTK_COMBO_BOX(combodelaytype), "changed", G_CALLBACK(delaytype_changed), NULL);
	gtk_container_add(GTK_CONTAINER(dlybox1), combodelaytype);

// delay
	delaylabel1 = gtk_label_new("Delay (ms)");
	gtk_widget_set_size_request(delaylabel1, 100, 30);
	gtk_container_add(GTK_CONTAINER(dlybox1), delaylabel1);
	spinbutton5 = gtk_spin_button_new_with_range(0.0, 2000.0, 10.0);
	gtk_widget_set_size_request(spinbutton5, 120, 30);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(spinbutton5), 500.0);
	g_signal_connect(GTK_SPIN_BUTTON(spinbutton5), "value-changed", G_CALLBACK(delay_changed), NULL);
	gtk_container_add(GTK_CONTAINER(dlybox1), spinbutton5);

// feedback
	feedbacklabel1 = gtk_label_new("Feedback");
	gtk_widget_set_size_request(feedbacklabel1, 100, 30);
	gtk_container_add(GTK_CONTAINER(dlybox1), feedbacklabel1);
	spinbutton6 = gtk_spin_button_new_with_range(0.0, 0.95, 0.01);
	gtk_widget_set_size_request(spinbutton6, 120, 30);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(spinbutton6), 0.7);
	g_signal_connect(GTK_SPIN_BUTTON(spinbutton6), "value-changed", G_CALLBACK(feedback_changed), NULL);
	gtk_container_add(GTK_CONTAINER(dlybox1), spinbutton6);


// statusbar
	statusbar = gtk_statusbar_new();
	gtk_container_add(GTK_CONTAINER(fxbox), statusbar);

	gtk_widget_show_all(window);

	create_thread0(&aq);

	gtk_main();

	exit(0);
}
