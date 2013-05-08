#include "mpxp_config.h"
#include "osdep/mplib.h"
using namespace	usr;
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "libvo2/img_format.h"
#include "xmpcore/xmp_image.h"
#include "vf.h"
#include "vf_internal.h"

#include "osdep/fastmemcpy.h"
#include "mpxp_conf_lavc.h"
#include "vf_scale.h"
#include "pp_msg.h"
//===========================================================================//

static int __FASTCALL__ vf_config(vf_instance_t* vf,
	int width, int height, int d_width, int d_height,
	vo_flags_e flags, unsigned int outfmt){

    sws_rgb2rgb_init();

    if(vf_next_query_format(vf,IMGFMT_YUY2,d_width,d_height)<=0){
	MSG_ERR("yuy2 isn't supported by next filter/vo :(\n");
	return 0;
    }

    return vf_next_config(vf,width,height,d_width,d_height,flags,IMGFMT_YUY2);
}

static int __FASTCALL__ put_slice(vf_instance_t* vf, mp_image_t *mpi){
    mp_image_t *dmpi;

    // hope we'll get DR buffer:
    dmpi=vf_get_new_image(vf->next,IMGFMT_YUY2,
	MP_IMGTYPE_TEMP, MP_IMGFLAG_ACCEPT_STRIDE,
	mpi->w, mpi->h,mpi->xp_idx);

    if(mpi->imgfmt==IMGFMT_422P)
    yuv422ptoyuy2(mpi->planes[0],mpi->planes[1],mpi->planes[2], dmpi->planes[0],
	    mpi->w,mpi->h, mpi->stride[0],mpi->stride[1],dmpi->stride[0]);
    else
    yv12toyuy2(mpi->planes[0],mpi->planes[1],mpi->planes[2], dmpi->planes[0],
	    mpi->w,mpi->h, mpi->stride[0],mpi->stride[1],dmpi->stride[0]);

    vf_clone_mpi_attributes(dmpi, mpi);

    return vf_next_put_slice(vf,dmpi);
}

//===========================================================================//

static int __FASTCALL__ query_format(vf_instance_t* vf, unsigned int fmt,unsigned w,unsigned h){
    switch(fmt){
    case IMGFMT_YV12:
    case IMGFMT_I420:
    case IMGFMT_IYUV:
    case IMGFMT_422P:
	return vf_next_query_format(vf,IMGFMT_YUY2,w,h) & (~VFCAP_CSP_SUPPORTED_BY_HW);
    }
    return 0;
}

static MPXP_Rc __FASTCALL__ vf_open(vf_instance_t *vf,const char* args){
    vf->config_vf=vf_config;
    vf->put_slice=put_slice;
    vf->query_format=query_format;
    check_pin("vfilter",vf->pin,VF_PIN);
    return MPXP_Ok;
}

extern const vf_info_t vf_info_yuy2 = {
    "fast YV12/Y422p -> YUY2 conversion",
    "yuy2",
    "A'rpi",
    "",
    VF_FLAGS_THREADS,
    vf_open
};

//===========================================================================//
