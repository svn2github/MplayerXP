#include "mpxp_config.h"
#include "osdep/mplib.h"
using namespace	usr;
/* GyS-TermIO v2.0 (for GySmail v3)          (C) 1999 A'rpi/ESP-team */
//#define HAVE_TERMCAP
#define USE_IOCTL

#define MAX_KEYS 64
#define BUF_LEN 256

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#ifdef USE_IOCTL
#include <sys/ioctl.h>
#endif

#ifdef HAVE_TERMIOS_H
#define HAVE_TERMIOS
#include <termios.h>
#endif
#ifdef HAVE_SYS_TERMIOS_H
#define HAVE_TERMIOS
#include <sys/termios.h>
#endif
#include <unistd.h>

#include "osdep_msg.h"
#include "keycodes.h"

#include "getch2.h"

#ifdef HAVE_TERMCAP

#if 0
#include <termcap.h>
#else
  extern int tgetent (char *BUFFER, char *TERMTYPE);
  extern int tgetnum (char *NAME);
  extern int tgetflag (char *NAME);
  extern char *tgetstr (char *NAME, char **AREA);
#endif
#endif

namespace	usr {
#ifdef HAVE_TERMIOS
static struct termios tio_orig;
#endif
static int getch2_len=0;
static char getch2_buf[BUF_LEN];

static int screen_width=80;
static int screen_height=24;

typedef struct {
  int len;
  int code;
  char chars[8];
} keycode_st;
static keycode_st getch2_keys[MAX_KEYS];
static int getch2_key_db=0;

#ifdef HAVE_TERMCAP

static char term_buffer[4096];
static char term_buffer2[4096];
static char *term_p=term_buffer2;

static void termcap_add(char *id,int code){
    char *p=::tgetstr(id,&term_p);
    if(!p) return;
    if(getch2_key_db>=MAX_KEYS) return;
    getch2_keys[getch2_key_db].len=strlen(p);
    strncpy(getch2_keys[getch2_key_db].chars,p,8);
    getch2_keys[getch2_key_db].code=code;
    ++getch2_key_db;
}

static int success=0;

int load_termcap(const std::string& _termtype,const std::map<std::string,std::string>& envm){
    std::string termtype=_termtype;
    if(termtype.empty()) {
	std::map<std::string,std::string>::const_iterator it;
	it = envm.find("TERM");
	if(it!=envm.end()) termtype = (*it).second;
    }

    success=::tgetent(term_buffer, termtype);
    if(success<0){ mpxp_err<<"Could not access the 'termcap' data base"<<std::endl; return 0; }
    if(success==0){ mpxp_err<<"Terminal type `"<<termtype<<"' is not defined"<<std::endl; return 0;}

    screen_width=::tgetnum("co");
    screen_height=::tgetnum("li");
    if(screen_width<1 || screen_width>255) screen_width=80;
    if(screen_height<1 || screen_height>255) screen_height=24;

    termcap_add("kP",KEY_PGUP);
    termcap_add("kN",KEY_PGDWN);
    termcap_add("kh",KEY_HOME);
    termcap_add("kH",KEY_END);
    termcap_add("kI",KEY_INS);
    termcap_add("kD",KEY_DEL);
    termcap_add("kb",KEY_BS);
    termcap_add("kl",KEY_LEFT);
    termcap_add("kd",KEY_DOWN);
    termcap_add("ku",KEY_UP);
    termcap_add("kr",KEY_RIGHT);
    termcap_add("k0",KEY_F+0);
    termcap_add("k1",KEY_F+1);
    termcap_add("k2",KEY_F+2);
    termcap_add("k3",KEY_F+3);
    termcap_add("k4",KEY_F+4);
    termcap_add("k5",KEY_F+5);
    termcap_add("k6",KEY_F+6);
    termcap_add("k7",KEY_F+7);
    termcap_add("k8",KEY_F+8);
    termcap_add("k9",KEY_F+9);
    termcap_add("k;",KEY_F+10);
    return getch2_key_db;
}

#endif

static void get_screen_size(){
#ifdef USE_IOCTL
  struct winsize ws;
  if (::ioctl(0, TIOCGWINSZ, &ws) < 0 || !ws.ws_row || !ws.ws_col) return;
  screen_width=ws.ws_col;
  screen_height=ws.ws_row;
#endif
}

int getch2(int _time){
  int len=0;
  int code=0;
  int i=0;
  int c;

  while(!getch2_len || (getch2_len==1 && getch2_buf[0]==27)){
    fd_set rfds;
    struct timeval tv;
    int retval;
    /* Watch stdin (fd 0) to see when it has input. */
    FD_ZERO(&rfds); FD_SET(0,&rfds);
    /* Wait up to 'time' microseconds. */
    tv.tv_sec=_time/1000; tv.tv_usec = (_time%1000)*1000;
    retval=::select(1, &rfds, NULL, NULL, &tv);
    if(retval<=0) return -1;
    /* Data is available now. */
    retval=::read(0,&getch2_buf[getch2_len],BUF_LEN-getch2_len);
    if(retval<1) return -1;
    getch2_len+=retval;
  }

    /* First find in the TERMCAP database: */
    for(i=0;i<getch2_key_db;i++){
      if((len=getch2_keys[i].len)<=getch2_len)
	if(memcmp(getch2_keys[i].chars,getch2_buf,len)==0){
	  code=getch2_keys[i].code; goto found;
	}
    }
    len=1;code=getch2_buf[0];
    /* Check the well-known codes... */
    if(code!=27){
      if(code=='A'-64){ code=KEY_HOME; goto found;}
      if(code=='E'-64){ code=KEY_END; goto found;}
      if(code=='D'-64){ code=KEY_DEL; goto found;}
      if(code=='H'-64){ code=KEY_BS; goto found;}
      if(code=='U'-64){ code=KEY_PGUP; goto found;}
      if(code=='V'-64){ code=KEY_PGDWN; goto found;}
      if(code==8 || code==127){ code=KEY_BS; goto found;}
      if(code==10 || code==13){
	if(getch2_len>1){
	  c=getch2_buf[1];
	  if(c==10 || c==13) if(c!=code) len=2;
	}
	code=KEY_ENTER;
	goto found;
      }
    } else if(getch2_len>1){
      c=getch2_buf[1];
      if(c==27){ code=KEY_ESC; len=2; goto found;}
      if(c>='0' && c<='9'){ code=c-'0'+KEY_F; len=2; goto found;}
      if(getch2_len>=4 && c=='[' && getch2_buf[2]=='['){
	c=getch2_buf[3];
	if(c>='A' && c<'A'+12){ code=KEY_F+1+c-'A';len=4;goto found;}
      }
      if(c=='[' || c=='O') if(getch2_len>=3){
	c=getch2_buf[2];
	static short int ctable[]={ KEY_UP,KEY_DOWN,KEY_RIGHT,KEY_LEFT,0,
		       KEY_END,KEY_PGDWN,KEY_HOME,KEY_PGUP,0,0,KEY_INS,0,0,0,
		       KEY_F+1,KEY_F+2,KEY_F+3,KEY_F+4};
	if(c>='A' && c<='S')
	  if(ctable[c-'A']){ code=ctable[c-'A']; len=3; goto found;}
      }
      if(getch2_len>=4 && c=='[' && getch2_buf[3]=='~'){
	c=getch2_buf[2];
	int ctable[8]={KEY_HOME,KEY_INS,KEY_DEL,KEY_END,KEY_PGUP,KEY_PGDWN,KEY_HOME,KEY_END};
	if(c>='1' && c<='8'){ code=ctable[c-'1']; len=4; goto found;}
      }
      if(getch2_len>=5 && c=='[' && getch2_buf[4]=='~'){
	i=getch2_buf[2]-'0';
	int j=getch2_buf[3]-'0';
	if(i>=0 && i<=9 && j>=0 && j<=9){
	  static short int ftable[20]={
	    11,12,13,14,15, 17,18,19,20,21,
	    23,24,25,26,28, 29,31,32,33,34 };
	  int a=i*10+j;
	  for(i=0;i<20;i++) if(ftable[i]==a){ code=KEY_F+1+i;len=5;goto found;}
	}
      }
    }
found:
  if((getch2_len-=len)>0){
    for(i=0;i<getch2_len;i++) getch2_buf[i]=getch2_buf[len+i];
  }
  return code;
}

static int getch2_status=0;

void getch2_enable(){
#ifdef HAVE_TERMIOS
struct termios tio_new;
#if defined(__NetBSD__) || defined(__svr4__) || defined(__CYGWIN__)
    ::tcgetattr(0,&tio_orig);
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__bsdi__) || defined(__APPLE__)
    ::ioctl(0,TIOCGETA,&tio_orig);
#else
    ::ioctl(0,TCGETS,&tio_orig);
#endif
    tio_new=tio_orig;
    tio_new.c_lflag &= ~(ICANON|ECHO); /* Clear ICANON and ECHO. */
    tio_new.c_cc[VMIN] = 1;
    tio_new.c_cc[VTIME] = 0;
#if defined(__NetBSD__) || defined(__svr4__) || defined(__CYGWIN__) || defined(__OS2__)
    ::tcsetattr(0,TCSANOW,&tio_new);
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__bsdi__) || defined(__APPLE__)
    ::ioctl(0,TIOCSETA,&tio_new);
#else
    ::ioctl(0,TCSETS,&tio_new);
#endif
#endif
    getch2_status=1;
}

void getch2_disable(){
    if(!getch2_status) return; // already disabled / never enabled
#ifdef HAVE_TERMIOS
#if defined(__NetBSD__) || defined(__svr4__) || defined(__CYGWIN__) || defined(__OS2__)
    ::tcsetattr(0,TCSANOW,&tio_orig);
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__bsdi__) || defined(__APPLE__)
    ::ioctl(0,TIOCSETA,&tio_orig);
#else
    ::ioctl(0,TCSETS,&tio_orig);
#endif
#endif
    getch2_status=0;
}

} // namespace	usr
