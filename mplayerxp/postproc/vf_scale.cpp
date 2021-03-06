#include "mpxp_config.h"
#include "osdep/mplib.h"
using namespace	usr;
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "osdep/cpudetect.h"

#include "libvo2/img_format.h"
#include "xmpcore/xmp_image.h"
#include "vf.h"
#include "vf_internal.h"

#include "libvo2/video_out.h"
#include "osdep/fastmemcpy.h"
#include "swscale.h"
#include "libavutil/log.h"
#include "vf_scale.h"
#include "pp_msg.h"

struct vf_priv_t {
    int w,h,ofmt;
    int sw,sh,sfmt;
    int v_chr_drop;
    double param[2];
    unsigned int fmt;
    struct SwsContext *ctx;
    struct SwsContext *ctx2; //for interlaced slices only
    unsigned char* palette;
    int interlaced;
    int query_format_cache[64];
};
#if 0
 vf_priv_dflt = {
  -1,-1,
  0,
  {SWS_PARAM_DEFAULT, SWS_PARAM_DEFAULT},
  0,
  NULL,
  NULL,
  NULL,
  0
};
#endif
static int firstTime=1;
//===========================================================================//

void sws_getFlagsAndFilterFromCmdLine(int *flags, SwsFilter **srcFilterParam, SwsFilter **dstFilterParam);

static const unsigned int outfmt_list[]={
// YUV:
    IMGFMT_444P16_LE,
    IMGFMT_444P16_BE,
    IMGFMT_422P16_LE,
    IMGFMT_422P16_BE,
    IMGFMT_420P16_LE,
    IMGFMT_420P16_BE,
    IMGFMT_420A,
    IMGFMT_444P,
    IMGFMT_422P,
    IMGFMT_YV12,
    IMGFMT_I420,
    IMGFMT_IYUV,
    IMGFMT_YVU9,
    IMGFMT_IF09,
    IMGFMT_411P,
    IMGFMT_YUY2,
    IMGFMT_UYVY,
// RGB and grayscale (Y8 and Y800):
    IMGFMT_RGB48LE,
    IMGFMT_RGB48BE,
    IMGFMT_BGR32,
    IMGFMT_RGB32,
    IMGFMT_BGR24,
    IMGFMT_RGB24,
    IMGFMT_BGR16,
    IMGFMT_RGB16,
    IMGFMT_BGR15,
    IMGFMT_RGB15,
    IMGFMT_Y800,
    IMGFMT_Y8,
    IMGFMT_BGR8,
    IMGFMT_RGB8,
    IMGFMT_BGR4,
    IMGFMT_RGB4,
    IMGFMT_BG4B,
    IMGFMT_RG4B,
    IMGFMT_BGR1,
    IMGFMT_RGB1,
    0
};

static unsigned int __FASTCALL__ find_best_out(vf_instance_t *vf,unsigned w,unsigned h){
    unsigned int best=0;
    unsigned int i;

    // find the best outfmt:
    for(i=0; i<sizeof(outfmt_list)/sizeof(int)-1; i++){
	const int format= outfmt_list[i];
	int ret= vf->priv->query_format_cache[i]-1;
	if(ret == -1){
	    ret= vf_next_query_format(vf, outfmt_list[i],w,h);
	    vf->priv->query_format_cache[i]= ret+1;
	}

	mpxp_dbg2<<"scale: query("<<vo_format_name(format)<<") -> "<<(ret&3)<<std::endl;
	if(ret&VFCAP_CSP_SUPPORTED_BY_HW){
	    best=format; // no conversion -> bingo!
	    break;
	}
	if(ret&VFCAP_CSP_SUPPORTED && !best)
	    best=format; // best with conversion
    }
    return best;
}

static void __FASTCALL__ print_conf(vf_instance_t* vf)
{
    mpxp_info<<"[vf_scale]: in["<<vf->priv->sw<<"x"<<vf->priv->sh<<","<<vo_format_name(vf->priv->sfmt)
	<<"] -> out["<<vf->priv->w<<"x"<<vf->priv->h<<","<<vo_format_name(vf->priv->ofmt)<<"]"<<std::endl;
}

static void __FASTCALL__ print_conf_fmtcvt(vf_instance_t* vf)
{
    mpxp_info<<"[vf_fmtcvt]: video["<<vf->priv->sw<<"x"<<vf->priv->sh<<"] in["<<vo_format_name(vf->priv->sfmt)
	<<"] -> out["<<vo_format_name(vf->priv->ofmt)<<"]"<<std::endl;
}

static int __FASTCALL__ vf_config(vf_instance_t* vf,
	int width, int height, int d_width, int d_height,
	vo_flags_e flags, unsigned int outfmt){
    unsigned int best=find_best_out(vf,d_width,d_height);
    int vo_flags;
    int int_sws_flags=0;
    SwsFilter *srcFilter, *dstFilter;

    if(!best){
	mpxp_warn<<"SwScale: no supported outfmt found :("<<std::endl;
	return 0;
    }

    vo_flags=vf_next_query_format(vf,best,d_width,d_height);
    mpxp_dbg2<<"vf_scale: "<<vo_flags<<"=vf_next_query_format("<<std::hex<<best
	<<","<<d_width<<","<<d_height<<");"<<std::endl;
    // scaling to dwidth*d_height, if all these TRUE:
    // - option -zoom
    // - no other sw/hw up/down scaling avail.
    // - we're after postproc
    // - user didn't set w:h
    if(!(vo_flags&VFCAP_POSTPROC) && (flags&4) &&
	    vf->priv->w<0 && vf->priv->h<0){	// -zoom
	int x=(vo_flags&VFCAP_SWSCALE) ? 0 : 1;
	if(d_width<width || d_height<height){
	    // downscale!
	    if(vo_flags&VFCAP_HWSCALE_DOWN) x=0;
	} else {
	    // upscale:
	    if(vo_flags&VFCAP_HWSCALE_UP) x=0;
	}
	if(x){
	    // user wants sw scaling! (-zoom)
	    vf->priv->w=d_width;
	    vf->priv->h=d_height;
	}
    }
    vf->priv->sw=width;
    vf->priv->sh=height;
    vf->priv->sfmt=outfmt;
    vf->priv->ofmt=best;
    // calculate the missing parameters:
    switch(best) {
    case IMGFMT_YUY2:		/* YUY2 needs w rounded to 2 */
    case IMGFMT_UYVY:
	if(vf->priv->w==-3) vf->priv->w=(vf->priv->h*width/height+1)&~1; else
	if(vf->priv->w==-2) vf->priv->w=(vf->priv->h*d_width/d_height+1)&~1;
	if(vf->priv->w<0) vf->priv->w=width; else
	if(vf->priv->w==0) vf->priv->w=d_width;
	if(vf->priv->h==-3) vf->priv->h=vf->priv->w*height/width; else
	if(vf->priv->h==-2) vf->priv->h=vf->priv->w*d_height/d_width;
	break;
    case IMGFMT_YV12:		/* YV12 needs w & h rounded to 2 */
    case IMGFMT_I420:
    case IMGFMT_IYUV:
	if(vf->priv->w==-3) vf->priv->w=(vf->priv->h*width/height+1)&~1; else
	if(vf->priv->w==-2) vf->priv->w=(vf->priv->h*d_width/d_height+1)&~1;
	if(vf->priv->w<0) vf->priv->w=width; else
	if(vf->priv->w==0) vf->priv->w=d_width;
	if(vf->priv->h==-3) vf->priv->h=(vf->priv->w*height/width+1)&~1; else
	if(vf->priv->h==-2) vf->priv->h=(vf->priv->w*d_height/d_width+1)&~1;
	break;
    default:
    if(vf->priv->w==-3) vf->priv->w=vf->priv->h*width/height; else
    if(vf->priv->w==-2) vf->priv->w=vf->priv->h*d_width/d_height;
    if(vf->priv->w<0) vf->priv->w=width; else
    if(vf->priv->w==0) vf->priv->w=d_width;
    if(vf->priv->h==-3) vf->priv->h=vf->priv->w*height/width; else
    if(vf->priv->h==-2) vf->priv->h=vf->priv->w*d_height/d_width;
    break;
    }

    if(vf->priv->h<0) vf->priv->h=height; else
    if(vf->priv->h==0) vf->priv->h=d_height;

    // mp_free old ctx:
    if(vf->priv->ctx) sws_freeContext(vf->priv->ctx);
    if(vf->priv->ctx2)sws_freeContext(vf->priv->ctx2);

    // new swscaler:
    sws_getFlagsAndFilterFromCmdLine(&int_sws_flags, &srcFilter, &dstFilter);
    mpxp_dbg2<<"vf_scale: sws_getFlagsAndFilterFromCmdLine(...);"<<std::endl;
    int_sws_flags|= vf->priv->v_chr_drop << SWS_SRC_V_CHR_DROP_SHIFT;

    mpxp_dbg2<<"vf_scale: sws_getContext("<<width<<", "<<(height>>vf->priv->interlaced)
	<<", "<<vo_format_name(outfmt)<<", "<<vf->priv->w<<", "
	<<(vf->priv->h >> vf->priv->interlaced)<<", "<<vo_format_name(best)<<", "<<std::hex<<(int_sws_flags | get_sws_cpuflags() | SWS_PRINT_INFO)<<");"<<std::endl;
    vf->priv->ctx=sws_getContext(width, height >> vf->priv->interlaced,
	    pixfmt_from_fourcc(outfmt),
		  vf->priv->w, vf->priv->h >> vf->priv->interlaced,
	    pixfmt_from_fourcc(best),
	    int_sws_flags | get_sws_cpuflags() | SWS_PRINT_INFO,
	    srcFilter, dstFilter, vf->priv->param);
    if(vf->priv->interlaced){
	vf->priv->ctx2=sws_getContext(width, height >> 1,
	    pixfmt_from_fourcc(outfmt),
		  vf->priv->w, vf->priv->h >> 1,
	    pixfmt_from_fourcc(best),
	    int_sws_flags | get_sws_cpuflags(), srcFilter, dstFilter, vf->priv->param);
    }
    if(!vf->priv->ctx){
	// error...
	mpxp_warn<<"Couldn't init SwScaler for this setup"<<std::endl;
	return 0;
    }
    mpxp_dbg2<<"vf_scale: SwScaler for was inited"<<std::endl;
    vf->priv->fmt=best;

    if(vf->priv->palette){
	delete vf->priv->palette;
	vf->priv->palette=NULL;
    }
    switch(best) {
    case IMGFMT_RGB8: {
      /* set 332 palette for 8 bpp */
	int i;
	vf->priv->palette=new unsigned char [4*256];
	for(i=0; i<256; i++){
	    vf->priv->palette[4*i+0]=4*(i>>6)*21;
	    vf->priv->palette[4*i+1]=4*((i>>3)&7)*9;
	    vf->priv->palette[4*i+2]=4*((i&7)&7)*9;
	    vf->priv->palette[4*i+3]=0;
	}
	break; }
    case IMGFMT_BGR8: {
      /* set 332 palette for 8 bpp */
	int i;
	vf->priv->palette=new unsigned char [4*256];
	for(i=0; i<256; i++){
	    vf->priv->palette[4*i+0]=4*(i&3)*21;
	    vf->priv->palette[4*i+1]=4*((i>>2)&7)*9;
	    vf->priv->palette[4*i+2]=4*((i>>5)&7)*9;
	    vf->priv->palette[4*i+3]=0;
	}
	break; }
    case IMGFMT_BGR4:
    case IMGFMT_BG4B: {
	int i;
	vf->priv->palette=new unsigned char [4*16];
	for(i=0; i<16; i++){
	    vf->priv->palette[4*i+0]=4*(i&1)*63;
	    vf->priv->palette[4*i+1]=4*((i>>1)&3)*21;
	    vf->priv->palette[4*i+2]=4*((i>>3)&1)*63;
	    vf->priv->palette[4*i+3]=0;
	}
	break; }
    case IMGFMT_RGB4:
    case IMGFMT_RG4B: {
	int i;
	vf->priv->palette=new unsigned char [4*16];
	for(i=0; i<16; i++){
	    vf->priv->palette[4*i+0]=4*(i>>3)*63;
	    vf->priv->palette[4*i+1]=4*((i>>1)&3)*21;
	    vf->priv->palette[4*i+2]=4*((i&1)&1)*63;
	    vf->priv->palette[4*i+3]=0;
	}
	break; }
    }

    if(!vo_conf.image_width && !vo_conf.image_height && !(vo_conf.image_zoom >= 0.001)){
	// Compute new d_width and d_height, preserving aspect
	// while ensuring that both are >= output size in pixels.
	if (vf->priv->h * d_width > vf->priv->w * d_height) {
		d_width = vf->priv->h * d_width / d_height;
		d_height = vf->priv->h;
	} else {
		d_height = vf->priv->w * d_height / d_width;
		d_width = vf->priv->w;
	}
	//d_width=d_width*vf->priv->w/width;
	//d_height=d_height*vf->priv->h/height;
    }
    return vf_next_config(vf,vf->priv->w,vf->priv->h,d_width,d_height,flags,best);
}

static void __FASTCALL__ scale(struct SwsContext *sws1, struct SwsContext *sws2, const uint8_t *src[3], unsigned src_stride[3], int y, int h,
		  uint8_t *dst[3], unsigned dst_stride[3], int interlaced){
    if(interlaced){
	int i;
	const uint8_t *src2[3]={src[0], src[1], src[2]};
	uint8_t *dst2[3]={dst[0], dst[1], dst[2]};
	unsigned src_stride2[3]={2*src_stride[0], 2*src_stride[1], 2*src_stride[2]};
	unsigned dst_stride2[3]={2*dst_stride[0], 2*dst_stride[1], 2*dst_stride[2]};

	sws_scale(sws1, src2, (const int*)src_stride2, y>>1, h>>1, dst2, (const int*)dst_stride2);
	for(i=0; i<3; i++){
	    src2[i] += src_stride[i];
	    dst2[i] += dst_stride[i];
	}
	sws_scale(sws2, src2, (const int*)src_stride2, y>>1, h>>1, dst2, (const int*)dst_stride2);
    }else{
	sws_scale(sws1, src, (const int*)src_stride, y, h, dst, (const int*)dst_stride);
    }
}

static int __FASTCALL__ put_frame(vf_instance_t* vf,const mp_image_t& smpi){
    mp_image_t *dmpi;//=smpi.priv;
    const uint8_t *planes[4];
    unsigned stride[4];
    planes[0]=smpi.planes[0];
    stride[0]=smpi.stride[0];
    if(smpi.flags&MP_IMGFLAG_PLANAR){
	  planes[1]=smpi.planes[1];
	  planes[2]=smpi.planes[2];
	  planes[3]=smpi.planes[3];
	  stride[1]=smpi.stride[1];
	  stride[2]=smpi.stride[2];
	  stride[3]=smpi.stride[3];
    }
    mpxp_dbg2<<"vf_scale.put_frame was called"<<std::endl;
    dmpi=vf_get_new_image(vf->next,vf->priv->fmt,
	MP_IMGTYPE_TEMP, MP_IMGFLAG_ACCEPT_STRIDE | MP_IMGFLAG_PREFER_ALIGNED_STRIDE,
	vf->priv->w, vf->priv->h,smpi.xp_idx);
    scale(vf->priv->ctx, vf->priv->ctx2, planes, stride, smpi.y, smpi.h, dmpi->planes, dmpi->stride, vf->priv->interlaced);
    return vf_next_put_slice(vf,*dmpi);
}

static int __FASTCALL__ put_slice(vf_instance_t* vf,const mp_image_t& smpi){
    mp_image_t *dmpi;//=smpi.priv;
    const uint8_t *planes[4];
    uint8_t* dplanes[4];
    unsigned stride[4];
    planes[0]=smpi.planes[0];
    stride[0]=smpi.stride[0];
    if(smpi.flags&MP_IMGFLAG_PLANAR){
	  planes[1]=smpi.planes[1];
	  planes[2]=smpi.planes[2];
	  planes[3]=smpi.planes[3];
	  stride[1]=smpi.stride[1];
	  stride[2]=smpi.stride[2];
	  stride[3]=smpi.stride[3];
    }
    mpxp_dbg2<<"vf_scale.put_slice was called["<<smpi.y<<" "<<smpi.h<<"]"<<std::endl;
    dmpi=vf_get_new_image(vf->next,vf->priv->fmt,
	MP_IMGTYPE_TEMP, MP_IMGFLAG_ACCEPT_STRIDE | MP_IMGFLAG_PREFER_ALIGNED_STRIDE,
	vf->priv->w, vf->priv->h,smpi.xp_idx);
    /* Try to fake first slice*/
    dplanes[0] = dmpi->planes[0];
    if(smpi.flags&MP_IMGFLAG_PLANAR) {
	dplanes[1] = dmpi->planes[1];
	dplanes[2] = dmpi->planes[2];
	dplanes[3] = dmpi->planes[3];
    }
    planes[0]  += smpi.y*smpi.stride[0];
    dplanes[0] += smpi.y*dmpi->stride[0];
    if(smpi.flags&MP_IMGFLAG_PLANAR){
	planes[1] += (smpi.y>>smpi.chroma_y_shift)*smpi.stride[1];
	planes[2] += (smpi.y>>smpi.chroma_y_shift)*smpi.stride[2];
	planes[3] += (smpi.y>>smpi.chroma_y_shift)*smpi.stride[3];
	dplanes[1]+= (smpi.y>>dmpi->chroma_y_shift)*dmpi->stride[0];
	dplanes[2]+= (smpi.y>>dmpi->chroma_y_shift)*dmpi->stride[1];
	dplanes[3]+= (smpi.y>>dmpi->chroma_y_shift)*dmpi->stride[2];
    }
    scale(vf->priv->ctx, vf->priv->ctx2, planes, stride, 0, smpi.h, dplanes, dmpi->stride, vf->priv->interlaced);
    dmpi->y = smpi.y;
    dmpi->h = smpi.h;
    return vf_next_put_slice(vf,*dmpi);
}

static MPXP_Rc __FASTCALL__ control_vf(vf_instance_t* vf, int request, any_t* data){
    int *table;
    int *inv_table;
    int r;
    int brightness, contrast, saturation, srcRange, dstRange;
    vf_equalizer_t *eq;

  if(vf->priv->ctx)
    switch(request){
    case VFCTRL_GET_EQUALIZER:
	r= sws_getColorspaceDetails(vf->priv->ctx, &inv_table, &srcRange, &table, &dstRange, &brightness, &contrast, &saturation);
	if(r<0) break;

	eq = reinterpret_cast<vf_equalizer_t*>(data);
	if (!strcmp(eq->item,"brightness")) {
		eq->value =  ((brightness*100) + (1<<15))>>16;
	}
	else if (!strcmp(eq->item,"contrast")) {
		eq->value = (((contrast  *100) + (1<<15))>>16) - 100;
	}
	else if (!strcmp(eq->item,"saturation")) {
		eq->value = (((saturation*100) + (1<<15))>>16) - 100;
	}
	else
		break;
	return MPXP_True;
    case VFCTRL_SET_EQUALIZER:
	r= sws_getColorspaceDetails(vf->priv->ctx, &inv_table, &srcRange, &table, &dstRange, &brightness, &contrast, &saturation);
	if(r<0) break;
//printf("set %f %f %f\n", brightness/(float)(1<<16), contrast/(float)(1<<16), saturation/(float)(1<<16));
	eq = reinterpret_cast<vf_equalizer_t*>(data);

	if (!strcmp(eq->item,"brightness")) {
		brightness = (( eq->value     <<16) + 50)/100;
	}
	else if (!strcmp(eq->item,"contrast")) {
		contrast   = (((eq->value+100)<<16) + 50)/100;
	}
	else if (!strcmp(eq->item,"saturation")) {
		saturation = (((eq->value+100)<<16) + 50)/100;
	}
	else
		break;

	r= sws_setColorspaceDetails(vf->priv->ctx, inv_table, srcRange, table, dstRange, brightness, contrast, saturation);
	if(r<0) break;
	if(vf->priv->ctx2){
	    r= sws_setColorspaceDetails(vf->priv->ctx2, inv_table, srcRange, table, dstRange, brightness, contrast, saturation);
	    if(r<0) break;
	}

	return MPXP_True;
    default:
	break;
    }

    return vf_next_control(vf,request,data);
}

//===========================================================================//

//  supported Input formats: YV12, I420, IYUV, YUY2, UYVY, BGR32, BGR24, BGR16, BGR15, RGB32, RGB24, Y8, Y800

static int __FASTCALL__ query_format(vf_instance_t* vf, unsigned int fmt,unsigned w,unsigned h){
    mpxp_dbg3<<"vf_scale: query_format("<<std::hex<<fmt<<"("<<vo_format_name(fmt)<<"), "<<w<<", "<<h<<std::endl;
    switch(fmt){
    case IMGFMT_YV12:
    case IMGFMT_I420:
    case IMGFMT_IYUV:
    case IMGFMT_UYVY:
    case IMGFMT_YUY2:
    case IMGFMT_BGR32:
    case IMGFMT_BGR24:
    case IMGFMT_BGR16:
    case IMGFMT_BGR15:
    case IMGFMT_RGB32:
    case IMGFMT_RGB24:
    case IMGFMT_Y800:
    case IMGFMT_Y8:
    case IMGFMT_YVU9:
    case IMGFMT_IF09:
    case IMGFMT_444P:
    case IMGFMT_422P:
    case IMGFMT_411P:
    {
	unsigned int best=find_best_out(vf,w,h);
	int flags;
	if(!best) {
	    mpxp_dbg2<<"[sw_scale] Can't find best out for "<<vo_format_name(fmt)<<std::endl;
	    return 0;	 // no matching out-fmt
	}
	flags=vf_next_query_format(vf,best,w,h);
	if(!(flags&3)) {
	    mpxp_dbg2<<"[sw_scale] Can't find HW support for "<<vo_format_name(best)<<" on "<<vf->next->info->name<<std::endl;
	    return 0; // huh?
	}
	mpxp_dbg3<<"[sw_scale] "<<vo_format_name(best)<<" supported on "<<vf->next->info->name<<" like "<<flags<<std::endl;
	if(fmt!=best) flags&=~VFCAP_CSP_SUPPORTED_BY_HW;
	// do not allow scaling, if we are before the PP fliter!
	if(!(flags&VFCAP_POSTPROC)) flags|=VFCAP_SWSCALE;
	mpxp_dbg3<<"[sw_scale] returning: "<<flags<<std::endl;
	return flags;
      }
    }
    mpxp_dbg2<<"format "<<vo_format_name(fmt)<<" is not supported by sw_scaler"<<std::endl;
    return 0;	// nomatching in-fmt
}

static void __FASTCALL__ uninit(vf_instance_t *vf){
    if(vf->priv->ctx) sws_freeContext(vf->priv->ctx);
    if(vf->priv->ctx2) sws_freeContext(vf->priv->ctx2);
    if(vf->priv->palette) delete vf->priv->palette;
    delete vf->priv;
    firstTime=1;
}

static MPXP_Rc __FASTCALL__ vf_open(vf_instance_t *vf,const char* args){
    vf->config_vf=vf_config;
    vf->put_slice=put_frame;
    vf->query_format=query_format;
    vf->control_vf= control_vf;
    vf->uninit=uninit;
    vf->print_conf=print_conf;
    if(!vf->priv) {
	vf->priv=new(zeromem) vf_priv_t;
	// TODO: parse args ->
	vf->priv->w=
	vf->priv->h=-1;
	vf->priv->param[0]=
	vf->priv->param[1]=SWS_PARAM_DEFAULT;
    } // if(!vf->priv)
    if(args) sscanf(args, "%d:%d:%d:%lf:%lf",
    &vf->priv->w,
    &vf->priv->h,
    &vf->priv->v_chr_drop,
    &vf->priv->param[0],
    &vf->priv->param[1]);
    mpxp_v<<"SwScale params: "<<vf->priv->w<<" x "<<vf->priv->h<<" (-1=no scaling)"<<std::endl;
    if(!mp_conf.verbose) av_log_set_level(AV_LOG_FATAL); /* suppress: slices start in the middle */
    check_pin("vfilter",vf->pin,VF_PIN);
    return MPXP_Ok;
}

static MPXP_Rc __FASTCALL__ vf_open_fmtcvt(vf_instance_t *vf,const char* args){
    MPXP_Rc retval = vf_open(vf,args);
    vf->put_slice=put_slice;
    vf->print_conf=print_conf_fmtcvt;
    check_pin("vfilter",vf->pin,VF_PIN);
    return retval;
}

//global sws_flags from the command line
int sws_flags=2;

//global srcFilter
static SwsFilter *src_filter= NULL;

float sws_lum_gblur= 0.0;
float sws_chr_gblur= 0.0;
int sws_chr_vshift= 0;
int sws_chr_hshift= 0;
float sws_chr_sharpen= 0.0;
float sws_lum_sharpen= 0.0;

int get_sws_cpuflags(){
    int flags=0;
    if(gCpuCaps.hasMMX) flags |= SWS_CPU_CAPS_MMX;
    if(gCpuCaps.hasMMX2) flags |= SWS_CPU_CAPS_MMX2;
    if(gCpuCaps.has3DNow) flags |= SWS_CPU_CAPS_3DNOW;
    return flags;
}

void sws_getFlagsAndFilterFromCmdLine(int *flags, SwsFilter **srcFilterParam, SwsFilter **dstFilterParam)
{
	*flags=0;

	if(firstTime)
	{
		firstTime=0;
		*flags= SWS_PRINT_INFO;
	}
	else if(mp_conf.verbose>1) *flags= SWS_PRINT_INFO;

	if(src_filter) sws_freeFilter(src_filter);

	src_filter= sws_getDefaultFilter(
		sws_lum_gblur, sws_chr_gblur,
		sws_lum_sharpen, sws_chr_sharpen,
		sws_chr_hshift, sws_chr_vshift, mp_conf.verbose>1);

	switch(sws_flags)
	{
		case 0: *flags|= SWS_FAST_BILINEAR; break;
		case 1: *flags|= SWS_BILINEAR; break;
		case 2: *flags|= SWS_BICUBIC; break;
		case 3: *flags|= SWS_X; break;
		case 4: *flags|= SWS_POINT; break;
		case 5: *flags|= SWS_AREA; break;
		case 6: *flags|= SWS_BICUBLIN; break;
		case 7: *flags|= SWS_GAUSS; break;
		case 8: *flags|= SWS_SINC; break;
		case 9: *flags|= SWS_LANCZOS; break;
		case 10:*flags|= SWS_SPLINE; break;
		default:*flags|= SWS_BILINEAR; break;
	}

	*srcFilterParam= src_filter;
	*dstFilterParam= NULL;
}

// will use sws_flags & src_filter (from cmd line)
struct SwsContext *sws_getContextFromCmdLine(int srcW, int srcH, int srcFormat, int dstW, int dstH, int dstFormat)
{
	int flags;
	SwsFilter *dstFilterParam, *srcFilterParam;
	sws_getFlagsAndFilterFromCmdLine(&flags, &srcFilterParam, &dstFilterParam);

	return sws_getContext(srcW, srcH, PixelFormat(srcFormat), dstW, dstH, PixelFormat(dstFormat), flags | get_sws_cpuflags(), srcFilterParam, dstFilterParam, NULL);
}

/* note: slices give unstable performance downgrading */
extern const vf_info_t vf_info_scale = {
    "software scaling",
    "scale",
    "A'rpi",
    "",
    VF_FLAGS_THREADS,
    vf_open
};

/* Just for analisys */
extern const vf_info_t vf_info_fmtcvt = {
    "format converter",
    "fmtcvt",
    "A'rpi",
    "",
    VF_FLAGS_THREADS|VF_FLAGS_SLICES,
    vf_open_fmtcvt
};

//===========================================================================//
