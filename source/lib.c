#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <debug.h>
#include <xenos/xenos.h>
#include <diskio/ata.h>
#include <input/input.h>
#include <console/console.h>
#include <diskio/disc_io.h>
#include <sys/iosupport.h>
#include <usb/usbmain.h>
#include <time/time.h>
#include <ppc/timebase.h>
#include <xenon_soc/xenon_power.h>
#include <elf/elf.h>
#include <dirent.h>

#ifdef HAVE_AV_CONFIG_H
#undef HAVE_AV_CONFIG_H
#endif

#include "libavcodec/avcodec.h"
#include "libavutil/audioconvert.h"
#include "libavutil/common.h"
#include "libavutil/imgutils.h"
#include "libavutil/mathematics.h"
#include "libavutil/samplefmt.h"
#include "libswscale/swscale.h"

#define INBUF_SIZE 4096
#define AUDIO_INBUF_SIZE 20480
#define AUDIO_REFILL_THRESH 4096
#define DEFAULT_BITRATE 1200*1024
#define DEFAULT_CODEC_ID AV_CODEC_ID_MPEG4
#define DEFAULT_THREAD 5


static uint32_t * pUntiledFrameBuffer = NULL;
static unsigned char stack[5 * 0x10000]  __attribute__ ((aligned (128)));
static unsigned int sws_flags = SWS_BICUBIC;
static volatile unsigned int libav_encoding = 0;

// config
static int screenWidth;
static int screenHeight;
static int bitrate = DEFAULT_BITRATE;
static int codec = DEFAULT_CODEC_ID;
static int thread_id = DEFAULT_THREAD;
static char capture_filename[512];

// Linkage issues
void rmdir(){};

static int getVideoWidth() {
	return screenWidth;
}

static int getVideoHeight() {
	return screenHeight;
}

static int getBitrate() {
	return bitrate;
}

static enum AVCodecID getCodec() {
	return codec;
}

static uint32_t * getFrameBuffer(){
	//return (uint32_t *)(0xec806110 | 0x80000000);
	return (uint32_t *)0x9e000000ULL;
}


static uint32_t * getUntiledFrameBuffer() {
	
	int y,x;
	int width = getVideoWidth();
	int height = getVideoHeight();
	
	uint32_t * fb = getFrameBuffer();
	for(y=0;y<height;y++) {
		for(x=0;x<width;x++) {
			unsigned int base = ((((y & ~31) * width) + (x & ~31)*32) +
				(((x & 3) + ((y & 1) << 2) + ((x & 28) << 1) + ((y & 30) << 5)) ^ ((y & 8) << 2)));
			pUntiledFrameBuffer[y * width + x] = fb[base];
		}
	}
	return pUntiledFrameBuffer;
}



/*
 * Video encoding example
 */
static void video_encode(const char *filename)
{
    AVCodec *codec;
    AVCodecContext *c= NULL;
    struct SwsContext *img_convert_ctx;
    int i, ret, x, y, got_output;
    FILE *f;
    AVFrame *picture, *tmp_picture;
    AVPacket pkt;
    uint8_t endcode[] = { 0, 0, 1, 0xb7 };
    
    uint32_t * fb = getFrameBuffer();

    printf("Video encoding\n");
    av_register_all();
    
    /* Enumerate the codecs*/
	codec = av_codec_next(NULL);
	while(codec != NULL)
	{
		if(codec->encode2) {
			// fprintf(stderr, "%s\n", codec->long_name);
		}
		codec = av_codec_next(codec);
	}

    /* find the mpeg1 video encoder */
    codec = avcodec_find_encoder(getCodec());
    if (!codec) {
        fprintf(stderr, "codec not found\n");
        exit(1);
    }

    c = avcodec_alloc_context3(codec);
    picture= avcodec_alloc_frame();

    /* put sample parameters */
    c->bit_rate = getBitrate();
    /* resolution must be a multiple of two */
    c->width = getVideoWidth();
    c->height = getVideoHeight();
    /* frames per second */
    c->time_base= (AVRational){1,25};
    c->gop_size = 10; /* emit one intra frame every ten frames */
    c->max_b_frames=1;
    c->pix_fmt = PIX_FMT_YUV420P;

    /* open it */
    if (avcodec_open2(c, codec, NULL) < 0) {
        fprintf(stderr, "could not open codec\n");
        exit(1);
    }

    f = fopen(filename, "wb");
    if (!f) {
        fprintf(stderr, "could not open %s\n", filename);
        exit(1);
    }

    ret = av_image_alloc(picture->data, picture->linesize, c->width, c->height,
                         c->pix_fmt, 32);
    if (ret < 0) {
        fprintf(stderr, "could not alloc raw picture buffer\n");
        exit(1);
    }
    picture->format = c->pix_fmt;
    picture->width  = c->width;
    picture->height = c->height;
    
    tmp_picture = avcodec_alloc_frame();
    
    img_convert_ctx = sws_getContext(c->width, c->height,
	  AV_PIX_FMT_BGRA,
	  c->width, c->height,
	  c->pix_fmt,
	  sws_flags, NULL, NULL, NULL);

    while (libav_encoding) {
		tmp_picture->data[0] = getUntiledFrameBuffer(c->width, c->height);
		tmp_picture->linesize[0] = c->width * 4;
        av_init_packet(&pkt);
        pkt.data = NULL;    // packet data will be allocated by the encoder
        pkt.size = 0;

        fflush(stdout);
        
        // convert picture
		sws_scale(img_convert_ctx, tmp_picture->data, tmp_picture->linesize, 0, c->height, picture->data, picture->linesize);
        
        picture->pts = i;

        /* encode the image */
        ret = avcodec_encode_video2(c, &pkt, picture, &got_output);
        if (ret < 0) {
            fprintf(stderr, "error encoding frame\n");
            exit(1);
        }

        if (got_output) {
            fwrite(pkt.data, 1, pkt.size, f);
            av_free_packet(&pkt);
        }
        
        i++;
    }

    /* get the delayed frames */
    for (got_output = 1; got_output; i++) {
        fflush(stdout);

        ret = avcodec_encode_video2(c, &pkt, NULL, &got_output);
        if (ret < 0) {
            fprintf(stderr, "error encoding frame\n");
            exit(1);
        }

        if (got_output) {
            fwrite(pkt.data, 1, pkt.size, f);
            av_free_packet(&pkt);
        }
    }

    /* add sequence end code to have a real mpeg file */
    fwrite(endcode, 1, sizeof(endcode), f);
    fclose(f);

    avcodec_close(c);
    av_free(c);
    av_freep(&picture->data[0]);
    avcodec_free_frame(&picture);
    printf("\n");
}



static void encoding_thread() {
	video_encode(capture_filename);
}


void xenon_caps_init(const char * filename) {
	if (pUntiledFrameBuffer == NULL) {
		pUntiledFrameBuffer = malloc(1280*720*sizeof(uint32_t));
	}
	screenWidth = 1280;
	screenHeight = 720;
	strcpy(capture_filename, filename);
}

void xenon_caps_start() {
	if (libav_encoding)
		return;
		
	libav_encoding = 1;
	xenon_run_thread_task(thread_id, stack + ( thread_id* 0x10000) - 0x1000, encoding_thread);
}
void xenon_caps_end() {
	// stop it !
	libav_encoding = 0;	
	while(xenon_is_thread_task_running(thread_id));
}
void xenon_caps_set_codec(int codecid) {
	if (libav_encoding == 0)
		codec = codecid;
}
void xenon_caps_set_bitrate(int br) {
	if (libav_encoding == 0)
		bitrate = br;
}
void xenon_caps_set_hw_thread(int t) {
	if (libav_encoding == 0)
		thread_id = t;
}
