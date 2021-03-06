#include "mpxp_config.h"
#include "osdep/mplib.h"
using namespace	usr;
/*
    Only a sample!
*/
#ifdef USE_TV

#include <stdio.h>
#include "libvo2/img_format.h"
#include "tv.h"
#include "tvi_def.h"

namespace	usr {
/* information about this file */
static const tvi_info_t info = {
	"NULL-TV",
	"dummy",
	"alex",
	NULL
};

/* private data's */
struct priv_t {
    int width;
    int height;
};

#include "tvi_def.h"

/* handler creator - entry point ! */
tvi_handle_t *tvi_init_dummy(const char *device)
{
    UNUSED(device);
    return new_handle();
}

/* initialisation */
static int init(priv_t *priv)
{
    priv->width = 320;
    priv->height = 200;
    return 1;
}

/* that's the real start, we'got the format parameters (checked with control) */
static int start(priv_t *priv)
{
    UNUSED(priv);
    return 1;
}

static int uninit(priv_t *priv)
{
    UNUSED(priv);
    return 1;
}

static int control(priv_t *priv, int cmd, any_t*arg)
{
    switch(cmd)
    {
	case TVI_CONTROL_IS_VIDEO:
	    return TVI_CONTROL_TRUE;
	case TVI_CONTROL_VID_GET_FORMAT:
	    *(int *)arg = IMGFMT_YV12;
	    return TVI_CONTROL_TRUE;
	case TVI_CONTROL_VID_SET_FORMAT:
	{
	    int req_fmt = (long)*(any_t**)arg;
	    if (req_fmt != IMGFMT_YV12)
		return TVI_CONTROL_FALSE;
	    return TVI_CONTROL_TRUE;
	}
	case TVI_CONTROL_VID_SET_WIDTH:
	    priv->width = (long)*(any_t**)arg;
	    return TVI_CONTROL_TRUE;
	case TVI_CONTROL_VID_GET_WIDTH:
	    *(int *)arg = priv->width;
	    return TVI_CONTROL_TRUE;
	case TVI_CONTROL_VID_SET_HEIGHT:
	    priv->height = (long)*(any_t**)arg;
	    return TVI_CONTROL_TRUE;
	case TVI_CONTROL_VID_GET_HEIGHT:
	    *(int *)arg = priv->height;
	    return TVI_CONTROL_TRUE;
	case TVI_CONTROL_VID_CHK_WIDTH:
	case TVI_CONTROL_VID_CHK_HEIGHT:
	    return TVI_CONTROL_TRUE;
    }
    return TVI_CONTROL_UNKNOWN;
}

#ifdef HAVE_TV_BSDBT848
static double grabimmediate_video_frame(priv_t *priv,unsigned char *buffer, int len)
{
    UNUSED(priv);
    memset(buffer, 0xCC, len);
    return 1;
}
#endif

static double grab_video_frame(priv_t *priv,unsigned char *buffer, int len)
{
    UNUSED(priv);
    memset(buffer, 0x42, len);
    return 1;
}

static int get_video_framesize(priv_t *priv)
{
    /* YV12 */
    return priv->width*priv->height*12/8;
}

static double grab_audio_frame(priv_t *priv,unsigned char *buffer, int len)
{
    UNUSED(priv);
    memset(buffer, 0x42, len);
    return 1;
}

static int get_audio_framesize(priv_t *priv)
{
    UNUSED(priv);
    return 1;
}

#endif /* USE_TV */

tvi_handle_t *new_handle()
{
    tvi_handle_t *h = new tvi_handle_t;

    if (!h)
	return NULL;
    h->priv = new(zeromem) priv_t;
    if (!h->priv) {
	delete h;
	return NULL;
    }
    h->info = &info;
    h->functions = &functions;
    h->seq = 0;
    h->chanlist = -1;
    h->chanlist_s = NULL;
    h->norm = -1;
    h->channel = -1;
    return h;
}

void free_handle(tvi_handle_t *h)
{
    if (h) {
	if (h->priv)
	    delete h->priv;
	delete h;
    }
}
} // namespace	usr