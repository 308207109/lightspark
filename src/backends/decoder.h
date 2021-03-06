/**************************************************************************
    Lightspark, a free flash player implementation

    Copyright (C) 2010-2013  Alessandro Pignotti (a.pignotti@sssup.it)

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
**************************************************************************/

#ifndef BACKENDS_DECODER_H
#define BACKENDS_DECODER_H 1

#include "compat.h"
#include "threading.h"
#include "backends/graphics.h"
#ifdef ENABLE_LIBAVCODEC
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#ifdef HAVE_LIBAVRESAMPLE
#include <libavresample/avresample.h>
#endif
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#ifndef AVCODEC_MAX_AUDIO_FRAME_SIZE
#define AVCODEC_MAX_AUDIO_FRAME_SIZE 192000 // 1 second of 48khz 32bit audio
#endif
#ifdef HAVE_AVCODECID
#define CodecID AVCodecID
#define CODEC_ID_NONE AV_CODEC_ID_NONE
#define CODEC_ID_H264 AV_CODEC_ID_H264
#define CODEC_ID_FLV1 AV_CODEC_ID_FLV1
#define CODEC_ID_VP6F AV_CODEC_ID_VP6F
#define CODEC_ID_AAC AV_CODEC_ID_AAC
#define CODEC_ID_MP3 AV_CODEC_ID_MP3
#define CODEC_ID_ADPCM_SWF AV_CODEC_ID_ADPCM_SWF
#endif
#define MAX_AUDIO_FRAME_SIZE AVCODEC_MAX_AUDIO_FRAME_SIZE
}
#else
// Correct size? 192000?
// TODO: a real plugins system
#define MAX_AUDIO_FRAME_SIZE 20
#endif

namespace lightspark
{

enum LS_VIDEO_CODEC { H264=0, H263, VP6 };
// "Audio coding formats" from Chapter 11 in SWF documentation
enum LS_AUDIO_CODEC { CODEC_NONE=-1, LINEAR_PCM_PLATFORM_ENDIAN=0, ADPCM=1, MP3=2, LINEAR_PCM_LE=3, AAC=10 };

class AudioFormat
{
public:
	AudioFormat(LS_AUDIO_CODEC co, int sr, int ch):codec(co),sampleRate(sr),channels(ch) {}
	LS_AUDIO_CODEC codec;
	int sampleRate;
	int channels;
};

class Decoder
{
protected:
	Semaphore flushed;
	enum STATUS { PREINIT=0, INIT, VALID, FLUSHED};
	STATUS status;
	bool flushing;
public:
	Decoder():flushed(0),status(PREINIT),flushing(false){}
	virtual ~Decoder(){}
	bool isValid() const
	{
		return status>=VALID;
	}
	virtual void setFlushing()=0;
	void waitFlushed()
	{
		flushed.wait();
	}
};

class VideoDecoder: public Decoder, public ITextureUploadable
{
public:
	VideoDecoder():frameRate(0),framesdecoded(0),framesdropped(0),frameWidth(0),frameHeight(0),fenceCount(0),resizeGLBuffers(false){}
	virtual ~VideoDecoder(){}
	virtual void switchCodec(LS_VIDEO_CODEC codecId, uint8_t* initdata, uint32_t datalen, double frameRateHint)=0;
	virtual bool decodeData(uint8_t* data, uint32_t datalen, uint32_t time)=0;
	virtual bool discardFrame()=0;
	virtual void skipUntil(uint32_t time)=0;
	virtual void skipAll()=0;
	uint32_t getWidth()
	{
		return frameWidth;
	}
	uint32_t getHeight()
	{
		return frameHeight;
	}
	double frameRate;
	uint32_t framesdecoded;
	uint32_t framesdropped;
	/*
		Useful to avoid destruction of the object while a pending upload is waiting
	*/
	void waitForFencing();
	//ITextureUploadable interface
	void sizeNeeded(uint32_t& w, uint32_t& h) const;
	const TextureChunk& getTexture();
	void uploadFence();
protected:
	TextureChunk videoTexture;
	uint32_t frameWidth;
	uint32_t frameHeight;
	/*
		Derived classes must spinwaits on this to become false before deleting
	*/
	ATOMIC_INT32(fenceCount);
	bool setSize(uint32_t w, uint32_t h);
	bool resizeIfNeeded(TextureChunk& tex);
	LS_VIDEO_CODEC videoCodec;
private:
	bool resizeGLBuffers;
};

class NullVideoDecoder: public VideoDecoder
{
public:
	NullVideoDecoder() {status=VALID;}
	~NullVideoDecoder() { while(fenceCount); }
	void switchCodec(LS_VIDEO_CODEC codecId, uint8_t* initdata, uint32_t datalen, double frameRateHint){};
	bool decodeData(uint8_t* data, uint32_t datalen, uint32_t time){return false;}
	bool discardFrame(){return false;}
	void skipUntil(uint32_t time){}
	void skipAll(){}
	void setFlushing()
	{
		flushing=true;
	}
	//ITextureUploadable interface
	void upload(uint8_t* data, uint32_t w, uint32_t h) const
	{
	}
};

#ifdef ENABLE_LIBAVCODEC
class FFMpegVideoDecoder: public VideoDecoder
{
private:
	class YUVBuffer
	{
	YUVBuffer(const YUVBuffer&); /* no impl */
	YUVBuffer& operator=(const YUVBuffer&); /* no impl */
	public:
		uint8_t* ch[3];
		uint32_t time;
		YUVBuffer():time(0){ch[0]=NULL;ch[1]=NULL;ch[2]=NULL;}
		~YUVBuffer()
		{
			if(ch[0])
			{
				aligned_free(ch[0]);
				aligned_free(ch[1]);
				aligned_free(ch[2]);
			}
		}
	};
	class YUVBufferGenerator
	{
	private:
		uint32_t bufferSize;
	public:
		YUVBufferGenerator(uint32_t b):bufferSize(b){}
		void init(YUVBuffer& buf) const;
	};
	bool ownedContext;
	uint32_t curBuffer;
	AVCodecContext* codecContext;
	BlockingCircularQueue<YUVBuffer,80> buffers;
	Mutex mutex;
	AVFrame* frameIn;
	void copyFrameToBuffers(const AVFrame* frameIn, uint32_t time);
	void setSize(uint32_t w, uint32_t h);
	bool fillDataAndCheckValidity();
	uint32_t curBufferOffset;
public:
	FFMpegVideoDecoder(LS_VIDEO_CODEC codec, uint8_t* initdata, uint32_t datalen, double frameRateHint);
	/*
	   Specialized constructor used by FFMpegStreamDecoder
	*/
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(57, 40, 101)
	FFMpegVideoDecoder(AVCodecID codecID, double frameRateHint);
#else
	FFMpegVideoDecoder(AVCodecContext* codecContext, double frameRateHint);
#endif
	~FFMpegVideoDecoder();
	/*
	   Specialized decoding used by FFMpegStreamDecoder
	*/
	bool decodePacket(AVPacket* pkt, uint32_t time);
	void switchCodec(LS_VIDEO_CODEC codecId, uint8_t* initdata, uint32_t datalen, double frameRateHint);
	bool decodeData(uint8_t* data, uint32_t datalen, uint32_t time);
	bool discardFrame();
	void skipUntil(uint32_t time);
	void skipAll();
	void setFlushing()
	{
		flushing=true;
		if(buffers.isEmpty())
		{
			status=FLUSHED;
			flushed.signal();
		}
	}
	//ITextureUploadable interface
	void upload(uint8_t* data, uint32_t w, uint32_t h) const;
};
#endif

class AudioDecoder: public Decoder
{
protected:
	class FrameSamples
	{
	public:
		int16_t samples[MAX_AUDIO_FRAME_SIZE/2];
		__attribute__ ((aligned (16)))
		int16_t* current;
		uint32_t len;
		uint32_t time;
		FrameSamples():current(samples),len(0),time(0){}
	};
	class FrameSamplesGenerator
	{
	public:
		void init(FrameSamples& f) const {f.len=0;}
	};
public:
	uint32_t sampleRate;
protected:
	BlockingCircularQueue<FrameSamples,150> samplesBuffer;
public:
	/**
	  	The AudioDecoder contains audio buffers that must be aligned to 16 bytes, so we redefine the allocator
	*/
	void* operator new(size_t);
	void operator delete(void*);
	AudioDecoder():sampleRate(0),channelCount(0),initialTime(-1){}
	virtual ~AudioDecoder(){}
	virtual void switchCodec(LS_AUDIO_CODEC codecId, uint8_t* initdata, uint32_t datalen)=0;
	virtual uint32_t decodeData(uint8_t* data, int32_t datalen, uint32_t time)=0;
	bool hasDecodedFrames() const
	{
		return !samplesBuffer.isEmpty();
	}
	uint32_t getFrontTime() const;
	uint32_t getBytesPerMSec() const
	{
		return sampleRate*channelCount*2/1000;
	}
	uint32_t copyFrame(int16_t* dest, uint32_t len) DLL_PUBLIC;
	/**
	  	Skip samples until the given time

		@param time the desired time in millisecond
		@param a fractional time in microseconds
	*/
	void skipUntil(uint32_t time, uint32_t usecs);
	/**
	  	Skip all the samples
	*/
	void skipAll() DLL_PUBLIC;
	bool discardFrame();
	void setFlushing()
	{
		flushing=true;
		if(samplesBuffer.isEmpty())
		{
			status=FLUSHED;
			flushed.signal();
		}
	}
	uint32_t channelCount;
	//Saves the timestamp of the first decoded frame
	uint32_t initialTime;
};

class NullAudioDecoder: public AudioDecoder
{
public:
	NullAudioDecoder()
	{
		status=VALID;
		sampleRate=44100;
		channelCount=2;
	}
	void switchCodec(LS_AUDIO_CODEC codecId, uint8_t* initdata, uint32_t datalen){};
	uint32_t decodeData(uint8_t* data, int32_t datalen, uint32_t time){return 0;}
};

#ifdef ENABLE_LIBAVCODEC
class EngineData;
class FFMpegAudioDecoder: public AudioDecoder
{
private:
	EngineData* engine;
	bool ownedContext;
	AVCodecContext* codecContext;
#ifdef HAVE_LIBAVRESAMPLE
	AVAudioResampleContext* resamplecontext;
#endif
	std::vector<uint8_t> overflowBuffer;
	bool fillDataAndCheckValidity();
	CodecID LSToFFMpegCodec(LS_AUDIO_CODEC lscodec);
#ifdef HAVE_AVCODEC_DECODE_AUDIO4
	AVFrame* frameIn;
	int resampleFrameToS16(FrameSamples& curTail);
#endif
public:
	FFMpegAudioDecoder(EngineData* eng,LS_AUDIO_CODEC codec, uint8_t* initdata, uint32_t datalen);
	FFMpegAudioDecoder(EngineData* eng,LS_AUDIO_CODEC codec, int sampleRate, int channels, bool);
	/*
	   Specialized constructor used by FFMpegStreamDecoder
	*/
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(57, 40, 101)
	FFMpegAudioDecoder(EngineData* eng,AVCodecID codecID);
#else
	FFMpegAudioDecoder(EngineData* eng,AVCodecContext* codecContext);
#endif
	~FFMpegAudioDecoder();
	/*
	   Specialized decoding used by FFMpegStreamDecoder
	*/
	uint32_t decodePacket(AVPacket* pkt, uint32_t time);
	void switchCodec(LS_AUDIO_CODEC audioCodec, uint8_t* initdata, uint32_t datalen);
	uint32_t decodeData(uint8_t* data, int32_t datalen, uint32_t time);
};
#endif

class StreamDecoder
{
public:
	StreamDecoder():audioDecoder(NULL),videoDecoder(NULL),valid(false),hasvideo(false){}
	virtual ~StreamDecoder();
	virtual bool decodeNextFrame() = 0;
	bool isValid() const { return valid; }
	AudioDecoder* audioDecoder;
	VideoDecoder* videoDecoder;
	bool hasVideo() const { return hasvideo; }
protected:
	bool valid;
	bool hasvideo;
};

#ifdef ENABLE_LIBAVCODEC
class FFMpegStreamDecoder: public StreamDecoder
{
private:
	bool audioFound;
	bool videoFound;
	std::istream& stream;
	AVFormatContext* formatCtx;
	int32_t audioIndex;
	int32_t videoIndex;
	//We use our own copy of these to have access of the ffmpeg specific methods
	FFMpegAudioDecoder* customAudioDecoder;
	FFMpegVideoDecoder* customVideoDecoder;
	//Helpers for custom I/O of libavformat
	uint8_t* avioBuffer;
	static int avioReadPacket(void* t, uint8_t* buf, int buf_size);
	//NOTE: this will become AVIOContext in FFMpeg 0.7
#if LIBAVUTIL_VERSION_MAJOR < 51
	ByteIOContext* avioContext;
#else
	AVIOContext* avioContext;
#endif
	int availablestreamlength;
public:
	FFMpegStreamDecoder(EngineData* eng,std::istream& s, AudioFormat* format = NULL, int streamsize = -1);
	~FFMpegStreamDecoder();
	bool decodeNextFrame();
};
#endif

};
#endif /* BACKENDS_DECODER */
