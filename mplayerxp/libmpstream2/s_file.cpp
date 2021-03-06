#include "mpxp_config.h"
#include "osdep/mplib.h"
using namespace	usr;
/*
    s_file - stream interface for file i/o.
*/
#include <iostream>
#include <fstream>

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifndef __USE_GNU
#define __USE_GNU
#endif
#include <unistd.h>
#include <fcntl.h>

#include "mplayerxp.h"
#include "stream.h"
#include "stream_internal.h"
#include "stream_msg.h"

namespace	usr {
    class File_Stream_Interface : public Stream_Interface {
	public:
	    File_Stream_Interface(libinput_t& libinput);
	    virtual ~File_Stream_Interface();

	    virtual MPXP_Rc	open(const std::string& filename,unsigned flags);
	    virtual int		read(stream_packet_t * sp);
	    virtual off_t	seek(off_t off);
	    virtual off_t	tell() const;
	    virtual void	close();
	    virtual MPXP_Rc	ctrl(unsigned cmd,any_t* param);
	    virtual Stream::type_e type() const;
	    virtual off_t	size() const;
	    virtual off_t	sector_size() const;
	    virtual std::string mime_type() const;
	private:
	    std::ifstream fs;
	    off_t spos;
	    off_t end_pos;
    };

File_Stream_Interface::File_Stream_Interface(libinput_t&l)
			:Stream_Interface(l),
			spos(0) {}
File_Stream_Interface::~File_Stream_Interface() {}

MPXP_Rc File_Stream_Interface::open(const std::string& filename,unsigned flags)
{
    UNUSED(flags);
//    if(filename=="-") fd=0;
//    else fd=::open(filename.c_str(),O_RDONLY);
    fs.open(filename.c_str(),std::ios_base::binary | std::ios_base::in);
    if(!fs.is_open()) {
	mpxp_err<<"[s_file] Cannot open file: "<<filename<<std::endl;
	return MPXP_False;
    }
    fs.seekg(0,std::ios_base::end);
    end_pos = fs.tellg();
    fs.seekg(0,std::ios_base::beg);
    /* decreasing number of packet from 256 to 10 speedups cache2 from 3.27% to 1.26%
       with full speed 1.04% for -nocache */
    /* Note: Please locate sector_size changinf after all read/write operations of open() function */
    return MPXP_Ok;
}

Stream::type_e File_Stream_Interface::type() const { return (end_pos==-1)?Stream::Type_Stream:Stream::Type_Seekable; }
off_t	File_Stream_Interface::size() const { return end_pos; }
off_t	File_Stream_Interface::sector_size() const { return STREAM_BUFFER_SIZE; }
std::string File_Stream_Interface::mime_type() const { return "application/octet-stream"; }

int File_Stream_Interface::read(stream_packet_t*sp)
{
/*
    Should we repeate read() again on these errno: `EAGAIN', `EIO' ???
*/
    sp->type=0;
    fs.read(sp->buf,sp->len);
    spos = fs.tellg();
    return fs.good()?sp->len:-1;
}

off_t File_Stream_Interface::seek(off_t pos)
{
    fs.clear(fs.failbit);
    fs.clear(fs.badbit);
    fs.clear(fs.eofbit);
    fs.seekg(pos,std::ios_base::beg);
    spos=fs.tellg();
    return spos;
}

off_t File_Stream_Interface::tell() const
{
    return spos;
}

void File_Stream_Interface::close()
{
    if(fs.is_open()) fs.close();
}

MPXP_Rc File_Stream_Interface::ctrl(unsigned cmd,any_t*args) {
    UNUSED(cmd);
    UNUSED(args);
    return MPXP_Unknown;
}

static Stream_Interface* query_file_interface(libinput_t& libinput) { return new(zeromem) File_Stream_Interface(libinput); }

extern const stream_interface_info_t file_stream =
{
    "file://",
    "reads multimedia stream from regular file",
    query_file_interface
};

    class Stdin_Stream_Interface : public File_Stream_Interface {
	public:
	    Stdin_Stream_Interface(libinput_t& libinput);
	    virtual ~Stdin_Stream_Interface();

	    virtual MPXP_Rc	open(const std::string& filename,unsigned flags);
    };

Stdin_Stream_Interface::Stdin_Stream_Interface(libinput_t&l):File_Stream_Interface(l) {}
Stdin_Stream_Interface::~Stdin_Stream_Interface() {}
MPXP_Rc Stdin_Stream_Interface::open(const std::string& filename,unsigned flags) {
    UNUSED(filename);
    return File_Stream_Interface::open("-",flags);
}

static Stream_Interface* query_stdin_interface(libinput_t& libinput) { return new(zeromem) Stdin_Stream_Interface(libinput); }

extern const stream_interface_info_t stdin_stream =
{
    "stdin://",
    "reads multimedia stream from standard input",
    query_stdin_interface
};

}