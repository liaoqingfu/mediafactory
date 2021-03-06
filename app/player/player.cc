#include <list>
#include <atomic>
#include <deque>
#include <unistd.h>
#include <thread>
#include <mutex>

#include "ffmpegdemux.h"
#include "ffmpegdec.h"
#include "ffmpegscale.h"
#include "ffmpegresample.h"
#include "sdlplay.h"
#include "player.h"

#define AVCODEC_MAX_AUDIO_FRAME_SIZE 192000

struct PACKETINFO
{
	PACKETINFO(){iUsage=0;}
	PACKETINFO( const PACKETINFO &packet )
	{
		memcpy(data,packet.data,sizeof(data));
		iSize=packet.iSize;
		iUsage=packet.iUsage;
		ipts=packet.ipts;
		streamid = packet.streamid;
		codecid = packet.codecid;
	}
	uint8_t data[AVCODEC_MAX_AUDIO_FRAME_SIZE];
	int iSize;
	int iUsage;
	int ipts;
	int streamid;
	int codecid;
};

typedef struct
{
	volatile double lfAudioClock;
	volatile double lfVideoClock;

	std::mutex audiomutex;	
	std::deque<PACKETINFO> queueAudioFrame;
	std::mutex videomutex;	
	std::deque<PACKETINFO> queueVideoFrame;

	std::thread threadeventloop;
	void* demuxhandle;
	void* sdlhandle;
	void* rescalehandle;
	void* resamplehandle;
}playerdesc_t;

void AudioCallback(void *userdata, uint8_t *stream, int len)
{
	playerdesc_t *pVideoState = (playerdesc_t *)userdata;
	uint8_t *pBuf=stream;
	int length=len;
	int iFilledSize=0;
	while( length>0 )
	{
		pVideoState->audiomutex.lock();
		if( !pVideoState->queueAudioFrame.empty() )
		{
			PACKETINFO *pPacket = &pVideoState->queueAudioFrame.front();
			double dClock=-ffmpegdemux_getclock(pVideoState->demuxhandle, pPacket->streamid, pPacket->ipts);
			pVideoState->lfAudioClock=dClock;
			printf("Audio Clock=%lf %lf %d %d \n",pVideoState->lfAudioClock, dClock, pPacket->streamid, pPacket->ipts);

			int iPacketSize=pPacket->iSize-pPacket->iUsage;
			int len1=length-iPacketSize;
			if( len1>=0 )
			{
				memcpy(pBuf,pPacket->data+pPacket->iUsage,iPacketSize);
				pBuf+=iPacketSize;
				pVideoState->queueAudioFrame.pop_front();
			}
			else if( len1<0 )
			{
				memcpy(pBuf,pPacket->data+pPacket->iUsage,length);
				pPacket->iUsage+=length;
			}
			length=len1;
		}
		else
		{
			memset(stream,0,len);
		}
		pVideoState->audiomutex.unlock();
	}
}

int VideoThread(void *arg)
{
	if( !arg )return -1;

	playerdesc_t *pVideoState=(playerdesc_t*)arg;
	ffmpegdemuxframe_t frame;
	image_scaled image;

	while( 1 )
	{
		if( fabs(pVideoState->lfVideoClock - pVideoState->lfAudioClock) * 1000 > 100 )
		{
			printf("clock wait %lf %lf\n", pVideoState->lfVideoClock, pVideoState->lfAudioClock);
			usleep(10 * 1000);
			continue;
		}

		pVideoState->videomutex.lock();
		if( !pVideoState->queueVideoFrame.empty() )
		{
			PACKETINFO *packet = &pVideoState->queueVideoFrame.front();
//			printf("videoframe size=%d \n", packet->iSize);
			int ret = ffmpegdemux_decode(pVideoState->demuxhandle, packet->codecid, packet->data, packet->iSize, &frame);
			if( ret == 0 )
			{
				if( !pVideoState->rescalehandle )
				{
					pVideoState->rescalehandle = scale_open(frame.info.image.width, frame.info.image.height, frame.info.image.pix_fmt);
					if( sdlplay_set_video(pVideoState->sdlhandle, "wwl", frame.info.image.width, frame.info.image.height) < 0 )
					{
						printf("SDL_InitializeVideo error \n");
						return -1;
					}					
				}

				double dClock=-ffmpegdemux_getclock(pVideoState->demuxhandle, packet->streamid, packet->ipts);
	//			printf("Video Clock=%lf %d %d \n",dClock, packet->streamid, packet->ipts);
				pVideoState->lfVideoClock=dClock;

				scale_image(pVideoState->rescalehandle, frame.data, frame.linesize, &image);
				sdlplay_display_yuv(pVideoState->sdlhandle, image.data, image.linesize);			
			}


			pVideoState->queueVideoFrame.pop_front();
		}
		pVideoState->videomutex.unlock();

		usleep(10 * 1000);
	}
	return 1;
}

//////////////////////////////////////////////////
void* player_open()
{
	playerdesc_t *inst = new playerdesc_t;
	inst->lfVideoClock=0.0;
	inst->lfAudioClock=0.0;
	inst->resamplehandle = inst->rescalehandle = NULL;

	return inst;
}

int player_play(void* handle, const char* url)
{
	playerdesc_t *inst = (playerdesc_t*)handle;

	//initialize ffmpeg
	void* demuxhandle = ffmpegdemux_open(url);
	if( !demuxhandle )
	{
		printf("ffmpegdemux_open error \n");
		return NULL;		
	}
	inst->demuxhandle = demuxhandle;

	void* sdlhandle = sdlplay_open(NULL);
	if( !sdlhandle )
	{
		printf("SDL_Initialize error \n");
		return NULL;		
	}
	inst->sdlhandle = sdlhandle;

    inst->threadeventloop = std::thread(VideoThread, (void*)inst);

/////////////////////////////////////////////////
	ffmpegdemuxpacket_t packet; 
	ffmpegdemuxframe_t frame;
	sound_resampled soundframe;
	void* resamplehandle = NULL;

	while( 1 )
	{
		inst->videomutex.lock();
		int iVideoLen=inst->queueVideoFrame.size();
		inst->videomutex.unlock();

		inst->audiomutex.lock();
		int iAudioLen=inst->queueAudioFrame.size();
		inst->audiomutex.unlock();

		if( iVideoLen>5	&& iAudioLen>5 )
		{
			printf("audiolistsize=%d,videolistsize=%d\n",iAudioLen,iVideoLen);
			usleep(100 * 1000 );
			continue;
		}

		if( ffmpegdemux_read(demuxhandle, &packet) < 0 )
			break;

		if( packet.codec_type == 0 )
		{
			PACKETINFO info;
			info.ipts=packet.pts;
			memcpy(info.data, packet.data, packet.size);
			info.iSize=packet.size;
			info.streamid = packet.stream_index;
			info.codecid = packet.codec_id;

			inst->videomutex.lock();
			inst->queueVideoFrame.push_back(info);
			inst->videomutex.unlock();
		}
		else if( packet.codec_type == 1 )
		{
			int ret = ffmpegdemux_decode(demuxhandle, packet.codec_id, packet.data, packet.size, &frame);
			if( ret == 0 )
			{
				if( !resamplehandle )
				{
					resamplehandle = resample_open(frame.info.sample.channels, 
						frame.info.sample.sample_fmt, frame.info.sample.sample_rate);
					inst->resamplehandle = resamplehandle;

					if( sdlplay_set_audio(inst->sdlhandle, frame.info.sample.sample_rate,
						frame.info.sample.channels, handle, AudioCallback) < 0 )
					{
						printf("sdlplay_set_audio error\n");
						return -1;
					}					
				}

				PACKETINFO info;
				info.ipts=packet.pts;

				resample_sound(resamplehandle, frame.data, frame.linesize, 
					frame.info.sample.nb_samples, &soundframe);

				memcpy(info.data, soundframe.data, soundframe.linesize);
				info.iSize=soundframe.linesize;
				info.streamid = packet.stream_index;
				info.codecid = packet.codec_id;

				inst->audiomutex.lock();
				inst->queueAudioFrame.push_back(info);
				inst->audiomutex.unlock();
			}
		}

		usleep(10 * 1000);
	}

	//-------------------release resource
// 	if( pFrameYUV )
// 		av_free(pFrameYUV);
// 	if( pFrameAudio )
// 		av_free(pFrameAudio);
// 
// 	if( pResampleCtx )
// 		avresample_free(&pResampleCtx);
// 
// 	if( pVideoCodecCtx )
// 		avcodec_close(pVideoCodecCtx);
// 	if( pAudioCodecCtx )
// 		avcodec_close(pAudioCodecCtx);
// 	if( pFormatCtx )
// 		avformat_close_input(&pFormatCtx);

	return 0;
}

int player_close(void* handle)
{
	playerdesc_t *inst = (playerdesc_t*)handle;
	
	delete inst;
	return 0;	
}
