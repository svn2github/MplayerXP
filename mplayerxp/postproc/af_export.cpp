/* This audio filter exports the incomming signal to other processes
   using memory mapping. Memory mapped area contains a header:
      int nch,
      int size,
      unsigned long long counter (updated every time the  contents of
				  the area changes),
   the rest is payload (non-interleaved).
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include "mp_config.h"

#ifdef HAVE_SYS_MMAN_H
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "af.h"
#include "help_mp.h"
#include "osdep/get_path.h"
#include "osdep/fastmemcpy.h"
#include "osdep/mplib.h"
#include "pp_msg.h"

#define DEF_SZ 512 // default buffer size (in samples)
#define SHARED_FILE "mplayer-af_export" /* default file name
					   (relative to ~/.mplayer/ */

#define SIZE_HEADER (2 * sizeof(int) + sizeof(unsigned long long))

// Data for specific instances of this filter
typedef struct af_export_s
{
  unsigned long long  count; // Used for sync
  any_t* buf[AF_NCH]; 	// Buffers for storing the data before it is exported
  int 	sz;	      	// Size of buffer in samples
  int 	wi;  		// Write index
  int	fd;           	// File descriptor to shared memory area
  char* filename;      	// File to export data
  any_t* mmap_area;     	// MMap shared area
} af_export_t;


/* Initialization and runtime control
   af audio filter instance
   cmd control command
   arg argument
*/
static MPXP_Rc __FASTCALL__ control(struct af_instance_s* af, int cmd, any_t* arg)
{
  af_export_t* s = reinterpret_cast<af_export_t*>(af->setup);

  switch (cmd){
  case AF_CONTROL_REINIT:{
    unsigned i=0;
    unsigned mapsize;

    // Free previous buffers
    if (s->buf && s->buf[0])
      delete s->buf[0];

    // unmap previous area
    if(s->mmap_area)
      munmap(s->mmap_area, SIZE_HEADER + ((af->data->format&MPAF_BPS_MASK)*s->sz*af->data->nch));
    // close previous file descriptor
    if(s->fd)
      close(s->fd);

    // Accept only int16_t as input format (which sucks)
    af->data->rate   = ((mp_aframe_t*)arg)->rate;
    af->data->nch    = ((mp_aframe_t*)arg)->nch;
    af->data->format = MPAF_SI|MPAF_NE|MPAF_BPS_2;

    // If buffer length isn't set, set it to the default value
    if(s->sz == 0)
      s->sz = DEF_SZ;

    // Allocate new buffers (as one continuous block)
    s->buf[0] = mp_calloc(s->sz*af->data->nch, af->data->format&MPAF_BPS_MASK);
    if(NULL == s->buf[0])
      MSG_FATAL(MSGTR_OutOfMemory);
    for(i = 1; i < af->data->nch; i++)
      s->buf[i] = s->buf[0] + i*s->sz*(af->data->format&MPAF_BPS_MASK);

    // Init memory mapping
    s->fd = open(s->filename, O_RDWR | O_CREAT | O_TRUNC, 0640);
    MSG_INFO( "[export] Exporting to file: %s\n", s->filename);
    if(s->fd < 0)
      MSG_FATAL( "[export] Could not open/create file: %s\n",
	     s->filename);

    // header + buffer
    mapsize = (SIZE_HEADER + ((af->data->format&MPAF_BPS_MASK) * s->sz * af->data->nch));

    // grow file to needed size
    for(i = 0; i < mapsize; i++){
      char null = 0;
      write(s->fd, (any_t*) &null, 1);
    }

    // mmap size
    s->mmap_area = mmap(0, mapsize, PROT_READ|PROT_WRITE,MAP_SHARED, s->fd, 0);
    if(s->mmap_area == NULL)
      MSG_FATAL( "[export] Could not mmap file %s\n", s->filename);
    MSG_INFO( "[export] Memory mapped to file: %s (%p)\n",
	   s->filename, s->mmap_area);

    // Initialize header
    *((int*)s->mmap_area) = af->data->nch;
    *((int*)s->mmap_area + 1) = s->sz * (af->data->format&MPAF_BPS_MASK) * af->data->nch;
    msync(s->mmap_area, mapsize, MS_ASYNC);

    // Use test_output to return FALSE if necessary
    return af_test_output(af, (mp_aframe_t*)arg)?MPXP_Ok:MPXP_False;
  }
  case AF_CONTROL_COMMAND_LINE:{
    int i=0;
    char *str = reinterpret_cast<char*>(arg);

    if (!str){
      if(s->filename)
	delete s->filename;

      s->filename = get_path(SHARED_FILE);
      return MPXP_Ok;
    }

    while((str[i]) && (str[i] != ':'))
      i++;

    if(s->filename)
      delete s->filename;

    s->filename = new(zeromem) char[i + 1];
    memcpy(s->filename, str, i);
    s->filename[i] = 0;

    sscanf(str + i + 1, "%d", &(s->sz));

    return af->control(af, AF_CONTROL_EXPORT_SZ | AF_CONTROL_SET, &s->sz);
  }
  case AF_CONTROL_EXPORT_SZ | AF_CONTROL_SET:
    s->sz = * (int *) arg;
    if((s->sz <= 0) || (s->sz > 2048))
      MSG_ERR( "[export] Buffer size must be between"
	      " 1 and 2048\n" );

    return MPXP_Ok;
  case AF_CONTROL_EXPORT_SZ | AF_CONTROL_GET:
    *(int*) arg = s->sz;
    return MPXP_Ok;
  default: break;
  }
  return MPXP_Unknown;
}

/* Free allocated memory and clean up other stuff too.
   af audio filter instance
*/
static void __FASTCALL__ uninit( struct af_instance_s* af )
{
  if (af->data){
    delete af->data;
    af->data = NULL;
  }

  if(af->setup){
    af_export_t* s = reinterpret_cast<af_export_t*>(af->setup);
    if (s->buf && s->buf[0])
      delete s->buf[0];

    // Free mmaped area
    if(s->mmap_area)
      munmap(s->mmap_area, sizeof(af_export_t));

    if(s->fd > -1)
      close(s->fd);

    if(s->filename)
	delete s->filename;

    delete af->setup;
    af->setup = NULL;
  }
}

/* Filter data through filter
   af audio filter instance
   data audio data
*/
static mp_aframe_t* __FASTCALL__ play( struct af_instance_s* af, mp_aframe_t* data,int final)
{
  mp_aframe_t*	c   = data;	     // Current working data
  af_export_t*	s   = reinterpret_cast<af_export_t*>(af->setup); // Setup for this instance
  int16_t*	a   = reinterpret_cast<int16_t*>(c->audio);   // Incomming sound
  int		nch = c->nch;	     // Number of channels
  int		len = c->len/(c->format&MPAF_BPS_MASK); // Number of sample in data chunk
  int		sz  = s->sz;         // buffer size (in samples)
  int		flag = 0;	     // Set to 1 if buffer is filled

  int		ch, i;

  // Fill all buffers
  for(ch = 0; ch < nch; ch++){
    int 	wi = s->wi;    	 // Reset write index
    int16_t* 	b  = reinterpret_cast<int16_t*>(s->buf[ch]); // Current buffer

    // Copy data to export buffers
    for(i = ch; i < len; i += nch){
      b[wi++] = a[i];
      if(wi >= sz){ // Don't write outside the end of the buffer
	flag = 1;
	break;
      }
    }
    s->wi = wi % s->sz;
  }

  // Export buffer to mmaped area
  if(flag){
    // update buffer in mapped area
	stream_copy(s->mmap_area + SIZE_HEADER, s->buf[0], sz * (c->format&MPAF_BPS_MASK) * nch);
    s->count++; // increment counter (to sync)
	stream_copy(s->mmap_area + SIZE_HEADER - sizeof(s->count),
		&(s->count), sizeof(s->count));
  }

  // We don't modify data, just export it
  return data;
}

/* Allocate memory and set function pointers
   af audio filter instance
   returns MPXP_Ok or MPXP_Error
*/
static MPXP_Rc __FASTCALL__ af_open( af_instance_t* af )
{
  af->control = control;
  af->uninit  = uninit;
  af->play    = play;
  af->mul.n   = 1;
  af->mul.d   = 1;
  af->data    = new(zeromem) mp_aframe_t;
  af->setup   = new(zeromem) af_export_t;
  if((af->data == NULL) || (af->setup == NULL))
    return MPXP_Error;

  ((af_export_t *)af->setup)->filename = get_path(SHARED_FILE);
    check_pin("afilter",af->pin,AF_PIN);
  return MPXP_Ok;
}

extern const af_info_t af_info_export;
// Description of this filter
const af_info_t af_info_export = {
    "Sound export filter",
    "export",
    "Anders; Gustavo Sverzut Barbieri <gustavo.barbieri@ic.unicamp.br>",
    "",
    AF_FLAGS_REENTRANT,
    af_open
};

#endif /*HAVE_SYS_MMAN_H*/