#include "live_mux.h"
#include "h264.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#if VAVA_LIVE_AV
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libswresample/swresample.h>
#define OUTPUT_MAX (512u*1024u)
struct live_mux {
    live_emit emit;void *arg;bool failed,started;uint64_t origin,last_video_ms;uint32_t epoch,fragment_sequence;
    struct h264_config config;
    AVFormatContext *format;AVIOContext *io;AVCodecContext *decoder,*encoder;SwrContext *resample;AVAudioFifo *fifo;
    AVPacket *video;int64_t video_pts,audio_next,audio_input_end;bool audio_clock,fragment_clock;int64_t fragment_at;
    int64_t anchors[2];bool anchored[2];
    uint8_t pending[OUTPUT_MAX],init[65536],fragment[OUTPUT_MAX];size_t used,init_size,fragment_size;
};
static uint32_t u32(const uint8_t *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void p32(uint8_t *p,uint32_t n){p[0]=(uint8_t)(n>>24);p[1]=(uint8_t)(n>>16);p[2]=(uint8_t)(n>>8);p[3]=(uint8_t)n;}
static bool patch_traf(struct live_mux *m,uint8_t *p,size_t n){
    unsigned track=0;uint8_t *tfdt=NULL;size_t tfdt_size=0;
    for(size_t at=8;at+8<=n;){size_t size=u32(p+at);if(size<8||size>n-at)return false;
        if(!memcmp(p+at+4,"tfhd",4)){if(size<16)return false;track=u32(p+at+12);}
        if(!memcmp(p+at+4,"tfdt",4)){tfdt=p+at;tfdt_size=size;}at+=size;
    }
    if(track<1||track>2||!tfdt||tfdt_size<20||tfdt[8]!=1||!m->anchored[track-1])return false;
    uint64_t old=mvm_u64(tfdt+12),anchor=(uint64_t)m->anchors[track-1];if(anchor>UINT64_MAX-old)return false;
    mvm_p64(tfdt+12,old+anchor);return true;
}
static bool patch_fragment(struct live_mux *m,uint8_t *p,size_t n){
    for(size_t at=8;at+8<=n;){size_t size=u32(p+at);if(size<8||size>n-at)return false;
        if(!memcmp(p+at+4,"traf",4)&&!patch_traf(m,p+at,size))return false;
        if(!memcmp(p+at+4,"mfhd",4)){if(size<16||m->fragment_sequence==UINT32_MAX)return false;p32(p+at+12,++m->fragment_sequence);}at+=size;
    }return true;
}
#if LIBAVFORMAT_VERSION_MAJOR < 61
static int output(void *arg,uint8_t *data,int len){
#else
static int output(void *arg,const uint8_t *data,int len){
#endif
    struct live_mux *m=arg;if(m->failed)return AVERROR(EIO);if(len<0||(size_t)len>sizeof(m->pending)-m->used)return AVERROR(ENOSPC);
    memcpy(m->pending+m->used,data,(size_t)len);m->used+=(size_t)len;size_t at=0;
    while(m->used-at>=8){size_t n=u32(m->pending+at);if(n<8||n>OUTPUT_MAX)return AVERROR_INVALIDDATA;if(n>m->used-at)break;uint8_t *box=m->pending+at;
        if(!memcmp(box+4,"ftyp",4)){if(n>sizeof(m->init))return AVERROR_INVALIDDATA;memcpy(m->init,box,n);m->init_size=n;}
        else if(!memcmp(box+4,"moov",4)){if(n>sizeof(m->init)-m->init_size)return AVERROR_INVALIDDATA;memcpy(m->init+m->init_size,box,n);m->init_size+=n;if(!m->emit(m->arg,LIVE_INIT,m->epoch,m->init,m->init_size))return AVERROR(EIO);}
        else if(!memcmp(box+4,"moof",4)){if(m->fragment_size||!patch_fragment(m,box,n))return AVERROR_INVALIDDATA;memcpy(m->fragment,box,n);m->fragment_size=n;}
        else if(!memcmp(box+4,"mdat",4)){if(!m->fragment_size||n>OUTPUT_MAX-m->fragment_size)return AVERROR_INVALIDDATA;memcpy(m->fragment+m->fragment_size,box,n);m->fragment_size+=n;if(!m->emit(m->arg,LIVE_MEDIA,m->epoch,m->fragment,m->fragment_size))return AVERROR(EIO);m->fragment_size=0;}
        at+=n;
    }
    memmove(m->pending,m->pending+at,m->used-at);m->used-=at;return len;
}
static void close_format(struct live_mux *m){
    if(m->format){if(!m->failed){if(av_write_frame(m->format,NULL)<0)m->failed=true;else if(av_write_trailer(m->format)<0)m->failed=true;}m->format->pb=NULL;avformat_free_context(m->format);m->format=NULL;}
    if(m->io){av_freep(&m->io->buffer);avio_context_free(&m->io);}
}
static bool open_format(struct live_mux *m){
    if(avformat_alloc_output_context2(&m->format,NULL,"mp4",NULL)<0||!m->format)return false;
    uint8_t *buffer=av_malloc(32768);if(!buffer)return false;m->io=avio_alloc_context(buffer,32768,1,m,NULL,output,NULL);if(!m->io){av_free(buffer);return false;}
    m->format->pb=m->io;m->format->flags|=AVFMT_FLAG_CUSTOM_IO;m->format->avoid_negative_ts=AVFMT_AVOID_NEG_TS_DISABLED;
    AVStream *v=avformat_new_stream(m->format,NULL),*a=avformat_new_stream(m->format,NULL);if(!v||!a)return false;
    v->id=1;v->time_base=(AVRational){1,90000};v->codecpar->codec_type=AVMEDIA_TYPE_VIDEO;v->codecpar->codec_id=AV_CODEC_ID_H264;v->codecpar->width=(int)m->config.width;v->codecpar->height=(int)m->config.height;
    uint8_t sps[2048];size_t ns=h264_sps_low_delay(m->config.active_sps,m->config.sps_size,sps,sizeof(sps));if(!ns)return false;
    size_t bytes=ns+m->config.pps_size;v->codecpar->extradata=av_mallocz(bytes+AV_INPUT_BUFFER_PADDING_SIZE);if(!v->codecpar->extradata)return false;
    memcpy(v->codecpar->extradata,sps,ns);memcpy(v->codecpar->extradata+ns,m->config.active_pps,m->config.pps_size);v->codecpar->extradata_size=(int)bytes;
    a->id=2;a->time_base=(AVRational){1,48000};if(avcodec_parameters_from_context(a->codecpar,m->encoder)<0)return false;
    m->used=m->init_size=m->fragment_size=0;m->fragment_clock=false;memset(m->anchored,0,sizeof(m->anchored));
    char config[256];const uint8_t *s=m->config.active_sps+4;int n=snprintf(config,sizeof(config),"{\"mime\":\"video/mp4; codecs=\\\"avc1.%02x%02x%02x,mp4a.40.2\\\"\",\"width\":%u,\"height\":%u}",s[1],s[2],s[3],m->config.width,m->config.height);
    if(n<0||(size_t)n>=sizeof(config)||!m->emit(m->arg,LIVE_CONFIG,m->epoch,(const uint8_t *)config,(size_t)n))return false;
    AVDictionary *opts=NULL;av_dict_set(&opts,"movflags","frag_custom+empty_moov+default_base_moof+negative_cts_offsets",0);av_dict_set(&opts,"use_editlist","0",0);av_dict_set(&opts,"video_track_timescale","90000",0);
    int result=avformat_write_header(m->format,&opts);av_dict_free(&opts);avio_flush(m->io);return result>=0&&m->io->error>=0;
}
static bool packet(struct live_mux *m,AVPacket *p,unsigned track){
    if(p->pts<0)return true;
    int64_t when=av_rescale_q(p->dts,track?(AVRational){1,48000}:(AVRational){1,90000},(AVRational){1,1000});
    if(!m->anchored[track]){m->anchors[track]=p->dts;m->anchored[track]=true;}
    p->pts-=m->anchors[track];p->dts-=m->anchors[track];p->stream_index=(int)track;
    int result=av_write_frame(m->format,p);if(result<0)return false;
    /* Bound append frequency while retaining short audio/video fragments. */
    if(!m->fragment_clock){m->fragment_clock=true;m->fragment_at=when;}
    if(when-m->fragment_at<150)return true;
    m->fragment_at=when;
    if(av_write_frame(m->format,NULL)<0)return false;avio_flush(m->io);return m->io->error>=0;
}
static bool encoded_audio(struct live_mux *m){
    AVPacket *p=av_packet_alloc();if(!p)return false;int result;bool ok=true;
    while((result=avcodec_receive_packet(m->encoder,p))>=0){if(!packet(m,p,1)){ok=false;break;}av_packet_unref(p);}
    av_packet_free(&p);return ok&&(result==AVERROR(EAGAIN)||result==AVERROR_EOF);
}
static bool audio(struct live_mux *m,const struct mvm_sample *s){
    size_t at=0;int64_t bundle_samples=0;while(at<s->size){const uint8_t *p=s->data+at;size_t remain=s->size-at;
        if(remain<7||p[0]!=255||(p[1]&0xf6)!=0xf0||p[2]>>6!=1||((p[2]>>2)&15)!=11||(((p[2]&1)<<2)|(p[3]>>6))!=1||(p[6]&3))return false;
        size_t n=((size_t)(p[3]&3)<<11)|((size_t)p[4]<<3)|(p[5]>>5);if(n<7||n>remain)return false;
        int64_t when=(int64_t)(s->timestamp-m->origin)*48+bundle_samples;bundle_samples+=6144;
        if(m->audio_clock&&when+12000<m->audio_input_end){fprintf(stderr,"live audio clock backwards: input=%lld expected=%lld\n",(long long)when,(long long)m->audio_input_end);return false;}
        if(m->audio_clock&&when>m->audio_input_end+12000){if(when-m->audio_input_end>48000*15)return false;av_audio_fifo_reset(m->fifo);m->audio_next=when;m->audio_input_end=when;if(m->resample){swr_close(m->resample);if(swr_init(m->resample)<0)return false;}}
        m->audio_input_end=m->audio_clock?m->audio_input_end+6144:when+6144;
        AVPacket *pkt=av_packet_alloc();AVFrame *f=av_frame_alloc();if(!pkt||!f){av_packet_free(&pkt);av_frame_free(&f);return false;}
        if(av_new_packet(pkt,(int)n)<0){av_packet_free(&pkt);av_frame_free(&f);return false;}memcpy(pkt->data,p,n);
        int r=avcodec_send_packet(m->decoder,pkt);av_packet_free(&pkt);if(r<0){av_frame_free(&f);return false;}
        while((r=avcodec_receive_frame(m->decoder,f))>=0){
            if(!m->resample){if(swr_alloc_set_opts2(&m->resample,&m->encoder->ch_layout,m->encoder->sample_fmt,48000,&f->ch_layout,(enum AVSampleFormat)f->format,f->sample_rate,0,NULL)<0||swr_init(m->resample)<0){av_frame_free(&f);return false;}}
            AVFrame *converted=av_frame_alloc();if(!converted){av_frame_free(&f);return false;}converted->format=m->encoder->sample_fmt;converted->sample_rate=48000;converted->nb_samples=(int)av_rescale_rnd(swr_get_delay(m->resample,f->sample_rate)+f->nb_samples,48000,f->sample_rate,AV_ROUND_UP);av_channel_layout_copy(&converted->ch_layout,&m->encoder->ch_layout);
            if(converted->nb_samples>16384||av_frame_get_buffer(converted,0)<0){av_frame_free(&converted);av_frame_free(&f);return false;}
            int samples=swr_convert(m->resample,converted->data,converted->nb_samples,(const uint8_t **)f->extended_data,f->nb_samples);
            if(samples<0||av_audio_fifo_size(m->fifo)+samples>32768){av_frame_free(&converted);av_frame_free(&f);return false;}
            if(!m->audio_clock){m->audio_next=when;m->audio_clock=true;}
            if(av_audio_fifo_write(m->fifo,(void **)converted->data,samples)!=samples){av_frame_free(&converted);av_frame_free(&f);return false;}av_frame_free(&converted);
            while(av_audio_fifo_size(m->fifo)>=m->encoder->frame_size){AVFrame *out=av_frame_alloc();if(!out){av_frame_free(&f);return false;}out->format=m->encoder->sample_fmt;out->sample_rate=48000;out->nb_samples=m->encoder->frame_size;av_channel_layout_copy(&out->ch_layout,&m->encoder->ch_layout);
                if(av_frame_get_buffer(out,0)<0||av_audio_fifo_read(m->fifo,(void **)out->data,out->nb_samples)!=out->nb_samples){av_frame_free(&out);av_frame_free(&f);return false;}
                out->pts=m->audio_next;m->audio_next+=out->nb_samples;int sent=avcodec_send_frame(m->encoder,out);av_frame_free(&out);if(sent<0||!encoded_audio(m)){av_frame_free(&f);return false;}
            }av_frame_unref(f);
        }
        av_frame_free(&f);if(r!=AVERROR(EAGAIN)&&r!=AVERROR_EOF)return false;at+=n;
    }return true;
}
bool live_mux_available(void){return true;}
struct live_mux *live_mux_new(live_emit emit,void *arg){
    struct live_mux *m=calloc(1,sizeof(*m));if(!m)return NULL;m->emit=emit;m->arg=arg;
    m->decoder=avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_AAC));m->encoder=avcodec_alloc_context3(avcodec_find_encoder(AV_CODEC_ID_AAC));
    if(!m->decoder||!m->encoder)goto fail;m->encoder->log_level_offset=16;m->encoder->sample_rate=48000;m->encoder->sample_fmt=AV_SAMPLE_FMT_FLTP;m->encoder->time_base=(AVRational){1,48000};m->encoder->bit_rate=32000;m->encoder->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;av_channel_layout_default(&m->encoder->ch_layout,1);
    if(avcodec_open2(m->decoder,m->decoder->codec,NULL)<0||avcodec_open2(m->encoder,m->encoder->codec,NULL)<0)goto fail;
    m->fifo=av_audio_fifo_alloc(m->encoder->sample_fmt,1,32768);m->video=av_packet_alloc();if(!m->fifo||!m->video)goto fail;return m;
fail:live_mux_free(m);return NULL;
}
static bool push(struct live_mux *m,const struct mvm_sample *s){
    if(!m||m->failed||!s->size||s->size>MVM_FRAME_MAX)return false;
    if(m->started&&s->audio&&s->timestamp<m->origin&&m->origin-s->timestamp<1000)return true;
    if(m->started&&(s->timestamp<m->origin||s->timestamp-m->origin>INT64_MAX/90000))return false;
    if(s->audio){if(!m->started)return true;return audio(m,s);}
    bool key,changed;if(h264_config_frame(&m->config,s->data,s->size,&key,&changed)!=1||key!=s->key)return false;
    if(!m->started){m->origin=s->timestamp;m->started=true;}
    int64_t pts=(int64_t)(s->timestamp-m->origin)*90;
    if(m->video->size){if(pts<=m->video_pts||pts-m->video_pts>15000*90)return false;m->video->duration=pts-m->video_pts;if(!packet(m,m->video,0))return false;av_packet_unref(m->video);}
    if(s->epoch!=m->epoch){if(!key)return false;close_format(m);m->epoch=s->epoch;if(!open_format(m))return false;}
    else if(changed)return false;
    if(av_new_packet(m->video,(int)s->size)<0)return false;size_t copied=h264_without_parameter_sets(s->data,s->size,m->video->data,s->size);if(!copied)return false;av_shrink_packet(m->video,(int)copied);m->video->pts=m->video->dts=pts;m->video->flags=key?AV_PKT_FLAG_KEY:0;m->video_pts=pts;m->last_video_ms=s->timestamp;return true;
}
bool live_mux_push(struct live_mux *m,const struct mvm_sample *s){if(!m)return false;bool ok=push(m,s);if(!ok){fprintf(stderr,"live mux rejected: epoch=%u audio=%u key=%u timestamp=%llu origin=%llu bytes=%zu\n",s->epoch,s->audio,s->key,(unsigned long long)s->timestamp,(unsigned long long)m->origin,s->size);m->failed=true;}return ok;}
void live_mux_free(struct live_mux *m){if(!m)return;m->failed=true;close_format(m);av_packet_free(&m->video);avcodec_free_context(&m->decoder);avcodec_free_context(&m->encoder);swr_free(&m->resample);av_audio_fifo_free(m->fifo);free(m);}
#else
struct live_mux {int unused;};
bool live_mux_available(void){return false;}
struct live_mux *live_mux_new(live_emit emit,void *arg){(void)emit;(void)arg;return NULL;}
bool live_mux_push(struct live_mux *m,const struct mvm_sample *s){(void)m;(void)s;return false;}
void live_mux_free(struct live_mux *m){free(m);}
#endif
