#include "mpxp_config.h"
#include "osdep/mplib.h"
using namespace	usr;
/*
    Copyright (C) 2002 Michael Niedermayer <michaelni@gmx.at>

    This program is mp_free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#include <math.h>

#include "libvo2/img_format.h"
#include "xmpcore/xmp_image.h"
#include "vf.h"
#include "vf_internal.h"
#include "osdep/fastmemcpy.h"
#include "pp_msg.h"

#define SUB_PIXEL_BITS 8
#define SUB_PIXELS (1<<SUB_PIXEL_BITS)
#define COEFF_BITS 11

//===========================================================================//

struct vf_priv_t {
	double ref[4][2];
	int32_t coeff[1<<SUB_PIXEL_BITS][4];
	int32_t (*pv)[2];
	int pvStride;
	int cubic;
};


/***************************************************************************/

static void __FASTCALL__ initPv(vf_priv_t *priv, int W, int H){
	double a,b,c,d,e,f,g,h,D;
	double (*ref)[2]= priv->ref;
	int x,y;

	g= (  (ref[0][0] - ref[1][0] - ref[2][0] + ref[3][0])*(ref[2][1] - ref[3][1])
	    - (ref[0][1] - ref[1][1] - ref[2][1] + ref[3][1])*(ref[2][0] - ref[3][0]))*H;
	h= (  (ref[0][1] - ref[1][1] - ref[2][1] + ref[3][1])*(ref[1][0] - ref[3][0])
	    - (ref[0][0] - ref[1][0] - ref[2][0] + ref[3][0])*(ref[1][1] - ref[3][1]))*W;
	D=   (ref[1][0] - ref[3][0])*(ref[2][1] - ref[3][1])
	   - (ref[2][0] - ref[3][0])*(ref[1][1] - ref[3][1]);

	a= D*(ref[1][0] - ref[0][0])*H + g*ref[1][0];
	b= D*(ref[2][0] - ref[0][0])*W + h*ref[2][0];
	c= D*ref[0][0]*W*H;
	d= D*(ref[1][1] - ref[0][1])*H + g*ref[1][1];
	e= D*(ref[2][1] - ref[0][1])*W + h*ref[2][1];
	f= D*ref[0][1]*W*H;

	for(y=0; y<H; y++){
		for(x=0; x<W; x++){
			int u, v;

			u= (int)floor( SUB_PIXELS*(a*x + b*y + c)/(g*x + h*y + D*W*H) + 0.5);
			v= (int)floor( SUB_PIXELS*(d*x + e*y + f)/(g*x + h*y + D*W*H) + 0.5);

			priv->pv[x + y*W][0]= u;
			priv->pv[x + y*W][1]= v;
		}
	}
}

static double __FASTCALL__ getCoeff(double d){
	double A= -0.60;
	double coeff;

	d= fabs(d);

	// Equation is from VirtualDub
	if(d<1.0)
		coeff = (1.0 - (A+3.0)*d*d + (A+2.0)*d*d*d);
	else if(d<2.0)
		coeff = (-4.0*A + 8.0*A*d - 5.0*A*d*d + A*d*d*d);
	else
		coeff=0.0;

	return coeff;
}

static int __FASTCALL__ vf_config(vf_instance_t* vf,
	int width, int height, int d_width, int d_height,
	vo_flags_e flags, unsigned int outfmt){
	int i, j;

	vf->priv->pvStride= width;
	vf->priv->pv= reinterpret_cast<int32_t (*)[2]>(mp_memalign(8, width*height*2*sizeof(int32_t)));
	initPv(vf->priv, width, height);

	for(i=0; i<SUB_PIXELS; i++){
		double d= i/(double)SUB_PIXELS;
		double temp[4];
		double sum=0;

		for(j=0; j<4; j++)
			temp[j]= getCoeff(j - d - 1);

		for(j=0; j<4; j++)
			sum+= temp[j];

		for(j=0; j<4; j++)
			vf->priv->coeff[i][j]= (int)floor((1<<COEFF_BITS)*temp[j]/sum + 0.5);
	}

	return vf_next_config(vf,width,height,d_width,d_height,flags,outfmt);
}

static void __FASTCALL__ uninit(vf_instance_t* vf){
	if(!vf->priv) return;

	if(vf->priv->pv) delete vf->priv->pv;
	vf->priv->pv= NULL;

	delete vf->priv;
	vf->priv=NULL;
}

static inline void resampleCubic(uint8_t *dst, uint8_t *src, int w, int h, int dstStride, int srcStride, vf_priv_t *privParam, int xShift, int yShift){
	int x, y;
	vf_priv_t priv= *privParam;

	for(y=0; y<h; y++){
		for(x=0; x<w; x++){
			int u, v, subU, subV, sum, sx, sy;

			sx= x << xShift;
			sy= y << yShift;
			u= priv.pv[sx + sy*priv.pvStride][0]>>xShift;
			v= priv.pv[sx + sy*priv.pvStride][1]>>yShift;
			subU= u & (SUB_PIXELS-1);
			subV= v & (SUB_PIXELS-1);
			u >>= SUB_PIXEL_BITS;
			v >>= SUB_PIXEL_BITS;

			if(u>0 && v>0 && u<w-2 && v<h-2){
				const int index= u + v*srcStride;
				const int a= priv.coeff[subU][0];
				const int b= priv.coeff[subU][1];
				const int c= priv.coeff[subU][2];
				const int d= priv.coeff[subU][3];

				sum=
				 priv.coeff[subV][0]*(  a*src[index - 1 - srcStride] + b*src[index - 0 - srcStride]
						      + c*src[index + 1 - srcStride] + d*src[index + 2 - srcStride])
				+priv.coeff[subV][1]*(  a*src[index - 1            ] + b*src[index - 0            ]
						      + c*src[index + 1            ] + d*src[index + 2            ])
				+priv.coeff[subV][2]*(  a*src[index - 1 + srcStride] + b*src[index - 0 + srcStride]
						      + c*src[index + 1 + srcStride] + d*src[index + 2 + srcStride])
				+priv.coeff[subV][3]*(  a*src[index - 1+2*srcStride] + b*src[index - 0+2*srcStride]
						      + c*src[index + 1+2*srcStride] + d*src[index + 2+2*srcStride]);
			}else{
				int dx, dy;
				sum=0;

				for(dy=0; dy<4; dy++){
					int iy= v + dy - 1;
					if     (iy< 0) iy=0;
					else if(iy>=h) iy=h-1;
					for(dx=0; dx<4; dx++){
						int ix= u + dx - 1;
						if     (ix< 0) ix=0;
						else if(ix>=w) ix=w-1;

						sum+=  priv.coeff[subU][dx]*priv.coeff[subV][dy]
						      *src[ ix + iy*srcStride];
					}
				}
			}
			sum= (sum + (1<<(COEFF_BITS*2-1)) ) >> (COEFF_BITS*2);
			if(sum&~255){
				if(sum<0) sum=0;
				else      sum=255;
			}
			dst[ x + y*dstStride]= sum;
		}
	}
}

static inline void resampleLinear(uint8_t *dst, uint8_t *src, int w, int h, int dstStride, int srcStride,
				  vf_priv_t *privParam, int xShift, int yShift){
	int x, y;
	vf_priv_t priv= *privParam;

	for(y=0; y<h; y++){
		for(x=0; x<w; x++){
			int u, v, subU, subV, sum, sx, sy, index, subUI, subVI;

			sx= x << xShift;
			sy= y << yShift;
			u= priv.pv[sx + sy*priv.pvStride][0]>>xShift;
			v= priv.pv[sx + sy*priv.pvStride][1]>>yShift;
			subU= u & (SUB_PIXELS-1);
			subV= v & (SUB_PIXELS-1);
			u >>= SUB_PIXEL_BITS;
			v >>= SUB_PIXEL_BITS;
			index= u + v*srcStride;
			subUI= SUB_PIXELS - subU;
			subVI= SUB_PIXELS - subV;

			if((unsigned)u < (unsigned)(w - 1)){
				if((unsigned)v < (unsigned)(h - 1)){
					sum= subVI*(subUI*src[index          ] + subU*src[index          +1])
					    +subV *(subUI*src[index+srcStride] + subU*src[index+srcStride+1]);
					sum= (sum + (1<<(SUB_PIXEL_BITS*2-1)) ) >> (SUB_PIXEL_BITS*2);
				}else{
					if(v<0) v= 0;
					else    v= h-1;
					index= u + v*srcStride;
					sum= subUI*src[index] + subU*src[index+1];
					sum= (sum + (1<<(SUB_PIXEL_BITS-1)) ) >> SUB_PIXEL_BITS;
				}
			}else{
				if((unsigned)v < (unsigned)(h - 1)){
					if(u<0) u= 0;
					else    u= w-1;
					index= u + v*srcStride;
					sum= subVI*src[index] + subV*src[index+srcStride];
					sum= (sum + (1<<(SUB_PIXEL_BITS-1)) ) >> SUB_PIXEL_BITS;
				}else{
					if(u<0) u= 0;
					else    u= w-1;
					if(v<0) v= 0;
					else    v= h-1;
					index= u + v*srcStride;
					sum= src[index];
				}
			}
			if(sum&~255){
				if(sum<0) sum=0;
				else      sum=255;
			}
			dst[ x + y*dstStride]= sum;
		}
	}
}

static int __FASTCALL__ put_slice(vf_instance_t* vf,const mp_image_t& smpi){
	int cw= smpi.w >> smpi.chroma_x_shift;
	int ch= smpi.h >> smpi.chroma_y_shift;

	mp_image_t *dmpi=vf_get_new_temp_genome(vf->next,smpi);

	assert(smpi.flags&MP_IMGFLAG_PLANAR);

	if(vf->priv->cubic){
#ifdef _OPENMP
#pragma omp parallel sections
{
#pragma omp section
#endif
		resampleCubic(dmpi->planes[0], smpi.planes[0], smpi.w,smpi.h, dmpi->stride[0], smpi.stride[0],
				vf->priv, 0, 0);
#ifdef _OPENMP
#pragma omp section
#endif
		resampleCubic(dmpi->planes[1], smpi.planes[1], cw    , ch   , dmpi->stride[1], smpi.stride[1],
				vf->priv, smpi.chroma_x_shift, smpi.chroma_y_shift);
#ifdef _OPENMP
#pragma omp section
#endif
		resampleCubic(dmpi->planes[2], smpi.planes[2], cw    , ch   , dmpi->stride[2], smpi.stride[2],
				vf->priv, smpi.chroma_x_shift, smpi.chroma_y_shift);
#ifdef _OPENMP
}
#endif
	}else{
#ifdef _OPENMP
#pragma omp parallel sections
{
#pragma omp section
#endif
		resampleLinear(dmpi->planes[0], smpi.planes[0], smpi.w,smpi.h, dmpi->stride[0], smpi.stride[0],
				vf->priv, 0, 0);
#ifdef _OPENMP
#pragma omp section
#endif
		resampleLinear(dmpi->planes[1], smpi.planes[1], cw    , ch   , dmpi->stride[1], smpi.stride[1],
				vf->priv, smpi.chroma_x_shift, smpi.chroma_y_shift);
#ifdef _OPENMP
#pragma omp section
#endif
		resampleLinear(dmpi->planes[2], smpi.planes[2], cw    , ch   , dmpi->stride[2], smpi.stride[2],
				vf->priv, smpi.chroma_x_shift, smpi.chroma_y_shift);
#ifdef _OPENMP
}
#endif
	}

	return vf_next_put_slice(vf,*dmpi);
}

//===========================================================================//

static int __FASTCALL__ query_format(vf_instance_t* vf, unsigned int fmt,unsigned w,unsigned h){
	switch(fmt)
	{
	case IMGFMT_YV12:
	case IMGFMT_I420:
	case IMGFMT_IYUV:
	case IMGFMT_YVU9:
	case IMGFMT_444P:
	case IMGFMT_422P:
	case IMGFMT_411P:
		return vf_next_query_format(vf, fmt,w,h);
	}
	return 0;
}

static MPXP_Rc __FASTCALL__ vf_open(vf_instance_t *vf,const char* args){
	int e;

	vf->config_vf=vf_config;
	vf->put_slice=put_slice;
	vf->query_format=query_format;
	vf->uninit=uninit;
	vf->priv=new(zeromem) vf_priv_t;

	if(args==NULL) return MPXP_False;

	e=sscanf(args, "%lf:%lf:%lf:%lf:%lf:%lf:%lf:%lf:%d",
		&vf->priv->ref[0][0], &vf->priv->ref[0][1],
		&vf->priv->ref[1][0], &vf->priv->ref[1][1],
		&vf->priv->ref[2][0], &vf->priv->ref[2][1],
		&vf->priv->ref[3][0], &vf->priv->ref[3][1],
		&vf->priv->cubic
		);

    if(e!=9)
	return MPXP_False;
    check_pin("vfilter",vf->pin,VF_PIN);
    return MPXP_Ok;
}

extern const vf_info_t vf_info_perspective = {
    "perspective correcture",
    "perspective",
    "Michael Niedermayer",
    "",
    VF_FLAGS_THREADS,
    vf_open
};

//===========================================================================//
