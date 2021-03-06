#include "mpxp_config.h"
#include "osdep/mplib.h"
using namespace	usr;
#define OSD_SUPPORT

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libvo2/img_format.h"
#include "xmpcore/xmp_image.h"
#include "vf.h"
#include "vf_internal.h"

#include "osdep/fastmemcpy.h"

#ifdef OSD_SUPPORT
#include "libvo2/video_out.h"
#include "libvo2/sub.h"
#endif
#include "pp_msg.h"
#include "mplayerxp.h" // mpxp_context().video().output

struct vf_priv_t {
    int up_h,dn_h;
    unsigned w,h;
    int new_frame;
};

//===========================================================================//

static int __FASTCALL__ vf_config(vf_instance_t* vf,
	int width, int height, int d_width, int d_height,
	vo_flags_e flags, unsigned int outfmt){
    unsigned h,dh;
    vf->priv->w=width;
    vf->priv->h=height;
    if(vf->priv->dn_h==-1 || vf->priv->up_h==-1)
	vf->priv->dn_h=vf->priv->up_h=(get_osd_height(mpxp_context().video().output,OSD_PB_START,0)*4)/3;
    h=height+vf->priv->up_h+vf->priv->dn_h;
    dh=d_height+vf->priv->up_h+vf->priv->dn_h;
    return vf_next_config(vf,width,h,d_width,dh,flags,outfmt);
}

static int __FASTCALL__ put_slice(vf_instance_t* vf,const mp_image_t& smpi){
    int finalize;
    mp_image_t *dmpi;
    if(vf->priv->new_frame)
    {
	vf->priv->new_frame = 0;
	dmpi=vf_get_new_temp_genome(vf->next,smpi);
	vf_mpi_clear(dmpi,0,0,vf->priv->w,vf->priv->up_h);
	dmpi=vf_get_new_image(vf->next,smpi.imgfmt,
	    MP_IMGTYPE_TEMP, MP_IMGFLAG_ACCEPT_STRIDE,
	    vf->priv->w,vf->priv->up_h+vf->priv->h+vf->priv->dn_h,smpi.xp_idx);
	vf_mpi_clear(dmpi,0,0,vf->priv->w,vf->priv->dn_h);
    }
    dmpi=vf_get_new_image(vf->next,smpi.imgfmt,
	    MP_IMGTYPE_TEMP, MP_IMGFLAG_ACCEPT_STRIDE,
	    smpi.w,smpi.h+vf->priv->up_h,smpi.xp_idx);
    finalize = dmpi->flags&MP_IMGFLAG_FINALIZED;
    // copy smpi.dmpi...
    if(smpi.flags&MP_IMGFLAG_PLANAR){
	if(finalize) {
	stream_copy_pic(dmpi->planes[0],
		smpi.planes[0], smpi.w, smpi.h,
		dmpi->stride[0],smpi.stride[0]);
	stream_copy_pic(dmpi->planes[1],
		smpi.planes[1], smpi.chroma_width, smpi.chroma_height,
		dmpi->stride[1],smpi.stride[1]);
	stream_copy_pic(dmpi->planes[2],
		smpi.planes[2], smpi.chroma_width, smpi.chroma_height,
		dmpi->stride[2],smpi.stride[2]);
	} else {
	memcpy_pic(dmpi->planes[0],
		smpi.planes[0], smpi.w, smpi.h,
		dmpi->stride[0],smpi.stride[0]);
	memcpy_pic(dmpi->planes[1],
		smpi.planes[1], smpi.chroma_width, smpi.chroma_height,
		dmpi->stride[1],smpi.stride[1]);
	memcpy_pic(dmpi->planes[2],
		smpi.planes[2], smpi.chroma_width, smpi.chroma_height,
		dmpi->stride[2],smpi.stride[2]);
	}
    } else {
	if(finalize) {
	stream_copy_pic(dmpi->planes[0],
		smpi.planes[0], smpi.w*(dmpi->bpp/8), smpi.h,
		dmpi->stride[0],smpi.stride[0]);
	} else {
	memcpy_pic(dmpi->planes[0],
		smpi.planes[0], smpi.w*(dmpi->bpp/8), smpi.h,
		dmpi->stride[0],smpi.stride[0]);
	}
	dmpi->planes[1] = smpi.planes[1]; // passthrough rgb8 palette
    }
    return vf_next_put_slice(vf,*dmpi);
}

//===========================================================================//

static MPXP_Rc __FASTCALL__ control_vf(vf_instance_t* vf, int request, any_t* data){
    switch(request){
    case VFCTRL_START_FRAME:
	vf->priv->new_frame=1;
	break;
    }
    return vf_next_control(vf,request,data);
}

static int __FASTCALL__ query_format(vf_instance_t* vf, unsigned int fmt,unsigned w,unsigned h){
    return vf_next_query_format(vf->next,fmt,w,h);
}

static MPXP_Rc __FASTCALL__ vf_open(vf_instance_t *vf,const char* args){
    vf->config_vf=vf_config;
    vf->control_vf=control_vf;
    vf->query_format=query_format;
    vf->put_slice=put_slice;
    if(!vf->priv) vf->priv=new(zeromem) vf_priv_t;
    if(args) sscanf(args,"%d:%d",&vf->priv->up_h,&vf->priv->dn_h);
    else     vf->priv->up_h=vf->priv->dn_h=-1;
    check_pin("vfilter",vf->pin,VF_PIN);
    return MPXP_Ok;
}

extern const vf_info_t vf_info_expand = {
    "expanding for osd",
    "expand",
    "A'rpi",
    "",
    VF_FLAGS_THREADS,
    vf_open
};

//===========================================================================//
