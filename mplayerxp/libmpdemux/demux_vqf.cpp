#include "mpxp_config.h"
#include "osdep/mplib.h"
using namespace	usr;
#include <algorithm>

#include <stdlib.h>
#include <stdio.h>

#include "osdep/bswap.h"
#include "libmpstream2/stream.h"
#include "demuxer.h"
#include "demuxer_internal.h"
#include "stheader.h"
#include "libmpconf/cfgparser.h"
#include "libmpcodecs/dec_audio.h"
#include "libao3/afmt.h"
#include "demux_msg.h"

static const int KEYWORD_BYTES=4;
static const int VERSION_BYTES=8;
static const int ELEM_BYTES=sizeof(unsigned long);
typedef struct{
	char		ID[KEYWORD_BYTES+VERSION_BYTES+1];
	int size;
	/* Common Chunk */
	int channelMode;   /* channel mode (mono:0/stereo:1) */
	int bitRate;       /* bit rate (kbit/s) */
	int samplingRate;  /* sampling rate (44.1 kHz -> 44) */
	int securityLevel; /* security level (always 0) */
	/* Text Chunk */
	char	Name[BUFSIZ];
	char	Comt[BUFSIZ];
	char	Auth[BUFSIZ];
	char	Cpyr[BUFSIZ];
	char	File[BUFSIZ];
	char	Extr[BUFSIZ];  // add by OKAMOTO 99.12.21
	/* Data size chunk*/
	int		Dsiz;
} headerInfo;


static MPXP_Rc vqf_probe(Demuxer* demuxer)
{
    Stream *s;
    s = demuxer->stream;
    binary_packet bp=s->read(12);
    if(memcmp(bp.data(),"TWIN",4)==0) return MPXP_Ok; /*version: 97012000*/
    return MPXP_False;
}

static Opaque* vqf_open(Demuxer* demuxer) {
  sh_audio_t* sh_audio;
  WAVEFORMATEX* w;
  Stream *s;
  headerInfo *hi;

  s = demuxer->stream;

  sh_audio = demuxer->new_sh_audio();
  sh_audio->wf = w = (WAVEFORMATEX*)mp_mallocz(sizeof(WAVEFORMATEX)+sizeof(headerInfo));
  hi = (headerInfo *)&w[1];
  w->wFormatTag = 0x1; sh_audio->wtag = mmioFOURCC('T','W','I','N'); /* TWinVQ */
  w->nChannels = sh_audio->nch = 2;
  w->nSamplesPerSec = sh_audio->rate = 44100;
  w->nAvgBytesPerSec = w->nSamplesPerSec*sh_audio->nch*2;
  w->nBlockAlign = 0;
  sh_audio->afmt = bps2afmt(2);
  w->wBitsPerSample = 8*afmt2bps(sh_audio->afmt);
  w->cbSize = 0;
  s->reset();
  s->seek(0);
  binary_packet bp=s->read(12); memcpy(hi->ID,bp.data(),bp.size()); /* fourcc+version_id */
  while(1)
  {
    char chunk_id[4];
    unsigned chunk_size;
    hi->size=chunk_size=s->read(type_dword); /* include itself */
    bp=s->read(4); memcpy(chunk_id,bp.data(),bp.size());
    if(*((uint32_t *)&chunk_id[0])==mmioFOURCC('C','O','M','M'))
    {
	char buf[chunk_size-8];
	unsigned i,subchunk_size;
	bp=s->read(chunk_size-8); memcpy(buf,bp.data(),bp.size());
	if(bp.size()!=chunk_size-8) return NULL;
	i=0;
	subchunk_size=be2me_32(*((uint32_t *)&buf[0]));
	hi->channelMode=be2me_32(*((uint32_t *)&buf[4]));
	w->nChannels=sh_audio->nch=hi->channelMode+1; /*0-mono;1-stereo*/
	hi->bitRate=be2me_32(*((uint32_t *)&buf[8]));
	sh_audio->i_bps=hi->bitRate*1000/8; /* bitrate kbit/s */
	w->nAvgBytesPerSec = sh_audio->i_bps;
	hi->samplingRate=be2me_32(*((uint32_t *)&buf[12]));
	switch(hi->samplingRate){
	case 44:
		w->nSamplesPerSec=44100;
		break;
	case 22:
		w->nSamplesPerSec=22050;
		break;
	case 11:
		w->nSamplesPerSec=11025;
		break;
	default:
		w->nSamplesPerSec=hi->samplingRate*1000;
		break;
	}
	sh_audio->rate=w->nSamplesPerSec;
	hi->securityLevel=be2me_32(*((uint32_t *)&buf[16]));
	w->nBlockAlign = 0;
	sh_audio->afmt = bps2afmt(4);
	w->wBitsPerSample = 8*afmt2bps(sh_audio->afmt);
	w->cbSize = 0;
	i+=subchunk_size+4;
	while(i<chunk_size-8)
	{
	    unsigned slen,sid;
	    char sdata[chunk_size];
	    sid=*((uint32_t *)&buf[i]); i+=4;
	    slen=be2me_32(*((uint32_t *)&buf[i])); i+=4;
	    if(sid==mmioFOURCC('D','S','I','Z'))
	    {
		hi->Dsiz=be2me_32(*((uint32_t *)&buf[i]));
		continue; /* describes the same info as size of DATA chunk */
	    }
	    memcpy(sdata,&buf[i],slen); sdata[slen]=0; i+=slen;
	    if(sid==mmioFOURCC('N','A','M','E'))
	    {
		memcpy(hi->Name,sdata,std::min(unsigned(BUFSIZ),slen));
		demuxer->info().add(INFOT_NAME,sdata);
	    }
	    else
	    if(sid==mmioFOURCC('A','U','T','H'))
	    {
		memcpy(hi->Auth,sdata,std::min(unsigned(BUFSIZ),slen));
		demuxer->info().add(INFOT_AUTHOR,sdata);
	    }
	    else
	    if(sid==mmioFOURCC('C','O','M','T'))
	    {
		memcpy(hi->Comt,sdata,std::min(unsigned(BUFSIZ),slen));
		demuxer->info().add(INFOT_COMMENTS,sdata);
	    }
	    else
	    if(sid==mmioFOURCC('(','c',')',' '))
	    {
		memcpy(hi->Cpyr,sdata,std::min(unsigned(BUFSIZ),slen));
		demuxer->info().add(INFOT_COPYRIGHT,sdata);
	    }
	    else
	    if(sid==mmioFOURCC('F','I','L','E'))
	    {
		memcpy(hi->File,sdata,std::min(unsigned(BUFSIZ),slen));
	    }
	    else
	    if(sid==mmioFOURCC('A','L','B','M')) demuxer->info().add(INFOT_ALBUM,sdata);
	    else
	    if(sid==mmioFOURCC('Y','E','A','R')) demuxer->info().add(INFOT_DATE,sdata);
	    else
	    if(sid==mmioFOURCC('T','R','A','C')) demuxer->info().add(INFOT_TRACK,sdata);
	    else
	    if(sid==mmioFOURCC('E','N','C','D')) demuxer->info().add(INFOT_ENCODER,sdata);
	    else
	    MSG_V("Unhandled subchunk '%c%c%c%c'='%s'\n",((char *)&sid)[0],((char *)&sid)[1],((char *)&sid)[2],((char *)&sid)[3],sdata);
	    /* other stuff is unrecognized due untranslatable japan's idiomatics */
	}
    }
    else
    if(*((uint32_t *)&chunk_id[0])==mmioFOURCC('D','A','T','A'))
    {
	demuxer->movi_start=s->tell();
	demuxer->movi_end=demuxer->movi_start+chunk_size-8;
	MSG_V("Found data at %llX size %llu\n",demuxer->movi_start,demuxer->movi_end);
	/* Done! play it */
	break;
    }
    else
    {
	MSG_V("Unhandled chunk '%c%c%c%c' %lu bytes\n",((char *)&chunk_id)[0],((char *)&chunk_id)[1],((char *)&chunk_id)[2],((char *)&chunk_id)[3],chunk_size);
	s->skip(chunk_size-8); /*unknown chunk type */
    }
  }

  demuxer->movi_length = (demuxer->movi_end-demuxer->movi_start)/w->nAvgBytesPerSec;
  demuxer->audio->sh = sh_audio;
  sh_audio->ds = demuxer->audio;
  s->seek(demuxer->movi_start);
    check_pin("demuxer",demuxer->pin,DEMUX_PIN);
    return demuxer;
}

static int vqf_demux(Demuxer* demuxer, Demuxer_Stream *ds) {
  sh_audio_t* sh_audio = reinterpret_cast<sh_audio_t*>(demuxer->audio->sh);
  int l = sh_audio->wf->nAvgBytesPerSec;
  off_t spos = demuxer->stream->tell();
  Demuxer_Packet*  dp;

  if(demuxer->stream->eof())
    return 0;

  dp = new(zeromem) Demuxer_Packet(l);
  dp->pts = spos / (float)(sh_audio->wf->nAvgBytesPerSec);
  dp->pos = spos;
  dp->flags = DP_NONKEYFRAME;

  binary_packet bp=demuxer->stream->read(l); memcpy(dp->buffer(),bp.data(),bp.size());
  l=bp.size();
  dp->resize(l);
  ds->add_packet(dp);

  return 1;
}

static void vqf_seek(Demuxer *demuxer,const seek_args_t* seeka){
  Stream* s = demuxer->stream;
  sh_audio_t* sh_audio = reinterpret_cast<sh_audio_t*>(demuxer->audio->sh);
  off_t base,pos;

  base = (seeka->flags&DEMUX_SEEK_SET) ? demuxer->movi_start : s->tell();
  pos=base+(seeka->flags&DEMUX_SEEK_PERCENTS?demuxer->movi_end-demuxer->movi_start:sh_audio->i_bps)*seeka->secs;
  pos -= (pos % (sh_audio->nch * afmt2bps(sh_audio->afmt)));
  s->seek(pos);
}

static void vqf_close(Demuxer* demuxer) { UNUSED(demuxer); }

static MPXP_Rc vqf_control(const Demuxer *demuxer,int cmd,any_t*args)
{
    UNUSED(demuxer);
    UNUSED(cmd);
    UNUSED(args);
    return MPXP_Unknown;
}

extern const demuxer_driver_t demux_vqf =
{
    "vqf",
    "TwinVQ - Transform-domain Weighted Interleave Vector Quantization",
    ".vqf",
    NULL,
    vqf_probe,
    vqf_open,
    vqf_demux,
    vqf_seek,
    vqf_close,
    vqf_control
};
