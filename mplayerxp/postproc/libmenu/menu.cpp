#include "mpxp_config.h"
#include "osdep/mplib.h"
using namespace	usr;
#include <algorithm>

#include "mpxp_help.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "libvo2/osd_render.h"
#include "libvo2/font_load.h"
#include "libvo2/sub.h"
#include "osdep/keycodes.h"
#include "libplaytree2/asxparser.h"
#include "nls/nls.h"

#include "libvo2/img_format.h"
#include "libvo2/video_out.h"
#include "xmpcore/xmp_image.h"
#include "libmpconf/m_option.h"
#include "libmpconf/m_struct.h"
#include "menu.h"
#include "pp_msg.h"

#include "mplayerxp.h" // mpxp_context().video().output

extern menu_info_t menu_info_cmdlist;
extern menu_info_t menu_info_pt;
extern menu_info_t menu_info_filesel;
extern menu_info_t menu_info_txt;
extern menu_info_t menu_info_console;
extern menu_info_t menu_info_pref;


menu_info_t* menu_info_list[] = {
  &menu_info_pt,
  &menu_info_cmdlist,
  &menu_info_filesel,
  &menu_info_txt,
  &menu_info_console,
  &menu_info_pref,
  NULL
};

typedef struct menu_def_st {
  const char* name;
  menu_info_t* type;
  any_t* cfg;
  const char* args;
} menu_def_t;

static struct MPContext *menu_ctx = NULL;
static menu_def_t* menu_list = NULL;
static int menu_count = 0;


static int menu_parse_config(const std::string& _buffer) {
  std::string name;
  ASX_Element element;
  menu_info_t* minfo = NULL;
  int r,i;
  ASX_Parser& parser = *new(zeromem) ASX_Parser;
  const char* buffer = _buffer.c_str();

  while(1) {
    r = parser.get_element(&buffer,element);
    if(r < 0) {
      mpxp_warn<<"[libmenu] Syntax error at line: "<<parser.get_line()<<std::endl;
      delete &parser;
      return 0;
    } else if(r == 0) {
      delete &parser;
      return 1;
    }
    // Has it a name ?
    name = element.attribs().get("name");
    if(name.empty()) {
      mpxp_warn<<"[libmenu] Menu definitions need a name attrib: "<<parser.get_line()<<std::endl;
      continue;
    }

    // Try to find this menu type in our list
    for(i = 0, minfo = NULL ; menu_info_list[i] ; i++) {
      if(strcasecmp(element.name().c_str(),menu_info_list[i]->name) == 0) {
	minfo = menu_info_list[i];
	break;
      }
    }
    // Got it : add this to our list
    if(minfo) {
	menu_list = (menu_def_t*)mp_realloc(menu_list,(menu_count+2)*sizeof(menu_def_t));
	menu_list[menu_count].name = mp_strdup(name.c_str());
	menu_list[menu_count].type = minfo;
	menu_list[menu_count].cfg = m_struct_alloc(minfo->priv_st);
	menu_list[menu_count].args = mp_strdup(element.body().c_str());
	std::map<std::string,std::string,ASX_Attrib::stricomp>::iterator it;
	std::map<std::string,std::string,ASX_Attrib::stricomp>& _map = element.attribs().map();
	for(it=_map.begin();it!=_map.end();it++) {
	    std::string sfirst,ssecond;
	    sfirst=(*it).first;
	    ssecond=(*it).second;
	    if(strcasecmp(sfirst.c_str(),"name") == 0) continue;
	    // Setup the attribs
	    if(!m_struct_set(minfo->priv_st,menu_list[menu_count].cfg,mp_strdup(sfirst.c_str()),mp_strdup(ssecond.c_str())))
		mpxp_warn<<"[libmenu] Bad attrib: "<<sfirst<<" "<<ssecond<<" "<<name<<" "<<parser.get_line()<<std::endl;
	}
	menu_count++;
	memset(&menu_list[menu_count],0,sizeof(menu_def_t));
    } else {
	mpxp_warn<<"[libmenu] Unknown menu type: "<<element.name()<<" "<<parser.get_line()<<std::endl;
    }
  }
  delete &parser;
  return 0;
}

/// This will build the menu_defs list from the cfg file
static const int BUF_STEP=1024;
static const int BUF_MIN=128;
static const int BUF_MAX=BUF_STEP*1024;
int menu_init(struct MPContext *mpctx,const std::string& cfg_file) {
  char* buffer = NULL;
  int bl = BUF_STEP, br = 0;
  int f, fd;
#ifndef HAVE_FREETYPE
  if(mpxp_context().video().output->font == NULL)
    return 0;
#endif
  fd = ::open(cfg_file.c_str(), O_RDONLY);
  if(fd < 0) {
    mpxp_warn<<"[libmenu] Can't open ConfigFile: "<<cfg_file<<std::endl;
    return 0;
  }
  buffer = new char [bl];
  while(1) {
    int r;
    if(bl - br < BUF_MIN) {
      if(bl >= BUF_MAX) {
	mpxp_warn<<"[libmenu] ConfigFile is too big: "<<(BUF_MAX/1024)<<std::endl;
	::close(fd);
	delete buffer;
	return 0;
      }
      bl += BUF_STEP;
      buffer = (char *)mp_realloc(buffer,bl);
    }
    r = ::read(fd,buffer+br,bl-br);
    if(r == 0) break;
    br += r;
  }
  if(!br) {
    mpxp_warn<<"[libmenu] ConfigFile is empty"<<std::endl;
    return 0;
  }
  buffer[br-1] = '\0';

  close(fd);

  menu_ctx = mpctx;
  f = menu_parse_config(buffer);
  delete buffer;
  return f;
}

// Destroy all this stuff
void menu_uninit(void) {
  int i;
  for(i = 0 ; menu_list && menu_list[i].name ; i++) {
    delete menu_list[i].name;
    m_struct_free(menu_list[i].type->priv_st,menu_list[i].cfg);
    if(menu_list[i].args) delete menu_list[i].args;
  }
  delete menu_list;
  menu_count = 0;
}

/// Default read_key function
void menu_dflt_read_key(menu_t* menu,int cmd) {
  switch(cmd) {
  case KEY_UP:
    menu->read_cmd(menu,MENU_CMD_UP);
    break;
  case KEY_DOWN:
    menu->read_cmd(menu,MENU_CMD_DOWN);
    break;
  case KEY_LEFT:
    menu->read_cmd(menu,MENU_CMD_LEFT);
    break;
  case KEY_ESC:
    menu->read_cmd(menu,MENU_CMD_CANCEL);
    break;
  case KEY_RIGHT:
    menu->read_cmd(menu,MENU_CMD_RIGHT);
    break;
  case KEY_ENTER:
    menu->read_cmd(menu,MENU_CMD_OK);
    break;
  }
}

menu_t* menu_open(const std::string& name,libinput_t& libinput) {
  menu_t* m;
  int i;

  if(menu_list == NULL) {
    mpxp_warn<<"[libmenu] Menu was not initialized"<<std::endl;
    return NULL;
  }
  if(name.empty()) {
    mpxp_warn<<"[libmenu] Name of menu was not specified"<<std::endl;
    return NULL;
  }
  if(name=="help") {
    for(i = 0 ; menu_list[i].name != NULL ; i++)
	mpxp_info<<"[libmenu] menu entry["<<i<<"]: "<<menu_list[i].name<<std::endl;
    return NULL;
  }
  for(i = 0 ; menu_list[i].name != NULL ; i++) {
    if(name==menu_list[i].name) break;
  }
  if(menu_list[i].name == NULL) {
    mpxp_warn<<"[libmenu] Menu not found: "<<name<<std::endl;
    return NULL;
  }
  m = new(zeromem) menu_t(libinput);
  m->priv_st = menu_list[i].type->priv_st;
  m->priv = (menu_priv_s*)m_struct_copy(m->priv_st,menu_list[i].cfg);
  m->ctx = menu_ctx;
  if(menu_list[i].type->mopen(m,menu_list[i].args))
    return m;
  if(m->priv)
    m_struct_free(m->priv_st,m->priv);
  delete m;
  mpxp_warn<<"[libmenu] Menu init failed: "<<name<<std::endl;
  return NULL;
}

void menu_draw(menu_t* menu,const mp_image_t& mpi) {
  if(menu->show && menu->draw)
    menu->draw(menu,mpi);
}

void menu_read_cmd(menu_t* menu,int cmd) {
  if(menu->read_cmd)
    menu->read_cmd(menu,cmd);
}

void menu_close(menu_t* menu) {
  if(menu->close)
    menu->close(menu);
  if(menu->priv)
    m_struct_free(menu->priv_st,menu->priv);
  delete menu;
}

void menu_read_key(menu_t* menu,int cmd) {
  if(menu->read_key)
    menu->read_key(menu,cmd);
  else
    menu_dflt_read_key(menu,cmd);
}

///////////////////////////// Helpers ////////////////////////////////////

// return the real height of a char:
static inline int get_height(int c,int h){
    int font;
    if ((font=mpxp_context().video().output->font->font[c])>=0)
	if(h<mpxp_context().video().output->font->pic_a[font]->h) h=mpxp_context().video().output->font->pic_a[font]->h;
    return h;
}

static void render_txt(const char *txt)
{
#warning FIXME: render_one_glyph is not implented yet!
#if 0
  while (*txt) {
    int c = utf8_get_char((const char**)&txt);
    render_one_glyph(mpxp_context().video().output->font, c);
  }
#else
  UNUSED(txt);
#endif
}

#ifdef USE_FRIBIDI
#include <fribidi/fribidi.h>
#include "libavutil/common.h"
char *menu_fribidi_charset = NULL;
int menu_flip_hebrew = 0;
int menu_fribidi_flip_commas = 0;

static char *menu_fribidi(char *txt)
{
  static int char_set_num = -1;
  static FriBidiChar *logical, *visual;
  static size_t buffer_size = 1024;
  static char *outputstr;

  FriBidiCharType base;
  fribidi_boolean log2vis;
  size_t len;

  if (menu_flip_hebrew) {
    len = strlen(txt);
    if (char_set_num == -1) {
      fribidi_set_mirroring (1);
      fribidi_set_reorder_nsm (0);
      char_set_num = fribidi_parse_charset("UTF-8");
      buffer_size = std::max(1024,len+1);
      logical = mp_malloc(buffer_size);
      visual = mp_malloc(buffer_size);
      outputstr = new char [buffer_size];
    } else if (len+1 > buffer_size) {
      buffer_size = len+1;
      logical = mp_realloc(logical, buffer_size);
      visual = mp_realloc(visual, buffer_size);
      outputstr = mp_realloc(outputstr, buffer_size);
    }
    len = fribidi_charset_to_unicode (char_set_num, txt, len, logical);
    base = menu_fribidi_flip_commas?FRIBIDI_TYPE_ON:FRIBIDI_TYPE_L;
    log2vis = fribidi_log2vis (logical, len, &base, visual, NULL, NULL, NULL);
    if (log2vis) {
      len = fribidi_remove_bidi_marks (visual, len, NULL, NULL, NULL);
      fribidi_unicode_to_charset (char_set_num, visual, len, outputstr);
      return outputstr;
    }
  }
  return txt;
}
#endif

void menu_draw_text(const mp_image_t& mpi,const std::string& _txt, int x, int y) {
  const char* txt = _txt.c_str();
  const LocalPtr<OSD_Render> draw_alpha(new(zeromem) OSD_Render(mpi.imgfmt));
  int font;
  int finalize=mpxp_context().video().output->is_final();

#ifdef USE_FRIBIDI
  txt = menu_fribidi(txt);
#endif
  render_txt(txt);

  while (*txt) {
    int c=utf8_get_char((const char**)&txt);
    if ((font=mpxp_context().video().output->font->font[c])>=0 && (x + mpxp_context().video().output->font->width[c] <= mpi.w) && (y + mpxp_context().video().output->font->pic_a[font]->h <= mpi.h))
      draw_alpha->render(mpxp_context().video().output->font->width[c], mpxp_context().video().output->font->pic_a[font]->h,
		 mpxp_context().video().output->font->pic_b[font]->bmp+mpxp_context().video().output->font->start[c],
		 mpxp_context().video().output->font->pic_a[font]->bmp+mpxp_context().video().output->font->start[c],
		 mpxp_context().video().output->font->pic_a[font]->w,
		 mpi.planes[0] + y * mpi.stride[0] + x * (mpi.bpp>>3),
		 mpi.stride[0],finalize);
    x+=mpxp_context().video().output->font->width[c]+mpxp_context().video().output->font->charspace;
  }
}

void menu_draw_text_full(const mp_image_t& mpi,const std::string& _txt,
			 int x, int y,int w, int h,
			 int vspace, int warp, int align, int anchor) {
  const char* txt=_txt.c_str();
  int need_w,need_h;
  int sy, ymin, ymax;
  int sx, xmin, xmax, xmid, xrmin;
  int ll = 0;
  int font;
  int finalize=mpxp_context().video().output->is_final();
  const LocalPtr<OSD_Render> draw_alpha(new(zeromem) OSD_Render(mpi.imgfmt));

#ifdef USE_FRIBIDI
  txt = menu_fribidi(txt);
#endif
  render_txt(txt);

  if(x > mpi.w || y > mpi.h)
    return;

  if(anchor & MENU_TEXT_VCENTER) {
    if(h <= 0) h = mpi.h;
    ymin = y - h/2;
    ymax = y + h/2;
  }  else if(anchor & MENU_TEXT_BOT) {
    if(h <= 0) h = mpi.h - y;
    ymin = y - h;
    ymax = y;
  } else {
    if(h <= 0) h = mpi.h - y;
    ymin = y;
    ymax = y + h;
  }

  if(anchor & MENU_TEXT_HCENTER) {
    if(w <= 0) w = mpi.w;
    xmin = x - w/2;
    xmax = x + w/2;
  }  else if(anchor & MENU_TEXT_RIGHT) {
    if(w <= 0) w = mpi.w -x;
    xmin = x - w;
    xmax = x;
  } else {
    if(w <= 0) w = mpi.w -x;
    xmin = x;
    xmax = x + w;
  }

  // How many space do we need to draw this ?
  menu_text_size(txt,w,vspace,warp,&need_w,&need_h);

  // Find the first line
  if(align & MENU_TEXT_VCENTER)
    sy = ymin + ((h - need_h)/2);
  else if(align & MENU_TEXT_BOT)
    sy = ymax - need_h - 1;
  else
    sy = y;

#if 0
  // Find the first col
  if(align & MENU_TEXT_HCENTER)
    sx = xmin + ((w - need_w)/2);
  else if(align & MENU_TEXT_RIGHT)
    sx = xmax - need_w;
#endif

  xmid = xmin + (xmax - xmin) / 2;
  xrmin = xmin;
  // Clamp the bb to the mpi size
  if(ymin < 0) ymin = 0;
  if(xmin < 0) xmin = 0;
  if(ymax > mpi.h) ymax = mpi.h;
  if(xmax > mpi.w) xmax = mpi.w;

  // Jump some the beginnig text if needed
  while(sy < ymin && *txt) {
    int c=utf8_get_char((const char**)&txt);
    if(c == '\n' || (warp && ll + mpxp_context().video().output->font->width[c] > w)) {
      ll = 0;
      sy += mpxp_context().video().output->font->height + vspace;
      if(c == '\n') continue;
    }
    ll += mpxp_context().video().output->font->width[c]+mpxp_context().video().output->font->charspace;
  }
  if(*txt == '\0') // Nothing left to draw
      return;

  while(sy < ymax && *txt) {
    const char* line_end = NULL;
    int n;

    if(txt[0] == '\n') { // New line
      sy += mpxp_context().video().output->font->height + vspace;
      txt++;
      continue;
    }

    // Get the length and end of this line
    for(n = 0, ll = 0 ; txt[n] != '\0' && txt[n] != '\n'  ; n++) {
      unsigned char c = txt[n];
      if(warp && ll + mpxp_context().video().output->font->width[c]  > w)  break;
      ll += mpxp_context().video().output->font->width[c]+mpxp_context().video().output->font->charspace;
    }
    line_end = &txt[n];
    ll -= mpxp_context().video().output->font->charspace;


    if(align & (MENU_TEXT_HCENTER|MENU_TEXT_RIGHT)) {
      // Too long line
      if(ll > xmax-xmin) {
	if(align & MENU_TEXT_HCENTER) {
	  int mid = ll/2;
	  // Find the middle point
	  for(n--, ll = 0 ; n <= 0 ; n--) {
	    ll += mpxp_context().video().output->font->width[(int)txt[n]]+mpxp_context().video().output->font->charspace;
	    if(ll - mpxp_context().video().output->font->charspace > mid) break;
	  }
	  ll -= mpxp_context().video().output->font->charspace;
	  sx = xmid + mid - ll;
	} else// MENU_TEXT_RIGHT)
	  sx = xmax + mpxp_context().video().output->font->charspace;

	// We are after the start point -> go back
	if(sx > xmin) {
	  for(n-- ; n <= 0 ; n--) {
	    unsigned char c = txt[n];
	    if(sx - mpxp_context().video().output->font->width[c] - mpxp_context().video().output->font->charspace < xmin) break;
	    sx -= mpxp_context().video().output->font->width[c]+mpxp_context().video().output->font->charspace;
	  }
	} else { // We are before the start point -> go forward
	  for( ; sx < xmin && (&txt[n]) != line_end ; n++) {
	    unsigned char c = txt[n];
	    sx += mpxp_context().video().output->font->width[c]+mpxp_context().video().output->font->charspace;
	  }
	}
	txt = &txt[n]; // Jump to the new start char
      } else {
	if(align & MENU_TEXT_HCENTER)
	  sx = xmid - ll/2;
	else
	  sx = xmax - 1 - ll;
      }
    } else {
      for(sx = xrmin ;  sx < xmin && txt != line_end ; txt++) {
	unsigned char c = txt[n];
	sx += mpxp_context().video().output->font->width[c]+mpxp_context().video().output->font->charspace;
      }
    }

    while(sx < xmax && txt != line_end) {
      int c=utf8_get_char((const char**)&txt);
      font = mpxp_context().video().output->font->font[c];
      if(font >= 0) {
	int cs = (mpxp_context().video().output->font->pic_a[font]->h - mpxp_context().video().output->font->height) / 2;
	if ((sx + mpxp_context().video().output->font->width[c] < xmax)  &&  (sy + mpxp_context().video().output->font->height < ymax) )
	  draw_alpha->render(mpxp_context().video().output->font->width[c], mpxp_context().video().output->font->height,
		     mpxp_context().video().output->font->pic_b[font]->bmp+mpxp_context().video().output->font->start[c] +
		     cs * mpxp_context().video().output->font->pic_a[font]->w,
		     mpxp_context().video().output->font->pic_a[font]->bmp+mpxp_context().video().output->font->start[c] +
		     cs * mpxp_context().video().output->font->pic_a[font]->w,
		     mpxp_context().video().output->font->pic_a[font]->w,
		     mpi.planes[0] + sy * mpi.stride[0] + sx * (mpi.bpp>>3),
		     mpi.stride[0],finalize);
	//	else
	//printf("Can't draw '%c'\n",c);
      }
      sx+=mpxp_context().video().output->font->width[c]+mpxp_context().video().output->font->charspace;
    }
    txt = line_end;
    if(txt[0] == '\0') break;
    sy += mpxp_context().video().output->font->height + vspace;
  }
}

int menu_text_length(const std::string& _txt) {
  const char* txt = _txt.c_str();
  int l = 0;
  render_txt(txt);
  while (*txt) {
    int c=utf8_get_char((const char**)&txt);
    l += mpxp_context().video().output->font->width[c]+mpxp_context().video().output->font->charspace;
  }
  return l - mpxp_context().video().output->font->charspace;
}

void menu_text_size(const std::string& _txt,int max_width, int vspace, int warp, int* _w, int* _h) {
  const char* txt = _txt.c_str();
  int l = 1, i = 0;
  int w = 0;

  render_txt(txt);
  while (*txt) {
    int c=utf8_get_char((const char**)&txt);
    if(c == '\n' || (warp && i + mpxp_context().video().output->font->width[c] >= max_width)) {
      if(*txt)
	l++;
      i = 0;
      if(c == '\n') continue;
    }
    i += mpxp_context().video().output->font->width[c]+mpxp_context().video().output->font->charspace;
    if(i > w) w = i;
  }

  *_w = w;
  *_h = (l-1) * (mpxp_context().video().output->font->height + vspace) + mpxp_context().video().output->font->height;
}


int menu_text_num_lines(const std::string& _txt, int max_width) {
  const char* txt = _txt.c_str();
  int l = 1, i = 0;
  render_txt(txt);
  while (*txt) {
    int c=utf8_get_char((const char**)&txt);
    if(c == '\n' || i + mpxp_context().video().output->font->width[c] > max_width) {
      l++;
      i = 0;
      if(c == '\n') continue;
    }
    i += mpxp_context().video().output->font->width[c]+mpxp_context().video().output->font->charspace;
  }
  return l;
}

static const char* menu_text_get_next_line(const char* txt, int max_width) {
  int i = 0;
  render_txt(txt);
  while (*txt) {
    int c=utf8_get_char((const char**)&txt);
    if(c == '\n') {
      txt++;
      break;
    }
    i += mpxp_context().video().output->font->width[c];
    if(i >= max_width)
      break;
    i += mpxp_context().video().output->font->charspace;
  }
  return txt;
}


void menu_draw_box(const mp_image_t& mpi,unsigned char grey,unsigned char alpha, int x, int y, int w, int h) {
    const LocalPtr<OSD_Render> draw_alpha(new(zeromem) OSD_Render(mpi.imgfmt));
    int g;

    if(x > mpi.w || y > mpi.h) return;

    if(x < 0) w += x, x = 0;
    if(x+w > mpi.w) w = mpi.w-x;
    if(y < 0) h += y, y = 0;
    if(y+h > mpi.h) h = mpi.h-y;

    g = ((256-alpha)*grey)>>8;
    if(g < 1) g = 1;

    int finalize = mpxp_context().video().output->is_final();
    int stride = (w+7)&(~7); // round to 8
    unsigned char pic[stride*h],pic_alpha[stride*h];
    memset(pic,g,stride*h);
    memset(pic_alpha,alpha,stride*h);
    draw_alpha->render(w,h,pic,pic_alpha,stride,
	       mpi.planes[0] + y * mpi.stride[0] + x * (mpi.bpp>>3),
	       mpi.stride[0],finalize);
}
