/*
	Copyright (C) 2005-2006 Theo Berkau
	Copyright (C) 2006-2010 DeSmuME team

	This file is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.

	This file is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with the this software.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "SPU.h"
#include "sndpsp.h"
#include "debug.h"
#include <pspaudio.h>
#include <pspaudiolib.h>

#include"PSP/pl_snd.h"
#include"PSP/pspvfpu.h"
#include"PSP/pspDmac.h"

int SNDPSPInit(int buffersize);
void SNDPSPDeInit();
void SNDPSPUpdateAudio(s16 *buffer, u32 num_samples);
u32 SNDPSPGetAudioSpace();
void SNDPSPMuteAudio();
void SNDPSPUnMuteAudio();
void SNDPSPSetVolume(int volume);

#define AUDIO_CHANNELS  1
#define DEFAULT_SAMPLES 512
#define VOLUME_MAX      0x8000

static u16 *stereodata16;
static u16 *outputbuffer;
static u32 soundoffset;
static volatile u32 soundpos;
static u32 soundlen;
static u32 soundbufsize;
static u32 samplecount;

static int handle;
static int stopAudio;

SoundInterface_struct SNDPSP = {
SNDCORE_PSP,
"PSP Sound Interface",
SNDPSPInit,
SNDPSPDeInit,
SNDPSPUpdateAudio,
SNDPSPGetAudioSpace,
SNDPSPMuteAudio,
SNDPSPUnMuteAudio,
SNDPSPSetVolume
};

void MixAudio(void * stream, u32 len, void *userdata) {
    int i;
    u8* soundbuf = (u8*)stereodata16;
    u8* ubuf = (u8*) stream;

    for (i = 0; i < len; i++)
    {
        if (soundpos >= soundbufsize)
            soundpos = 0;
            
        u8 sample = soundbuf[soundpos];
        ubuf[i] = sample;
        soundpos++;
    }
}

/*void Sound_Thread(void* buf, unsigned int length, void *userdata)
{
   samplecount = PSP_AUDIO_SAMPLE_ALIGN(2048);

   int channel = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, samplecount, PSP_AUDIO_FORMAT_STEREO);
   
   while (!stopAudio)
   {
        MixAudio((uint8_t*)outputbuffer, 2048 * 4);
        sceAudioOutput(channel, VOLUME_MAX, outputbuffer);
      
   }

   sceAudioChRelease(handle);
   sceKernelExitThread(0);
}*/

int SNDPSPInit(int buffersize)
{
    const int freq = 44100;
    const int samples = (freq / 60) * 2;

    u32 normSamples = 512;
    while (normSamples < samples)
        normSamples <<= 1;

    soundlen = freq / 60; // 60 for NTSC
    soundbufsize = buffersize * sizeof(s16) * 2;
    soundpos = 0;

    stereodata16 = (u16 *)malloc(soundbufsize);
    if (stereodata16 == NULL)
        return -1;

    memset(stereodata16, 0, soundbufsize);

    pspAudioInit();

    pspAudioSetChannelCallback(0, MixAudio, NULL);

    return 0;
}

//////////////////////////////////////////////////////////////////////////////

void SNDPSPDeInit()
{
    stopAudio = 1;  
}

//////////////////////////////////////////////////////////////////////////////

void SNDPSPUpdateAudio(s16 *buffer, u32 num_samples)
{
    u32 copy1size = 0, copy2size = 0;

   if ((soundbufsize - soundoffset) < (num_samples * sizeof(s16) * 2))
   {
       copy1size = (soundbufsize - soundoffset);
       copy2size = (num_samples * sizeof(s16) * 2) - copy1size;
   }
   else
   {
       copy1size = (num_samples * sizeof(s16) * 2);
       copy2size = 0;
   }

   memcpy((((u8*)stereodata16) + soundoffset), buffer, copy1size);

   if (copy2size)
       memcpy(stereodata16, ((u8*)buffer) + copy1size, copy2size);

   soundoffset += copy1size + copy2size;
   soundoffset %= soundbufsize;
}

//////////////////////////////////////////////////////////////////////////////

u32 SNDPSPGetAudioSpace()
{
   u32 freespace= 0;

   if (soundoffset > soundpos)
      freespace = soundbufsize - soundoffset + soundpos;
   else
      freespace = soundpos - soundoffset;

   return  freespace / sizeof(s16) / 2;
}

//////////////////////////////////////////////////////////////////////////////

void SNDPSPMuteAudio()
{
    
}

//////////////////////////////////////////////////////////////////////////////

void SNDPSPUnMuteAudio()
{
    
}

//////////////////////////////////////////////////////////////////////////////

void SNDPSPSetVolume(int volume)
{
}

//////////////////////////////////////////////////////////////////////////////