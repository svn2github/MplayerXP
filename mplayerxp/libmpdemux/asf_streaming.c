#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "../mp_config.h"
#include "../mplayer.h"
#ifndef HAVE_WINSOCK2
#define closesocket close
#else
#include <winsock2.h>
#endif

#include "url.h"
#include "tcp.h"
#include "http.h"
#include "asf.h"

#include "stream.h"

#include "network.h"
#include "demux_msg.h"

#if defined( ARCH_X86 ) || defined(ARCH_X86_64)
#define	ASF_LOAD_GUID_PREFIX(guid)	(*(uint32_t *)(guid))
#else
#define	ASF_LOAD_GUID_PREFIX(guid)	\
	((guid)[3] << 24 | (guid)[2] << 16 | (guid)[1] << 8 | (guid)[0])
#endif

// ASF streaming support several network protocol.
// One use UDP, not known, yet!
// Another is HTTP, this one is known.
// So for now, we use the HTTP protocol.
// 
// We can try several protocol for asf streaming
// * first the UDP protcol, if there is a firewall, UDP
//   packets will not come back, so the mmsu will failed.
// * Then we can try TCP, but if there is a proxy for
//   internet connection, the TCP connection will not get
//   through
// * Then we can try HTTP.
// 
// Note: 	MMS/HTTP support is now a "well known" support protocol,
// 		it has been tested for while, not like MMST support.
// 		WMP sequence is MMSU then MMST and then HTTP.
// 		In MPlayer case since HTTP support is more reliable,
// 		we are doing HTTP first then we try MMST if HTTP fail.
int asf_http_streaming_start(stream_t *stream, int *demuxer_type );
int asf_mmst_streaming_start(stream_t *stream );

/*
 ASF streaming support several network protocol.
 One use UDP, not known, yet!
 Another is HTTP, this one is known.
 So for now, we use the HTTP protocol.
 
 We can try several protocol for asf streaming
 * first the UDP protcol, if there is a firewall, UDP
   packets will not come back, so the mmsu will failed.
 * Then we can try TCP, but if there is a proxy for
   internet connection, the TCP connection will not get
   through
 * Then we can try HTTP.
 
 Note: 	MMS/HTTP support is now a "well known" support protocol,
 		it has been tested for while, not like MMST support.
 		WMP sequence is MMSU then MMST and then HTTP.
 		In MPlayer case since HTTP support is more reliable,
 		we are doing HTTP first then we try MMST if HTTP fail.
*/
int
asf_streaming_start( stream_t *stream, int *demuxer_type) {
    char *proto = stream->streaming_ctrl->url->protocol;
    int fd = -1;
    int port = stream->streaming_ctrl->url->port;

    // Is protocol even valid mms,mmsu,mmst,http,http_proxy?
    if (!(!strncasecmp(proto, "mmst", 4) || !strncasecmp(proto, "mmsu", 4) ||
	!strncasecmp(proto, "http_proxy", 10) || !strncasecmp(proto, "mms", 3) ||
	!strncasecmp(proto, "http", 4)))
    {
        MSG_ERR("Unknown protocol: %s\n", proto );
        return -1;
    }

    // Is protocol mms or mmsu?
    if (!strncasecmp(proto, "mmsu", 4) || !strncasecmp(proto, "mms", 3))
    {
		MSG_V("Trying ASF/UDP...\n");
		//fd = asf_mmsu_streaming_start( stream );
		if( fd>-1 ) return fd; //mmsu support is not implemented yet - using this code
		MSG_V("  ===> ASF/UDP failed\n");
		if( fd==-2 ) return -1;
	}

    //Is protocol mms or mmst?
    if (!strncasecmp(proto, "mmst", 4) || !strncasecmp(proto, "mms", 3))
    {
		MSG_V("Trying ASF/TCP...\n");
		fd = asf_mmst_streaming_start( stream );
		stream->streaming_ctrl->url->port = port;
		if( fd>-1 ) return fd;
		MSG_V("  ===> ASF/TCP failed\n");
		if( fd==-2 ) return -1;
	}

    //Is protocol http, http_proxy, or mms? 
    if (!strncasecmp(proto, "http_proxy", 10) || !strncasecmp(proto, "http", 4) ||
	!strncasecmp(proto, "mms", 3))
    {
		MSG_V("Trying ASF/HTTP...\n");
		fd = asf_http_streaming_start( stream, demuxer_type );
		stream->streaming_ctrl->url->port = port;
		if( fd>-1 ) return fd;
		MSG_V("  ===> ASF/HTTP failed\n");
		if( fd==-2 ) return -1;
	}

    //everything failed
	return -1;
}

int 
asf_streaming(ASF_stream_chunck_t *stream_chunck, int *drop_packet ) {
	if( drop_packet!=NULL ) *drop_packet = 0;

	if( stream_chunck->size<8 ) {
		MSG_ERR("Ahhhh, stream_chunck size is too small: %d\n", stream_chunck->size);
		return -1;
	}
	if( stream_chunck->size!=stream_chunck->size_confirm ) {
		MSG_ERR("size_confirm mismatch!: %d %d\n", stream_chunck->size, stream_chunck->size_confirm);
		return -1;
	}
	switch(stream_chunck->type) {
		case ASF_STREAMING_CLEAR:	// $C	Clear ASF configuration
			MSG_V("=====> Clearing ASF stream configuration!\n");
			if( drop_packet!=NULL ) *drop_packet = 1;
			return stream_chunck->size;
			break;
		case ASF_STREAMING_DATA:	// $D	Data follows
			break;
		case ASF_STREAMING_END_TRANS:	// $E	Transfer complete
			MSG_V("=====> Transfer complete\n");
			if( drop_packet!=NULL ) *drop_packet = 1;
			return stream_chunck->size;
			break;
		case ASF_STREAMING_HEADER:	// $H	ASF header chunk follows
			MSG_V("=====> ASF header chunk follows\n");
			break;
		default:
			MSG_V("=====> Unknown stream type 0x%x\n", stream_chunck->type );
	}
	return stream_chunck->size+4;
}

static const char asf_stream_header_guid[16] = {0x91, 0x07, 0xdc, 0xb7,
  0xb7, 0xa9, 0xcf, 0x11, 0x8e, 0xe6, 0x00, 0xc0, 0x0c, 0x20, 0x53, 0x65};
static const char asf_file_header_guid[16] = {0xa1, 0xdc, 0xab, 0x8c,
  0x47, 0xa9, 0xcf, 0x11, 0x8e, 0xe4, 0x00, 0xc0, 0x0c, 0x20, 0x53, 0x65};
static const char asf_content_desc_guid[16] = {0x33, 0x26, 0xb2, 0x75,
  0x8e, 0x66, 0xcf, 0x11, 0xa6, 0xd9, 0x00, 0xaa, 0x00, 0x62, 0xce, 0x6c};
static const char asf_stream_group_guid[16] = {0xce, 0x75, 0xf8, 0x7b,
  0x8d, 0x46, 0xd1, 0x11, 0x8d, 0x82, 0x00, 0x60, 0x97, 0xc9, 0xa2, 0xb2};
static const char asf_data_chunk_guid[16] = {0x36, 0x26, 0xb2, 0x75,
  0x8e, 0x66, 0xcf, 0x11, 0xa6, 0xd9, 0x00, 0xaa, 0x00, 0x62, 0xce, 0x6c};
static int find_asf_guid(char *buf, const char *guid, int cur_pos, int buf_len)
{
  int i;
  for (i = cur_pos; i < buf_len - 19; i++) {
    if (memcmp(&buf[i], guid, 16) == 0)
      return i + 16 + 8; // point after guid + length
  }
  return -1;
}

static int max_idx(int s_count, int *s_rates, int bound) {
  int i, best = -1, rate = -1;
  for (i = 0; i < s_count; i++) {
    if (s_rates[i] > rate && s_rates[i] <= bound) {
      rate = s_rates[i];
      best = i;
    }
  }
  return best;
}

static int
asf_streaming_parse_header(int fd, streaming_ctrl_t* streaming_ctrl) {
  ASF_header_t asfh;
  ASF_stream_chunck_t chunk;
  asf_http_streaming_ctrl_t* asf_ctrl = (asf_http_streaming_ctrl_t*) streaming_ctrl->data;
  char* buffer=NULL, *chunk_buffer=NULL;
  int i,r,size,pos = 0;
  int start;
  int buffer_size = 0;
  int chunk_size2read = 0;
  int bw = streaming_ctrl->bandwidth;
  int *v_rates = NULL, *a_rates = NULL;
  int v_rate = 0, a_rate = 0, a_idx = -1, v_idx = -1;
  
  if(asf_ctrl == NULL) return -1;

	// The ASF header can be in several network chunks. For example if the content description
	// is big, the ASF header will be split in 2 network chunk.
	// So we need to retrieve all the chunk before starting to parse the header.
  do {
	  for( r=0; r < (int)sizeof(ASF_stream_chunck_t) ; ) {
		i = nop_streaming_read(fd,((char*)&chunk)+r,sizeof(ASF_stream_chunck_t) - r,streaming_ctrl);
		if(i <= 0) return -1;
		r += i;
	  }
	  // Endian handling of the stream chunk
	  le2me_ASF_stream_chunck_t(&chunk);
	  size = asf_streaming( &chunk, &r) - sizeof(ASF_stream_chunck_t);
	  if(r) MSG_WARN("Warning : drop header ????\n");
	  if(size < 0){
		MSG_ERR("Error while parsing chunk header\n");
		return -1;
	  }
	  if (chunk.type != ASF_STREAMING_HEADER) {
	    MSG_ERR("Don't got a header as first chunk !!!!\n");
	    return -1;
	  }
	  
	  // audit: do not overflow buffer_size
	  if (size > SIZE_MAX - buffer_size) return -1;
	  buffer = (char*) malloc(size+buffer_size);
	  if(buffer == NULL) {
	    MSG_FATAL("Error can't allocate %d bytes buffer\n",size+buffer_size);
	    return -1;
	  }
	  if( chunk_buffer!=NULL ) {
	  	memcpy( buffer, chunk_buffer, buffer_size );
		free( chunk_buffer );
	  }
	  chunk_buffer = buffer;
	  buffer += buffer_size;
	  buffer_size += size;
	  
	  for(r = 0; r < size;) {
	    i = nop_streaming_read(fd,buffer+r,size-r,streaming_ctrl);
	    if(i < 0) {
		    MSG_ERR("Error while reading network stream\n");
		    return -1;
	    }
	    r += i;
	  }  

	  if( chunk_size2read==0 ) {
		if(size < (int)sizeof(asfh)) {
		    MSG_ERR("Error chunk is too small\n");
		    return -1;
		} else MSG_DBG2("Got chunk\n");
	  	memcpy(&asfh,buffer,sizeof(asfh));
	  	le2me_ASF_header_t(&asfh);
		chunk_size2read = asfh.objh.size;
		MSG_DBG2("Size 2 read=%d\n", chunk_size2read);
	  }
  } while( buffer_size<chunk_size2read);
  buffer = chunk_buffer;
  size = buffer_size;
	  
  if(asfh.cno > 256) {
    MSG_ERR("Error sub chunks number is invalid\n");
    return -1;
  }

  start = sizeof(asfh);
  
  pos = find_asf_guid(buffer, asf_file_header_guid, start, size);
  if (pos >= 0) {
    ASF_file_header_t *fileh = (ASF_file_header_t *) &buffer[pos];
    pos += sizeof(ASF_file_header_t);
    if (pos > size) goto len_err_out;
      le2me_ASF_file_header_t(fileh);
/*
      if(fileh.packetsize != fileh.packetsize2) {
	printf("Error packetsize check don't match\n");
	return -1;
      }
*/
      asf_ctrl->packet_size = fileh->max_packet_size;
      // before playing. 
      // preroll: time in ms to bufferize before playing
      streaming_ctrl->prebuffer_size = (unsigned int)(((double)fileh->preroll/1000.0)*((double)fileh->max_bitrate/8.0));
  }

  pos = start;
  while ((pos = find_asf_guid(buffer, asf_stream_header_guid, pos, size)) >= 0)
  {
    ASF_stream_header_t *streamh = (ASF_stream_header_t *)&buffer[pos];
    pos += sizeof(ASF_stream_header_t);
    if (pos > size) goto len_err_out;
      le2me_ASF_stream_header_t(streamh);
      switch(ASF_LOAD_GUID_PREFIX(streamh->type)) {
      case 0xF8699E40 : // audio stream
	if(asf_ctrl->audio_streams == NULL){
	  asf_ctrl->audio_streams = (int*)malloc(sizeof(int));
	  asf_ctrl->n_audio = 1;
	} else {
	  asf_ctrl->n_audio++;
	  asf_ctrl->audio_streams = (int*)realloc(asf_ctrl->audio_streams,
						     asf_ctrl->n_audio*sizeof(int));
	}
	asf_ctrl->audio_streams[asf_ctrl->n_audio-1] = streamh->stream_no;
	break;
      case 0xBC19EFC0 : // video stream
	if(asf_ctrl->video_streams == NULL){
	  asf_ctrl->video_streams = (int*)malloc(sizeof(int));
	  asf_ctrl->n_video = 1;
	} else {
	  asf_ctrl->n_video++;
	  asf_ctrl->video_streams = (int*)realloc(asf_ctrl->video_streams,
						     asf_ctrl->n_video*sizeof(int));
	}
	asf_ctrl->video_streams[asf_ctrl->n_video-1] = streamh->stream_no;
	break;
      }
  }

  // always allocate to avoid lots of ifs later
  v_rates = calloc(asf_ctrl->n_video, sizeof(int));
  a_rates = calloc(asf_ctrl->n_audio, sizeof(int));

  pos = find_asf_guid(buffer, asf_stream_group_guid, start, size);
  if (pos >= 0) {
    // stream bitrate properties object
	int stream_count;
	char *ptr = &buffer[pos];
	
	MSG_V("Stream bitrate properties object\n");
		stream_count = le2me_16(*(uint16_t*)ptr);
		ptr += sizeof(uint16_t);
		if (ptr > &buffer[size]) goto len_err_out;
		MSG_V(" stream count=[0x%x][%u]\n",
		        stream_count, stream_count );
		for( i=0 ; i<stream_count ; i++ ) {
			uint32_t rate;
			int id;
			int j;
			id = le2me_16(*(uint16_t*)ptr);
			ptr += sizeof(uint16_t);
			if (ptr > &buffer[size]) goto len_err_out;
			memcpy(&rate, ptr, sizeof(uint32_t));// workaround unaligment bug on sparc
			ptr += sizeof(uint32_t);
			if (ptr > &buffer[size]) goto len_err_out;
			rate = le2me_32(rate);
			MSG_V(
                                "  stream id=[0x%x][%u]\n", id, id);
			MSG_V(
			        "  max bitrate=[0x%x][%u]\n", rate, rate);
			for (j = 0; j < asf_ctrl->n_video; j++) {
			  if (id == asf_ctrl->video_streams[j]) {
			    MSG_V("  is video stream\n");
			    v_rates[j] = rate;
			    break;
			  }
			}
			for (j = 0; j < asf_ctrl->n_audio; j++) {
			  if (id == asf_ctrl->audio_streams[j]) {
			    MSG_V( "  is audio stream\n");
			    a_rates[j] = rate;
			    break;
			  }
			}
		}
  }
  free(buffer);

  // automatic stream selection based on bandwidth
  if (bw == 0) bw = INT_MAX;
  MSG_V( "Max bandwidth set to %d\n", bw);

  if (asf_ctrl->n_audio) {
    // find lowest-bitrate audio stream
    a_rate = a_rates[0];
    a_idx = 0;
    for (i = 0; i < asf_ctrl->n_audio; i++) {
      if (a_rates[i] < a_rate) {
        a_rate = a_rates[i];
        a_idx = i;
      }
    }
    if (max_idx(asf_ctrl->n_video, v_rates, bw - a_rate) < 0) {
      // both audio and video are not possible, try video only next
      a_idx = -1;
      a_rate = 0;
    }
  }
  // find best video stream
  v_idx = max_idx(asf_ctrl->n_video, v_rates, bw - a_rate);
  if (v_idx >= 0)
    v_rate = v_rates[v_idx];

  // find best audio stream
  a_idx = max_idx(asf_ctrl->n_audio, a_rates, bw - v_rate);
    
  free(v_rates);
  free(a_rates);

  if (a_idx < 0 && v_idx < 0) {
    MSG_FATAL( "bandwidth too small, "
            "file cannot be played!\n");
    return -1;
  }

  if (audio_id > 0)
    // a audio stream was forced
    asf_ctrl->audio_id = audio_id;
  else if (a_idx >= 0)
    asf_ctrl->audio_id = asf_ctrl->audio_streams[a_idx];
  else if (asf_ctrl->n_audio) {
    MSG_WARN( "bandwidth too small, "
            "deselected audio stream\n");
    audio_id = -2;
  }

  if (video_id > 0)
    // a video stream was forced
    asf_ctrl->video_id = video_id;
  else if (v_idx >= 0)
    asf_ctrl->video_id = asf_ctrl->video_streams[v_idx];
  else if (asf_ctrl->n_video) {
    MSG_WARN( "bandwidth too small, "
            "deselected video stream\n");
    video_id = -2;
  }

  return 1;

len_err_out:
  MSG_FATAL( "Invalid length in ASF header!\n");
  if (buffer) free(buffer);
  if (v_rates) free(v_rates);
  if (a_rates) free(a_rates);
  return -1;
}

int
asf_http_streaming_read( int fd, char *buffer, int size, streaming_ctrl_t *streaming_ctrl ) {
  static ASF_stream_chunck_t chunk;
  int read,chunk_size = 0;
  static int rest = 0, drop_chunk = 0, waiting = 0;
  asf_http_streaming_ctrl_t *asf_http_ctrl = (asf_http_streaming_ctrl_t*)streaming_ctrl->data;

  while(1) {
    if (rest == 0 && waiting == 0) {
      read = 0;
      while(read < (int)sizeof(ASF_stream_chunck_t)){
	int r = nop_streaming_read( fd, ((char*)&chunk) + read, 
				    sizeof(ASF_stream_chunck_t)-read, 
				    streaming_ctrl );
	if(r <= 0){
	  if( r < 0) 
	    MSG_ERR("Error while reading chunk header\n");
	  return -1;
	}
	read += r;
      }
      
      // Endian handling of the stream chunk
      le2me_ASF_stream_chunck_t(&chunk);
      chunk_size = asf_streaming( &chunk, &drop_chunk );
      if(chunk_size < 0) {
	MSG_ERR("Error while parsing chunk header\n");
	return -1;
      }
      chunk_size -= sizeof(ASF_stream_chunck_t);
	
      if(chunk.type != ASF_STREAMING_HEADER && (!drop_chunk)) {
	if (asf_http_ctrl->packet_size < chunk_size) {
	  MSG_ERR("Error chunk_size > packet_size\n");
	  return -1;
	}
	waiting = asf_http_ctrl->packet_size;
      } else {
	waiting = chunk_size;
      }

    } else if (rest){
      chunk_size = rest;
      rest = 0;
    }

    read = 0;
    if ( waiting >= chunk_size) {
      if (chunk_size > size){
	rest = chunk_size - size;
	chunk_size = size;
      }
      while(read < chunk_size) {
	int got = nop_streaming_read( fd,buffer+read,chunk_size-read,streaming_ctrl );
	if(got <= 0) {
	  if(got < 0)
	    MSG_ERR("Error while reading chunk\n");
	  return -1;
	}
	read += got;
      }
      waiting -= read;
      if (drop_chunk) continue;
    }
    if (rest == 0 && waiting > 0 && size-read > 0) {
      int s = MIN(waiting,size-read);
      memset(buffer+read,0,s);
      waiting -= s;
      read += s;
    }
    break;
  }

  return read;
}

int
asf_http_streaming_seek( int fd, off_t pos, streaming_ctrl_t *streaming_ctrl ) {
	return -1;
	// to shut up gcc warning
	fd++;
	pos++;
	streaming_ctrl=NULL;
}

int
asf_header_check( HTTP_header_t *http_hdr ) {
	ASF_obj_header_t *objh;
	if( http_hdr==NULL ) return -1;
	if( http_hdr->body==NULL || http_hdr->body_size<sizeof(ASF_obj_header_t) ) return -1;

	objh = (ASF_obj_header_t*)http_hdr->body;
	if( ASF_LOAD_GUID_PREFIX(objh->guid)==0x75B22630 ) return 0;
	return -1;
}

int
asf_http_streaming_type(char *content_type, char *features, HTTP_header_t *http_hdr ) {
	if( content_type==NULL ) return ASF_Unknown_e;
	if( 	!strcasecmp(content_type, "application/octet-stream") ||
		!strcasecmp(content_type, "application/vnd.ms.wms-hdr.asfv1") ||        // New in Corona, first request
		!strcasecmp(content_type, "application/x-mms-framed") ||                // New in Corana, second request
		!strcasecmp(content_type, "video/x-ms-asf")) {               

		if( strstr(features, "broadcast") ) {
			MSG_V("=====> ASF Live stream\n");
			return ASF_Live_e;
		} else {
			MSG_V("=====> ASF Prerecorded\n");
			return ASF_Prerecorded_e;
		}
	} else {
		// Ok in a perfect world, web servers should be well configured
		// so we could used mime type to know the stream type,
		// but guess what? All of them are not well configured.
		// So we have to check for an asf header :(, but it works :p
		if( http_hdr->body_size>sizeof(ASF_obj_header_t) ) {
			if( asf_header_check( http_hdr )==0 ) {
				MSG_V("=====> ASF Plain text\n");
				return ASF_PlainText_e;
			} else if( (!strcasecmp(content_type, "text/html")) ) {
				MSG_V("=====> HTML, mplayer is not a browser...yet!\n");
				return ASF_Unknown_e;
			} else {
				MSG_V("=====> ASF Redirector\n");
				return ASF_Redirector_e;
			}
		} else {
			if(	(!strcasecmp(content_type, "audio/x-ms-wax")) ||
				(!strcasecmp(content_type, "audio/x-ms-wma")) ||
				(!strcasecmp(content_type, "video/x-ms-asf")) ||
				(!strcasecmp(content_type, "video/x-ms-afs")) ||
				(!strcasecmp(content_type, "video/x-ms-wvx")) ||
				(!strcasecmp(content_type, "video/x-ms-wmv")) ||
				(!strcasecmp(content_type, "video/x-ms-wma")) ) {
				MSG_ERR("=====> ASF Redirector\n");
				return ASF_Redirector_e;
			} else if( !strcasecmp(content_type, "text/plain") ) {
				MSG_V("=====> ASF Plain text\n");
				return ASF_PlainText_e;
			} else {
				MSG_V("=====> ASF unknown content-type: %s\n", content_type );
				return ASF_Unknown_e;
			}
		}
	}
	return ASF_Unknown_e;
}

HTTP_header_t *
asf_http_request(streaming_ctrl_t *streaming_ctrl) {
	HTTP_header_t *http_hdr;
	URL_t *url = NULL;
	URL_t *server_url = NULL;
	asf_http_streaming_ctrl_t *asf_http_ctrl;
	char str[250];
	char *ptr;
	int i, enable;

	int offset_hi=0, offset_lo=0, length=0;
	int asf_nb_stream=0, stream_id;

	// Sanity check
	if( streaming_ctrl==NULL ) return NULL;
	url = streaming_ctrl->url;
	asf_http_ctrl = (asf_http_streaming_ctrl_t*)streaming_ctrl->data;
	if( url==NULL || asf_http_ctrl==NULL ) return NULL;

	// Common header for all requests.
	http_hdr = http_new_header();
	http_set_field( http_hdr, "Accept: */*" );
	http_set_field( http_hdr, "User-Agent: NSPlayer/4.1.0.3856" );
	http_add_basic_authentication( http_hdr, url->username, url->password );

	// Check if we are using a proxy
	if( !strcasecmp( url->protocol, "http_proxy" ) ) {
		server_url = url_new( (url->file)+1 );
		if( server_url==NULL ) {
			MSG_ERR("Invalid proxy URL\n");
			http_free( http_hdr );
			return NULL;
		}
		http_set_uri( http_hdr, server_url->url );
		sprintf( str, "Host: %.220s:%d", server_url->hostname, server_url->port );
		url_free( server_url );
	} else {
		http_set_uri( http_hdr, url->file );
		sprintf( str, "Host: %.220s:%d", url->hostname, url->port );
	}
	
	http_set_field( http_hdr, str );
	http_set_field( http_hdr, "Pragma: xClientGUID={c77e7400-738a-11d2-9add-0020af0a3278}" );
	sprintf(str, 
		"Pragma: no-cache,rate=1.000000,stream-time=0,stream-offset=%u:%u,request-context=%d,max-duration=%u",
		offset_hi, offset_lo, asf_http_ctrl->request, length );
	http_set_field( http_hdr, str );

	switch( asf_http_ctrl->streaming_type ) {
		case ASF_Live_e:
		case ASF_Prerecorded_e:
			http_set_field( http_hdr, "Pragma: xPlayStrm=1" );
			ptr = str;
			ptr += sprintf( ptr, "Pragma: stream-switch-entry=");
			if(asf_http_ctrl->n_audio > 0) {
				for( i=0; i<asf_http_ctrl->n_audio ; i++ ) {
					stream_id = asf_http_ctrl->audio_streams[i];
					if(stream_id == asf_http_ctrl->audio_id) {
						enable = 0;
					} else {
						enable = 2;
						continue;
					}
					asf_nb_stream++;
					ptr += sprintf(ptr, "ffff:%d:%d ", stream_id, enable);
				}
			}
			if(asf_http_ctrl->n_video > 0) {
				for( i=0; i<asf_http_ctrl->n_video ; i++ ) {
					stream_id = asf_http_ctrl->video_streams[i];
					if(stream_id == asf_http_ctrl->video_id) {
						enable = 0;
					} else {
						enable = 2;
						continue;
					}
					asf_nb_stream++;
					ptr += sprintf(ptr, "ffff:%d:%d ", stream_id, enable);
				}
			}
			http_set_field( http_hdr, str );
			sprintf( str, "Pragma: stream-switch-count=%d", asf_nb_stream );
			http_set_field( http_hdr, str );
			break;
		case ASF_Redirector_e:
			break;
		case ASF_Unknown_e:
			// First request goes here.
			break;
		default:
			MSG_ERR("Unknown asf stream type\n");
	}

	http_set_field( http_hdr, "Connection: Close" );
	http_build_request( http_hdr );

	return http_hdr;
}

int
asf_http_parse_response(asf_http_streaming_ctrl_t *asf_http_ctrl, HTTP_header_t *http_hdr ) {
	char *content_type, *pragma;
	char features[64] = "\0";
	size_t len;
	if( http_response_parse(http_hdr)<0 ) {
		MSG_ERR("Failed to parse HTTP response\n");
		return -1;
	}
	switch( http_hdr->status_code ) {
		case 200:
			break;
		case 401: // Authentication required
			return ASF_Authenticate_e;
		default:
			MSG_ERR("Server return %d:%s\n", http_hdr->status_code, http_hdr->reason_phrase);
			return -1;
	}

	content_type = http_get_field( http_hdr, "Content-Type");

	pragma = http_get_field( http_hdr, "Pragma");
	while( pragma!=NULL ) {
		char *comma_ptr=NULL;
		char *end;
		// The pragma line can get severals attributes 
		// separeted with a comma ','.
		do {
			if( !strncasecmp( pragma, "features=", 9) ) {
				pragma += 9;
				end = strstr( pragma, "," );
				if( end==NULL ) {
				  size_t s = strlen(pragma);
				  if(s > sizeof(features)) {
				    MSG_WARN("ASF HTTP PARSE WARNING : Pragma %s cuted from %d bytes to %d\n",pragma,s,sizeof(features));
				    len = sizeof(features);
				  } else {				   
				    len = s;
				  }
				} else { 
				  len = MIN((unsigned int)(end-pragma),sizeof(features));
				}
				strncpy( features, pragma, len );
				features[len]='\0';
				break;
			}
			comma_ptr = strstr( pragma, "," );
			if( comma_ptr!=NULL ) {
				pragma = comma_ptr+1;
				if( pragma[0]==' ' ) pragma++;
			}
		} while( comma_ptr!=NULL );
		pragma = http_get_next_field( http_hdr );
	}
	asf_http_ctrl->streaming_type = asf_http_streaming_type( content_type, features, http_hdr );
	return 0;
}

int
asf_http_streaming_start( stream_t *stream, int *demuxer_type ) {
	HTTP_header_t *http_hdr=NULL;
	URL_t *url = stream->streaming_ctrl->url;
	asf_http_streaming_ctrl_t *asf_http_ctrl;
	char buffer[BUFFER_SIZE];
	int i, ret;
	int fd = stream->fd;
	int done;
	int auth_retry = 0;

	asf_http_ctrl = (asf_http_streaming_ctrl_t*)malloc(sizeof(asf_http_streaming_ctrl_t));
	if( asf_http_ctrl==NULL ) {
		MSG_FATAL("Memory allocation failed\n");
		return -1;
	}
	asf_http_ctrl->streaming_type = ASF_Unknown_e;
	asf_http_ctrl->request = 1;
	asf_http_ctrl->audio_streams = asf_http_ctrl->video_streams = NULL;
	asf_http_ctrl->n_audio = asf_http_ctrl->n_video = 0;
	stream->streaming_ctrl->data = (void*)asf_http_ctrl;

	do {
		done = 1;
		if( fd>0 ) closesocket( fd );

		if( !strcasecmp( url->protocol, "http_proxy" ) ) {
			if( url->port==0 ) url->port = 8080;
		} else {
			if( url->port==0 ) url->port = 80;
		}
		fd = tcp_connect2Server( url->hostname, url->port, 0);
		if( fd<0 ) return fd;

		http_hdr = asf_http_request( stream->streaming_ctrl );
		MSG_DBG2("Request [%s]\n", http_hdr->buffer );
		for(i=0; i < (int)http_hdr->buffer_size ; ) {
			int r = send( fd, http_hdr->buffer+i, http_hdr->buffer_size-i, 0);
			if(r <0) {
				MSG_ERR("Socket write error : %s\n",strerror(errno));
				return -1;
			}
			i += r;
		}       
		http_free( http_hdr );
		http_hdr = http_new_header();
		do {
			i = recv( fd, buffer, BUFFER_SIZE, 0);
			if( i<=0 ) {
				perror("read");
				http_free( http_hdr );
				return -1;
			}
			http_response_append( http_hdr, buffer, i );
		} while( !http_is_header_entire( http_hdr ) );
		if( verbose>0 ) {
			http_hdr->buffer[http_hdr->buffer_size]='\0';
			MSG_DBG2("Response [%s]\n", http_hdr->buffer );
		}
		ret = asf_http_parse_response(asf_http_ctrl, http_hdr);
		if( ret<0 ) {
			MSG_ERR("Failed to parse header\n");
			http_free( http_hdr );
			return -1;
		}
		switch( asf_http_ctrl->streaming_type ) {
			case ASF_Live_e:
			case ASF_Prerecorded_e:
			case ASF_PlainText_e:
				if( http_hdr->body_size>0 ) {
					if( streaming_bufferize( stream->streaming_ctrl, http_hdr->body, http_hdr->body_size )<0 ) {
						http_free( http_hdr );
						return -1;
					}
				}
				if( asf_http_ctrl->request==1 ) {
					if( asf_http_ctrl->streaming_type!=ASF_PlainText_e ) {
						// First request, we only got the ASF header.
						ret = asf_streaming_parse_header(fd,stream->streaming_ctrl);
						if(ret < 0) return -1;
						if(asf_http_ctrl->n_audio == 0 && asf_http_ctrl->n_video == 0) {
							MSG_ERR("No stream found\n");
							return -1;
						}
						asf_http_ctrl->request++;
						done = 0;
					} else {
						done = 1;
					}
				}
				break;
			case ASF_Redirector_e:
				if( http_hdr->body_size>0 ) {
					if( streaming_bufferize( stream->streaming_ctrl, http_hdr->body, http_hdr->body_size )<0 ) {
						http_free( http_hdr );
						return -1;
					}
				}
				stream->type |= STREAMTYPE_TEXT;
				done = 1;
				break;
			case ASF_Authenticate_e:
				if( http_authenticate( http_hdr, url, &auth_retry)<0 ) return -1;
				asf_http_ctrl->streaming_type = ASF_Unknown_e;
				done = 0;
				break;
			case ASF_Unknown_e:
			default:
				MSG_ERR("Unknown ASF streaming type\n");
				closesocket(fd);
				http_free( http_hdr );
				return -1;
		}
	// Check if we got a redirect.	
	} while(!done);

	stream->fd = fd;
	if( asf_http_ctrl->streaming_type==ASF_PlainText_e || asf_http_ctrl->streaming_type==ASF_Redirector_e ) {
		stream->streaming_ctrl->streaming_read = nop_streaming_read;
		stream->streaming_ctrl->streaming_seek = nop_streaming_seek;
	} else {
		stream->streaming_ctrl->streaming_read = asf_http_streaming_read;
		stream->streaming_ctrl->streaming_seek = asf_http_streaming_seek;
		stream->streaming_ctrl->buffering = 1;
	}
	stream->streaming_ctrl->status = streaming_playing_e;

	http_free( http_hdr );
	return 0;
}

