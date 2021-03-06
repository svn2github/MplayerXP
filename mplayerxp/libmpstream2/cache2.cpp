#include "mpxp_config.h"
#include "osdep/mplib.h"
using namespace	usr;
#include <algorithm>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <pthread.h>

#include "stream.h"
#include "stream_internal.h"
#include "xmpcore/sig_hand.h"
#include "osdep/timer.h"
#include "osdep/cpudetect.h"
#include "osdep/bswap.h"
#include "osdep/fastmemcpy.h"
#include "mpxp_help.h"
#include "libmpdemux/mpdemux.h"
#include "mplayerxp.h"
#include "stream_msg.h"

namespace	usr {
static const int READ_USLEEP_TIME=10000;
static const int FILL_USLEEP_TIME=50000;
static const int PREFILL_SLEEP_TIME=200;
static const int CPF_EMPTY=0x00000001UL;
static const int CPF_EOF=0x80000000UL;
static const int CPF_DONE=0x40000000UL; /* special case for dvd packets to exclude them from sending again */
struct cache_packet_t
{
    off_t filepos;  /* some nav-packets have length so we need to know real pos of data packet */
    unsigned state; /* consists from of CPF_* */
    stream_packet_t sp;
    pthread_mutex_t cp_mutex;
};

struct cache_vars_t {
    public:
	unsigned	first;	/* index of the first packet */
	unsigned	last;	/* index of the last packet */
	unsigned	buffer_size; /* size of the allocated buffer memory (for statistic only) */
	unsigned	sector_size; /* size of a single sector (2048/2324) */
	unsigned	npackets;	/* number of packets in cache */
	int		back_size;	/* for backward seek */
	int		prefill;		/* min prefill bytes if cache is empty. TODO: remove */
	int		eof;
	/* reader's pointers: */
	off_t read_filepos;
	Cached_Stream* stream; /* parent stream */
	/* thread related stuff */
	int in_fill;
	pthread_mutex_t mutex;
	mpxp_thread_t*  pth;
	/* for optimization: */
	cache_packet_t *packets;
	char* mem;
};

inline void CACHE2_LOCK(cache_vars_t& cv) { pthread_mutex_lock(&cv.mutex); }
inline void CACHE2_UNLOCK(cache_vars_t& cv) { pthread_mutex_unlock(&cv.mutex); }

inline void CACHE2_TLOCK(cache_vars_t& cv) { pthread_mutex_lock(&cv.mutex); }
inline void CACHE2_TUNLOCK(cache_vars_t& cv) { pthread_mutex_unlock(&cv.mutex); }

inline void CACHE2_PACKET_LOCK(cache_packet_t& c) { pthread_mutex_lock(&c.cp_mutex); }
inline void CACHE2_PACKET_UNLOCK(cache_packet_t& c) { pthread_mutex_unlock(&c.cp_mutex); }

inline void CACHE2_PACKET_TLOCK(cache_packet_t& c) { pthread_mutex_lock(&c.cp_mutex); }
inline void CACHE2_PACKET_TUNLOCK(cache_packet_t& c) { pthread_mutex_unlock(&c.cp_mutex); }

inline off_t START_FILEPOS(cache_vars_t& c) { return c.packets[c.first].filepos; }
inline off_t END_FILEPOS(cache_vars_t& c) { return c.packets[c.last].filepos+c.packets[c.last].sp.len; }
inline unsigned CP_NEXT(cache_vars_t& c,unsigned idx) { return (idx+1)%c.npackets; }

#ifdef __i386__
inline void COREDUMP() { __asm __volatile(".short 0xffff":::"memory"); }
#else
inline void COREDUMP() {}
#endif

inline void C2_ASSERT(int cond) { if(cond) mpxp_fatal<<"internal error at cache2.c: "<<cond<<std::endl; }

static int __FASTCALL__ c2_cache_fill(cache_vars_t* c){
  int len,in_cache,legacy_eof,seek_eof;
  off_t readpos,new_start;
  unsigned cidx,cp;

  CACHE2_TLOCK(*c);
  readpos=c->read_filepos;
  in_cache=(readpos>=START_FILEPOS(*c)&&readpos<END_FILEPOS(*c));
  new_start = readpos - c->back_size;
  if(new_start<c->stream->start_pos()) new_start=c->stream->start_pos();
  seek_eof=0;
  if(!in_cache && c->stream->type()&Stream::Type_Seekable) {
	/* seeking... */
	mpxp_dbg2<<"Out of boundaries... seeking to "<<new_start
	    <<" {in_cache("<<in_cache<<") "
	    <<START_FILEPOS(*c)<<"<"<<readpos<<">"<<END_FILEPOS(*c)<<"}"<<std::endl;
	if(c->stream->eof() || c->eof) c->stream->reset();
	c->stream->seek(new_start);
	if(errno) { mpxp_warn<<"c2_seek(drv:"<<c->stream->driver_info->mrl<<") error: "<<strerror(errno)<<std::endl; errno=0; }
	if((c->packets[c->first].filepos=c->stream->tell())<0) seek_eof=1;
	c->last=c->first;
	if(c->packets[c->first].filepos < new_start-(off_t)c->stream->sector_size())
	    mpxp_warn<<"CACHE2: found wrong offset after seeking "<<c->packets[c->first].filepos<<" (wanted: "<<new_start<<")"<<std::endl;
	mpxp_dbg2<<"Seek done. new pos: "<<START_FILEPOS(*c)<<std::endl;
  } else {
    /* find new start of buffer according on readpos */
    cidx=c->first;
    do {
	if((new_start>=c->packets[cidx].filepos&&new_start<c->packets[cidx].filepos+c->packets[cidx].sp.len)
	   && !c->packets[cidx].sp.type) break;
	cidx=CP_NEXT(*c,cidx);
    }while(cidx!=c->first);
    mpxp_dbg2<<"CACHE2: Assigning first as "<<reinterpret_cast<any_t*>(c->first)<<" for "<<START_FILEPOS(*c)<<std::endl;
    c->first=cidx;
  }
  CACHE2_TUNLOCK(*c);
  if(CP_NEXT(*c,c->last) == c->first || c->eof) {
    mpxp_dbg2<<"CACHE2: cache full"<<std::endl;
    return 0; /* cache full */
  }
  len=0;
  cp=cidx=c->last==c->first?c->first:CP_NEXT(*c,c->last);
  do { CACHE2_PACKET_TLOCK(c->packets[cidx]); c->packets[cidx].state|=CPF_EMPTY; CACHE2_PACKET_TUNLOCK(c->packets[cidx]); cidx=CP_NEXT(*c,cidx); } while(cidx!=c->first);
  cidx=cp;
  c->in_fill=1;
  while(1)
  {
    CACHE2_PACKET_TLOCK(c->packets[cidx]);
    c->packets[cidx].sp.len=c->sector_size;
    c->packets[cidx].filepos = c->stream->tell();
    binary_packet bp=c->stream->read(c->packets[cidx].sp.len);
    memcpy(c->packets[cidx].sp.buf,bp.data(),bp.size());
    mpxp_dbg2<<"CACHE2: read_packet at "<<c->packets[cidx].filepos<<" (wanted "<<c->sector_size<<" got "<<c->packets[cidx].sp.len<<" type "<<c->packets[cidx].sp.type<<")";
    if(mp_conf.verbose>1)
	if(c->packets[cidx].sp.len>8) {
	    int i;
	    for(i=0;i<8;i++)
		mpxp_dbg2<<std::hex<<(unsigned)(unsigned char)c->packets[cidx].sp.buf[i]<<" ";
	}
    mpxp_dbg2<<std::endl;
    if(c->stream->ctrl(SCTRL_EOF,NULL)==MPXP_Ok) legacy_eof=1;
    else	legacy_eof=0;
    if(c->packets[cidx].sp.len < 0 || (c->packets[cidx].sp.len == 0 && c->packets[cidx].sp.type == 0) || legacy_eof || seek_eof) {
	/* EOF */
	mpxp_dbg2<<"CACHE2: guess EOF: "<<START_FILEPOS(*c)<<" "<<END_FILEPOS(*c)<<std::endl;
	c->packets[cidx].state|=CPF_EOF;
	c->eof=1;
	c->packets[cidx].state&=~CPF_EMPTY;
	c->packets[cidx].state&=~CPF_DONE;
	if(errno) { mpxp_warn<<"c2_fill_buffer(drv:"<<c->stream->driver_info->mrl<<") error: "<<strerror(errno)<<std::endl; errno=0; }
	CACHE2_PACKET_TUNLOCK(c->packets[cidx]);
	break;
    }
    if(c->packets[cidx].sp.type == 0) len += c->packets[cidx].sp.len;
    c->last=cidx;
    c->packets[cidx].state&=~CPF_EMPTY;
    c->packets[cidx].state&=~CPF_DONE;
    CACHE2_PACKET_TUNLOCK(c->packets[cidx]);
    cidx=CP_NEXT(*c,cidx);
    mpxp_dbg2<<"CACHE2: start="<<START_FILEPOS(*c)<<" end_filepos = "<<END_FILEPOS(*c)<<std::endl;
    if(cidx==c->first) {
	mpxp_dbg2<<"CACHE2: end of queue is reached: "<<reinterpret_cast<any_t*>(c->first)<<std::endl;
	break;
    }
    CACHE2_TUNLOCK(*c);
  }
  c->in_fill=0;
  mpxp_dbg2<<"CACHE2: totally got "<<len<<" bytes"<<std::endl;
  return len;
}

static cache_vars_t* __FASTCALL__  c2_cache_init(int size,int sector){
  pthread_mutex_t tmpl=PTHREAD_MUTEX_INITIALIZER;
  cache_vars_t* c=new(zeromem) cache_vars_t;
  char *pmem;
  unsigned i,num;
  c->npackets=num=size/sector;
  /* collection of all c2_packets in continuous memory area minimizes cache pollution
     and speedups cache as C+D=3.27% instead of 4.77% */
  c->packets=new(zeromem) cache_packet_t[num];
  c->mem=new char [num*sector];
  if(!c->packets || !c->mem)
  {
    mpxp_err<<MSGTR_OutOfMemory<<std::endl;
    delete c;
    return 0;
  }
  pmem = c->mem;
  mpxp_dbg2<<"For cache navigation was allocated "<<i<<" bytes as "<<num<<" packets ("<<size<<"/"<<sector<<")"<<std::endl;
  c->first=c->last=0;
  for(i=0;i<num;i++)
  {
    c->packets[i].sp.buf=pmem;
    c->packets[i].state|=CPF_EMPTY;
    memcpy(&c->packets[i].cp_mutex,&tmpl,sizeof(tmpl));
    pmem += sector;
  }
  if(mp_conf.verbose>1)
  for(i=0;i<num;i++) {
    mpxp_dbg2<<"sizeof(c)="<<sizeof(cache_packet_t)<<" c="<<i<<" c->sp.buf="<<reinterpret_cast<any_t*>(c->packets[i].sp.buf)<<std::endl;
  }
  c->buffer_size=num*sector;
  c->sector_size=sector;
  c->back_size=size/4; /* default 25% */
  c->prefill=size/20;
  memcpy(&c->mutex,&tmpl,sizeof(tmpl));
  return c;
}

static void sig_cache2( void )
{
    mpxp_v<<"cache2 segfault"<<std::endl;
    mpxp_print_flush();
    xmp_killall_threads(pthread_self());
    __exit_sighandler();
}

static int was_killed=0;
static void stream_unlink_cache(int force)
{
  if(force) was_killed=1;
}

static any_t*cache2_routine(any_t*arg)
{
    mpxp_thread_t* priv=reinterpret_cast<mpxp_thread_t*>(arg);

    double tt;
    unsigned int t=0;
    unsigned int t2;
    int cfill;
    cache_vars_t* c=(cache_vars_t*)arg;

    priv->state=Pth_Run;
    priv->pid = getpid();

    while(1) {
	if(mp_conf.benchmark) t=GetTimer();
	cfill=c2_cache_fill(c);
	if(mp_conf.benchmark) {
	    t2=GetTimer();t=t2-t;
	    tt = t*0.000001f;
	    mpxp_context().bench->c2+=tt;
	    if(tt > mpxp_context().bench->max_c2) mpxp_context().bench->max_c2=tt;
	    if(tt < mpxp_context().bench->min_c2) mpxp_context().bench->min_c2=tt;
	}
	if(!cfill) usleep(FILL_USLEEP_TIME); // idle
	if(priv->state==Pth_Canceling) break;
    }
    priv->state=Pth_Stand;
    return arg;
}

static int __FASTCALL__ c2_stream_fill_buffer(cache_vars_t* c)
{
  mpxp_dbg2<<"c2_stream_fill_buffer"<<std::endl;
  if(c->eof) return 0;
  while(c->read_filepos>=END_FILEPOS(*c) || c->read_filepos<START_FILEPOS(*c))
  {
	if(c->eof) break;
	usleep(READ_USLEEP_TIME); // 10ms
	mpxp_dbg2<<"Waiting for "<<c->read_filepos<<" in "<<START_FILEPOS(*c)<<" "<<END_FILEPOS(*c)<<std::endl;
	continue; // try again...
  }
  return c->eof?0:1;
}

static void __FASTCALL__ c2_stream_reset(cache_vars_t* c)
{
    unsigned cidx;
    int was_eof;
    mpxp_dbg2<<"c2_stream_reset"<<std::endl;
    c->stream->reset();
    cidx=c->first;
    was_eof=0;
    do{ was_eof |= (c->packets[cidx].state&CPF_EOF); c->packets[cidx].state&=~CPF_EOF; cidx=CP_NEXT(*c,cidx); }while(cidx!=c->first);
    c->eof=0;
    if(was_eof)
    {
	cidx=c->first;
	do{ c->packets[cidx].state|=CPF_EMPTY; cidx=CP_NEXT(*c,cidx); }while(cidx!=c->first);
	c->last=c->first;
	c->read_filepos=c->stream->start_pos();
	c->stream->seek(c->read_filepos);
    }
}

static int __FASTCALL__ c2_stream_seek_long(cache_vars_t* c,off_t pos){

  mpxp_dbg2<<"CACHE2_SEEK: "<<START_FILEPOS(*c)<<","<<c->read_filepos<<","<<END_FILEPOS(*c)<<" <> "<<pos<<std::endl;
  if(pos<0/* || pos>END_FILEPOS(*c)*/) { c->eof=1; return 0; }
  while(c->in_fill) yield_timeslice();
  CACHE2_LOCK(*c);
  if(c->eof) c2_stream_reset(c);
  C2_ASSERT(pos < c->stream->start_pos());
  c->read_filepos=pos;
  CACHE2_UNLOCK(*c);
  c2_stream_fill_buffer(c);
  return c->eof?pos<END_FILEPOS(*c)?1:0:1;
}

static unsigned __FASTCALL__ c2_find_packet(cache_vars_t* c,off_t pos)
{
    unsigned retval;
    CACHE2_LOCK(*c);
    retval = c->first;
    CACHE2_UNLOCK(*c);
    while(1)
    {
	CACHE2_PACKET_LOCK(c->packets[retval]);
	while(c->packets[retval].state&CPF_EMPTY) { CACHE2_PACKET_UNLOCK(c->packets[retval]); usleep(0); CACHE2_PACKET_LOCK(c->packets[retval]); }
	if((pos >= c->packets[retval].filepos &&
	    pos < c->packets[retval].filepos+c->packets[retval].sp.len &&
	    !c->packets[retval].sp.type) ||
	    (c->packets[retval].state&CPF_EOF))
			break; /* packet is locked */
	CACHE2_PACKET_UNLOCK(c->packets[retval]);
	CACHE2_LOCK(*c);
	retval=CP_NEXT(*c,retval);
	if(retval==c->first)
	{
	    mpxp_dbg2<<"Can't find packet for offset "<<pos<<std::endl;
	    CACHE2_UNLOCK(*c);
	    return UINT_MAX;
	}
	CACHE2_UNLOCK(*c);
    }
    return retval;
}

/* try return maximal continious memory to copy (speedups C+D on 30%)*/
static void __FASTCALL__ c2_get_continious_mem(cache_vars_t* c,unsigned cidx,int *len,unsigned *npackets)
{
    *npackets=1;
    *len=c->packets[cidx].sp.len;
    if(cidx != UINT_MAX)
    {
	unsigned i;
	int req_len;
	req_len = *len;
	for(i=cidx+1;i<c->npackets;i++)
	{
	    if(	*len >= req_len ||
		c->packets[i].sp.type ||
		c->packets[i].sp.len<0 ||
		c->packets[i].state&CPF_EMPTY ||
		(c->packets[i].sp.len==0 && c->packets[i].sp.type)) break;
	    CACHE2_PACKET_LOCK(c->packets[i]);
	    *len += c->packets[i].sp.len;
	    (*npackets)++;
	}
    }
}

static unsigned __FASTCALL__ c2_wait_packet(cache_vars_t* c,off_t pos,int *len,unsigned *npackets)
{
  unsigned cidx;
  while(1)
  {
    cidx = c2_find_packet(c,pos);
    if(cidx!=UINT_MAX || c->eof) break;
    if(cidx!=UINT_MAX) CACHE2_PACKET_UNLOCK(c->packets[cidx]);
    c2_stream_fill_buffer(c);
  }
  c2_get_continious_mem(c,cidx,len,npackets);
  return cidx;
}

static unsigned c2_next_packet(cache_vars_t* c,unsigned cidx,int *len,unsigned *npackets)
{
    mpxp_dbg2<<"next_packet: start="<<reinterpret_cast<any_t*>(c->first)<<" cur="<<cidx<<std::endl;
    while(1)
    {
	CACHE2_LOCK(*c);
	cidx=CP_NEXT(*c,cidx);
	CACHE2_UNLOCK(*c);
	CACHE2_PACKET_LOCK(c->packets[cidx]);
	while(c->packets[cidx].state&CPF_EMPTY) { CACHE2_PACKET_UNLOCK(c->packets[cidx]); usleep(0); CACHE2_PACKET_LOCK(c->packets[cidx]); }
	if(cidx==c->first)
	{
	    CACHE2_PACKET_UNLOCK(c->packets[cidx]);
	    c2_stream_fill_buffer(c);
	    cidx = c2_find_packet(c,c->read_filepos);
	    break;
	}
	if(!c->packets[cidx].sp.type) break; /* packet is locked */
	CACHE2_PACKET_UNLOCK(c->packets[cidx]);
    }
    c2_get_continious_mem(c,cidx,len,npackets);
    mpxp_dbg2<<"next_packet: rp: "<<c->read_filepos<<" fp: "<<c->packets[cidx].filepos<<" len "<<c->packets[cidx].sp.len<<" type "<<c->packets[cidx].sp.type<<std::endl;
    return cidx;
}

static binary_packet __FASTCALL__ c2_stream_read(cache_vars_t* c,size_t total){
    binary_packet rc(total);
    int len=total,eof,mlen;
    uint8_t *mem=rc.cdata();
    unsigned buf_pos;
    unsigned cur,i,npackets;

    cur=c2_wait_packet(c,c->read_filepos,&mlen,&npackets);
    eof = cur!=UINT_MAX?((int)(c->packets[cur].state&CPF_EOF)):c->eof;
    if(cur==UINT_MAX||eof) { if(cur!=UINT_MAX) CACHE2_PACKET_UNLOCK(c->packets[cur]); return 0; }
    mpxp_dbg2<<"c2_stream_read "<<total<<" bytes from "<<c->read_filepos<<std::endl;
    while(len){
	int x;
	if(c->read_filepos>=c->packets[cur].filepos+mlen){
	    for(i=0;i<npackets;i++) CACHE2_PACKET_UNLOCK(c->packets[cur+i]);
	    mlen=len;
	    cur=c2_next_packet(c,cur,&mlen,&npackets);
	    eof = cur!=UINT_MAX?(c->packets[cur].state&CPF_EOF):1;
	    if(eof) {
		CACHE2_PACKET_UNLOCK(c->packets[cur]);
		rc.resize(total-len);
		return rc; // EOF
	    }
	}
	buf_pos=c->read_filepos-c->packets[cur].filepos;
	x=mlen-buf_pos;
	C2_ASSERT(buf_pos>=(unsigned)mlen);
	if(x>len) x=len;
	if(!x) mpxp_warn<<"c2_read: dead-lock"<<std::endl;
	memcpy(mem,&c->packets[cur].sp.buf[buf_pos],x);
	buf_pos+=x;
	mem+=x; len-=x;
	c->read_filepos+=x;
    }
    CACHE2_PACKET_UNLOCK(c->packets[cur]);
    if(mp_conf.verbose>2) {
	mpxp_dbg2<<"c2_stream_read  got "<<total<<" bytes ";
	for(i=0;i<std::min(size_t(8),total);i++) mpxp_dbg2<<std::hex<<(unsigned)((unsigned char)mem[i]);
	mpxp_dbg2<<std::endl;
    }
    return rc;
}


inline static off_t c2_stream_tell(cache_vars_t* c){
  return c->read_filepos;
}

static int __FASTCALL__ c2_stream_seek(cache_vars_t* c,off_t pos)
{
  mpxp_dbg2<<"c2_seek to "<<pos<<" ("<<START_FILEPOS(*c)<<" "<<END_FILEPOS(*c)<<") "<<c->first<<std::endl;
  if(pos>=START_FILEPOS(*c) && pos < END_FILEPOS(*c))
  {
	c->read_filepos=pos;
	return pos;
  }
  return c2_stream_seek_long(c,pos);
}

inline static int __FASTCALL__ c2_stream_skip(cache_vars_t* c,off_t len)
{
    return c2_stream_seek(c,c2_stream_tell(c)+len);
}

static int __FASTCALL__ c2_stream_eof(cache_vars_t*c)
{
    unsigned cur;
    int retval;
    cur = c2_find_packet(c,c->read_filepos);
    if(cur!=UINT_MAX) CACHE2_PACKET_UNLOCK(c->packets[cur]);
    retval = cur!=UINT_MAX?((int)(c->packets[cur].state&CPF_EOF)):c->eof;
    mpxp_dbg2<<"stream_eof: "<<retval<<std::endl;
    return retval;
}

static void __FASTCALL__ c2_stream_set_eof(cache_vars_t*c,int eof)
{
    unsigned cur;
    cur = c2_find_packet(c,c->read_filepos);
    if(cur != UINT_MAX)
    {
	if(eof) c->packets[cur].state|=CPF_EOF;
	else	c->packets[cur].state&=~CPF_EOF;
	CACHE2_PACKET_UNLOCK(c->packets[cur]);
    }
    c->eof=eof;
    mpxp_dbg2<<"stream_set_eof: "<<eof<<std::endl;
}

/*
    main interface here!
*/
Cached_Stream::Cached_Stream(libinput_t& libinput,int size,int _min,int prefill,Stream::type_e t)
	    :Stream(t)
{
    int ss=sector_size()>1?sector_size():STREAM_BUFFER_SIZE;
    cache_vars_t* c;

    if (!(type()&Stream::Type_Seekable)) {
	// The stream has no 'fd' behind it, so is non-cacheable
	mpxp_warn<<"\rThis stream is non-cacheable"<<std::endl;
	return;
    }

    if(size<32*1024) size=32*1024; // 32kb min
    c=c2_cache_init(size,ss);
    cache_data=c;
    if(!c) return;
    c->stream=this;
    c->prefill=size*prefill;
    c->read_filepos=start_pos();

    unsigned rc;
    if((rc=xmp_register_thread(NULL,sig_cache2,cache2_routine,"cache2"))==UINT_MAX) return;
    c->pth=mpxp_context().engine().xp_core->mpxp_threads[rc];
    // wait until cache is filled at least prefill_init %
    mpxp_v<<"CACHE_PRE_INIT: "<<START_FILEPOS(*c)<<" ["<<c->read_filepos<<"] "<<END_FILEPOS(*c)<<" pre:"<<_min<<" eof:"<<c->eof<<" SS="<<ss<<std::endl;
    while((c->read_filepos<START_FILEPOS(*c) || END_FILEPOS(*c)-c->read_filepos<_min)
	&& !c->eof && CP_NEXT(*c,c->last)!=c->first){
	if(!(type()&Stream::Type_Seekable))
	mpxp_status<<"\rCache fill: "<<(100.0*(float)(END_FILEPOS(*c)-c->read_filepos)/(float)(c->buffer_size))<<"% ("<<(END_FILEPOS(*c)-c->read_filepos)<<" bytes)    ";
	else
	mpxp_v<<"\rCache fill: "<<(100.0*(float)(END_FILEPOS(*c)-c->read_filepos)/(float)(c->buffer_size))<<"% ("<<(END_FILEPOS(*c)-c->read_filepos)<<" bytes)    ";
	if(c->eof) break; // file is smaller than prefill size
	if(mpdemux_check_interrupt(libinput,PREFILL_SLEEP_TIME))
	  return;
    }
    mpxp_status<<"cache info: size="<<size<<" min="<<_min<<" prefill="<<prefill<<std::endl;
    return; // parent exits
}

Cached_Stream::~Cached_Stream() {
    cache_vars_t* c;
    c=cache_data;
    if(c) {
	if(c->pth && c->pth->state==Pth_Run) {
	    c->pth->state=Pth_Canceling;
	    while(c->pth->state==Pth_Canceling && !was_killed) usleep(0);
	}
	delete c->packets;
	delete c->mem;
	delete c;
    }
}

binary_packet Cached_Stream::read(size_t total)
{
    if(cache_data)	return c2_stream_read(cache_data,total);
    else		return Stream::read(total);
}

int Cached_Stream::eof() const
{
    if(cache_data)	return c2_stream_eof(cache_data);
    else		return Stream::eof();
}

void Cached_Stream::eof(int _e)
{
    if(!_e) reset();
    else {
	if(cache_data)	c2_stream_set_eof(cache_data,_e);
	else		Stream::eof(_e);
    }
}

off_t Cached_Stream::tell() const
{
    if(cache_data)	return c2_stream_tell(cache_data);
    else		return Stream::tell();
}

off_t Cached_Stream::seek(off_t _p)
{
    if(cache_data)	return c2_stream_seek(cache_data,_p);
    else		return Stream::seek(_p);
}

int Cached_Stream::skip(off_t len)
{
    if(cache_data)	return c2_stream_skip(cache_data,len);
    else		return Stream::skip(len);
}

void Cached_Stream::reset()
{
    if(cache_data)	c2_stream_reset(cache_data);
    else		Stream::reset();
}

} // namespace	usr
