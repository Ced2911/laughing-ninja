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
#include <ppc/timebase.h>

#ifdef HAVE_AV_CONFIG_H
#undef HAVE_AV_CONFIG_H
#endif
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavutil/audioconvert.h"
#include "libavutil/common.h"
#include "libavutil/imgutils.h"
#include "libavutil/mathematics.h"
#include "libavutil/samplefmt.h"
#include "libswscale/swscale.h"

#define HW_VIDEO_INFO_ADDR 0xec806100ULL
#define INBUF_SIZE 4096
#define AUDIO_INBUF_SIZE 20480
#define AUDIO_REFILL_THRESH 4096
#define DEFAULT_BITRATE 1200*1024
#define DEFAULT_CODEC_ID AV_CODEC_ID_HUFFYUV
#define DEFAULT_THREAD 5

#define STATS	1


static uint32_t * pUntiledFrameBuffer = NULL;
static unsigned char stack[5 * 0x10000]  __attribute__ ((aligned (128)));
static unsigned int sws_flags = SWS_POINT;
static volatile unsigned int libav_encoding = 0;

// config
static int screenWidth;
static int screenHeight;
static int bitrate = DEFAULT_BITRATE;
static int codec = DEFAULT_CODEC_ID;
static int thread_id = DEFAULT_THREAD;
static char capture_filename[512];

struct ati_info {
	uint32_t unknown1[4];
	uint32_t base;
	uint32_t unknown2[8];
	uint32_t width;
	uint32_t height;
} __attribute__ ((__packed__)) ;

static struct ati_info * hw_info;

// Linkage issues
void rmdir(){};

static int getVideoWidth() {
	return hw_info->width;
}

static int getVideoHeight() {
	return hw_info->height;
}

static int getBitrate() {
	return bitrate;
}

static enum AVCodecID getCodec() {
	return codec;
}

static uint32_t * getFrameBuffer(){
	return (uint32_t *)(long)(hw_info->base | 0x80000000);
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
			// pUntiledFrameBuffer[y * width + x] = fb[base];
			pUntiledFrameBuffer[y * width + x] = 0xFF | __builtin_bswap32(fb[base] >> 8);
		}
	}
	return pUntiledFrameBuffer;
}

static inline float msec_diff(uint64_t end, uint64_t start)
{
	return (float)(end-start)/(PPC_TIMEBASE_FREQ/1000);
}

/*
 * Video encoding example
 */
static int video_encode(const char *filename)
{
    AVCodec *codec;
    AVCodecContext *c= NULL;
    AVOutputFormat *fmt;
    AVFormatContext *oc;
    AVStream *video_st;
    struct SwsContext *img_convert_ctx;
    int i, ret, got_output;
    AVFrame *picture, *tmp_picture;
    AVPacket pkt;
    
    uint32_t * fb = getFrameBuffer();

    printf("Video encoding\n");
    av_register_all();
    
    /* Allocate the output media context. */
    oc = avformat_alloc_context();
    if (!oc) {
        fprintf(stderr, "Memory error\n");
        return 1;
    }
    
    /* Enumerate the codecs*/
	codec = av_codec_next(NULL);
	while(codec != NULL)
	{
		if(codec->encode2) {
			// fprintf(stderr, "%s\n", codec->long_name);
		}
		codec = av_codec_next(codec);
	}

    /* find the video encoder */
    codec = avcodec_find_encoder(getCodec());
    if (!codec) {
        fprintf(stderr, "codec not found\n");
        return 1;
    }
    
    /* Autodetect the output format from the name. default is mkv. */
    fmt = av_guess_format(NULL, filename, NULL);
    if (!fmt) {
        printf("Could not deduce output format from file extension: using mkv.\n");
        fmt = av_guess_format("avi", NULL, NULL);
    }
    if (!fmt) {
        fprintf(stderr, "Could not find suitable output format\n");
        return 1;
    }
        
    oc->oformat = fmt;
    snprintf(oc->filename, sizeof(oc->filename), "%s", filename);

    /* allocate video stream */
	video_st = avformat_new_stream(oc, codec);
    
    c = video_st->codec;
    picture= avcodec_alloc_frame();

    /* put sample parameters */
    c->bit_rate = getBitrate();
    /* resolution must be a multiple of two */
    c->width = getVideoWidth();
    c->height = getVideoHeight();
    /* frames per second */
    c->time_base= (AVRational){1,25};
    c->gop_size = 10; /* emit one intra frame every ten frames */
    //c->max_b_frames=1;
    
    c->pix_fmt = codec->pix_fmts[0];
    
    /* find best output format */
    enum PixelFormat * output_fmt = codec->pix_fmts;
    while (*output_fmt != AV_PIX_FMT_NONE) {
		if (*output_fmt == AV_PIX_FMT_RGB24) {
			c->pix_fmt = AV_PIX_FMT_RGB24;
			// don't break try to find a better format
		}
		
		/* best format */
		
		if (*output_fmt == AV_PIX_FMT_RGB32) {
			c->pix_fmt = AV_PIX_FMT_RGB32;
			break;
		}
		
		if (*output_fmt == AV_PIX_FMT_RGBA) {
			c->pix_fmt = AV_PIX_FMT_RGBA;
			break;
		}
		output_fmt++;
	}
	
	if (c->codec_id == AV_CODEC_ID_HUFFYUV) {
		c->prediction_method = FF_PRED_LEFT;
	}
    
	/* Some formats want stream headers to be separate. */
    if (oc->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= CODEC_FLAG_GLOBAL_HEADER;

    /* open codec */
    if (avcodec_open2(c, NULL, NULL) < 0) {
        fprintf(stderr, "could not open codec\n");
		return 1;
    }

	if (avio_open(&oc->pb, filename, AVIO_FLAG_WRITE) < 0) {
		fprintf(stderr, "Could not open '%s'\n", filename);
		return 1;
	}

    ret = av_image_alloc(picture->data, picture->linesize, c->width, c->height,
                         c->pix_fmt, 32);
    if (ret < 0) {
        fprintf(stderr, "could not alloc raw picture buffer\n");
        return 1;
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
	
	/* open the output file, if needed */
	if (avio_open(&oc->pb, filename, AVIO_FLAG_WRITE) < 0) {
		fprintf(stderr, "Could not open '%s'\n", filename);
		return 1;
	}
      
	av_dump_format(oc, 0, filename, 1);
	
	/* Write the stream header, if any. */
    avformat_write_header(oc, NULL);
    
#if STATS
	uint64_t start, end;
#endif
    while (libav_encoding) {
#if STATS
		start = mftb();
#endif
		tmp_picture->data[0] = getUntiledFrameBuffer(c->width, c->height);
#if STATS
		end = mftb();
		printf("getUntiledFrameBuffer: %f msec\n", msec_diff(end, start));
#endif
		tmp_picture->linesize[0] = c->width * 4;
        av_init_packet(&pkt);
        pkt.data = NULL;    // packet data will be allocated by the encoder
        pkt.size = 0;

        fflush(stdout);
#if STATS
		start = mftb();
#endif    
		// no scale
		if ((c->pix_fmt == AV_PIX_FMT_RGBA) || (c->pix_fmt == AV_PIX_FMT_RGB32)) {
			picture->data[0] = tmp_picture->data[0];
			picture->linesize[0] = tmp_picture->linesize[0];
		} else {
			// convert picture
			sws_scale(img_convert_ctx, tmp_picture->data, tmp_picture->linesize, 0, c->height, picture->data, picture->linesize);
		}
#if STATS
		end = mftb();
		printf("sws_scale: %f msec\n", msec_diff(end, start));
#endif        
        picture->pts = i;

        /* encode the image */
#if STATS
		start = mftb();
#endif
        ret = avcodec_encode_video2(c, &pkt, picture, &got_output);
#if STATS
		end = mftb();
		printf("avcodec_encode_video2: %f msec\n", msec_diff(end, start));
#endif
        if (ret < 0) {
            fprintf(stderr, "error encoding frame\n");
            return 1;
        }

        if (got_output) {
            av_interleaved_write_frame(oc, &pkt);
        }
        
        i++;
    }
    
    /* Write the trailer, if any. The trailer must be written before you
     * close the CodecContexts open when you wrote the header; otherwise
     * av_write_trailer() may try to use memory that was freed on
     * av_codec_close(). */
    av_write_trailer(oc);

    avcodec_close(c);
    /* Free the streams. */
    for (i = 0; i < oc->nb_streams; i++) {
        av_freep(&oc->streams[i]->codec);
        av_freep(&oc->streams[i]);
    }
    
    av_freep(&picture->data[0]);
    avcodec_free_frame(&picture);
    
	avio_close(oc->pb);

    /* free the stream */
    av_free(oc);
    
    printf("\n");
    
    return 0;
}



static void encoding_thread() {
	video_encode(capture_filename);
}


void xenon_caps_init(const char * filename) {	
	hw_info = (struct ati_info*)HW_VIDEO_INFO_ADDR;
	if (pUntiledFrameBuffer == NULL) {
		pUntiledFrameBuffer = malloc(hw_info->height*hw_info->width*sizeof(uint32_t));
	}
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
