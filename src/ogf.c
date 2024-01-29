/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "ogf.h"
#include "ogg.h"
#include "flac.h"

/* https://xiph.org/ogg/doc/framing.html 
 * https://xiph.org/flac/ogg_mapping.html
 * https://xiph.org/vorbis/doc/Vorbis_I_spec.html#x1-610004.2 */

int
get_ogf_metadata(PerlIO *infile, char *file, HV *info, HV *tags)
{
  return _ogf_parse(infile, file, info, tags, 0);
}

int
_ogf_parse(PerlIO *infile, char *file, HV *info, HV *tags, uint8_t seeking)
{
  Buffer ogg_buf;
  //Buffer vorbis_buf;
  unsigned char *bptr;
  unsigned char *last_bptr;
  unsigned int buf_size;
  unsigned int id3_size = 0; // size of leading ID3 data
  uint32_t song_length_ms = 0;

  off_t file_size;           // total file size
  off_t audio_size;          // total size of audio without tags
  off_t audio_offset = 0;    // offset to audio
  off_t seek_position;
  
  unsigned char ogghdr[OGG_HEADER_SIZE];
  char header_type;
  int serialno;
  uint64_t our_serialno = ULLONG_MAX;
  int final_serialno;
  int pagenum;
  uint8_t num_segments;
  int pagelen;
  int page = 0;
  int packets = 0;
  int streams = 0;
  short num_headers = 0;
  
  unsigned char opushdr[11];
  unsigned char channels;
  unsigned int input_samplerate = 0;
  uint64_t granule_pos = 0;
  
  unsigned char TOC_byte = 0;

  flacinfo *flac;
  Newz(0, flac, sizeof(flacinfo), flacinfo);
  Newz(0, flac->buf, sizeof(Buffer), Buffer);

  flac->infile         = infile;
  flac->file           = file;
  flac->info           = info;
  flac->tags           = tags;
  flac->audio_offset   = 0;
  flac->seeking        = seeking ? 1 : 0;
  flac->num_seekpoints = 0;

  buffer_init(flac->buf, 0);

  flac->file_size = _file_size(infile);

  int i;
  int err = 0;
  
  buffer_init(&ogg_buf, OGG_BLOCK_SIZE);

  file_size = _file_size(infile);
  my_hv_store( info, "file_size", newSVuv(file_size) );
  
  if ( !_check_buf(infile, &ogg_buf, 10, OGG_BLOCK_SIZE) ) {
    err = -1;
    goto out;
  }

  // Skip ID3 tags if any
  bptr = (unsigned char *)buffer_ptr(&ogg_buf);
  if (
    (bptr[0] == 'I' && bptr[1] == 'D' && bptr[2] == '3') &&
    bptr[3] < 0xff && bptr[4] < 0xff &&
    bptr[6] < 0x80 && bptr[7] < 0x80 && bptr[8] < 0x80 && bptr[9] < 0x80
  ) {
    /* found an ID3 header... */
    id3_size = 10 + (bptr[6]<<21) + (bptr[7]<<14) + (bptr[8]<<7) + bptr[9];

    if (bptr[5] & 0x10) {
      // footer present
      id3_size += 10;
    }
    
    buffer_clear(&ogg_buf);
    
    audio_offset += id3_size;
    
    DEBUG_TRACE("Skipping ID3v2 tag of size %d\n", id3_size);

    PerlIO_seek(infile, id3_size, SEEK_SET);
  }

  while (1) {
    bool full_packet = true;
    
    // Grab 28-byte Ogg header
    if ( !_check_buf(infile, &ogg_buf, OGG_HEADER_SIZE, OGG_BLOCK_SIZE) ) {
      err = -1;
      goto out;
    }
    
    buffer_get(&ogg_buf, ogghdr, OGG_HEADER_SIZE);
    
    audio_offset += OGG_HEADER_SIZE;
    
    // check that the first four bytes are 'OggS'
    if ( ogghdr[0] != 'O' || ogghdr[1] != 'g' || ogghdr[2] != 'g' || ogghdr[3] != 'S' ) {
      PerlIO_printf(PerlIO_stderr(), "Not an Ogg file (bad OggS header): %s\n", file);
      goto out;
    }
  
    // Header type flag
    header_type = ogghdr[5];
    
    // Absolute granule position, used to find the first audio page
    bptr = ogghdr + 6;
    granule_pos = (uint64_t)CONVERT_INT32LE(bptr);
    bptr += 4;
    granule_pos |= (uint64_t)CONVERT_INT32LE(bptr) << 32;
    
    // Stream serial number
    serialno = CONVERT_INT32LE((ogghdr+14));
    
    // Count start-of-stream pages
    if ( header_type & 0x02 ) {
      // we only care about first stream (and no multiplex) 
      if (our_serialno == ULLONG_MAX) our_serialno = serialno;        
      streams++;
    }
        
    // stop processing if we reach the 3rd packet and have no data
    if (!num_headers && packets > 2 * streams && !buffer_len(flac->buf) ) {
      break;
    }
    
    // Page seq number
    pagenum = CONVERT_INT32LE((ogghdr+18));
    
    if (page >= 0 && page == pagenum) {
      page++;
    }
    else {
      page = -1;
      DEBUG_TRACE("Missing page(s) in Ogg file: %s\n", file);
    }
  
    // Number of page segments
    num_segments = ogghdr[26];
    
    // Calculate total page size
    pagelen = ogghdr[27];
    if (num_segments > 1) {
      int i;
      full_packet = false;
      
      if ( !_check_buf(infile, &ogg_buf, num_segments, OGG_BLOCK_SIZE) ) {
        err = -1;
        goto out;
      }
      
      for( i = 0; i < num_segments - 1; i++ ) {
        u_char x;
        x = buffer_get_char(&ogg_buf);
        // detect packet termination(s) - there is only one packet per page in OggFlac
        if (x < 255) full_packet = true;
        pagelen += x;
      }

      audio_offset += num_segments - 1;
    }
    
    if ( !_check_buf(infile, &ogg_buf, pagelen, OGG_BLOCK_SIZE) ) {
      err = -1;
      goto out;
    }
  
    // Still don't have enough data, must have reached the end of the file
    if ( buffer_len(&ogg_buf) < pagelen ) {
      PerlIO_printf(PerlIO_stderr(), "Premature end of file: %s\n", file);
      err = -1;
      goto out;
    }
    
    DEBUG_TRACE("OggS page %d (len:%d+28, sn:%u) at %d\n", pagenum, pagelen, serialno, (int)(audio_offset - OGG_HEADER_SIZE));
    if (granule_pos != ULLONG_MAX) DEBUG_TRACE("  granule_pos: %llu\n", granule_pos);   
    else DEBUG_TRACE("  granule_pos: -1\n");   
    
    audio_offset += pagelen;
    
    // if this is not for us, just consume data
    if (serialno != our_serialno) {
      buffer_consume( &ogg_buf, pagelen );
      continue;
    } else if (granule_pos && granule_pos != -1) {
      PerlIO_printf(PerlIO_stderr(), "Audio granule before end of headers\n");
      err = -1;
      goto out;
    }  
    
    DEBUG_TRACE("  Append %d into buffer\n", pagelen);
    buffer_append( flac->buf, buffer_ptr(&ogg_buf), pagelen );
    
    if (!full_packet) {
      buffer_consume( &ogg_buf, pagelen );
      continue;
    } else {
      packets++;
    }   
    
    // we have a full packet in buffer, let's process it 
    TOC_byte = buffer_get_char(flac->buf);
    DEBUG_TRACE("Packet number %d\n", packets);            
    
    // Process \x7fFLAC packet    
    if ( TOC_byte == 0x7f ) {
      DEBUG_TRACE("First packet");            
      if ( strncmp( buffer_ptr(flac->buf), "FLAC", 4 ) == 0) {
        buffer_consume(flac->buf, 4+2);
        num_headers = buffer_get_short(flac->buf);
        DEBUG_TRACE("  Found OggFlac tags TOC packet type with %hu headers\n", num_headers);
        if ( strncmp( buffer_ptr(flac->buf), "fLaC", 4 ) != 0) {
          PerlIO_printf(PerlIO_stderr(), "Not an OggFlac (fLaC) file: %s\n", file);
          err = -1;
          goto out;
        }          
        buffer_consume(flac->buf, 8);
        _flac_parse_streaminfo(flac);
      }
      else {
        PerlIO_printf(PerlIO_stderr(), "Not and OggFlac (FLAC) file: %s\n", file);
        err = -1;
        goto out;
      }
    } else {     
      DEBUG_TRACE("Parsing header type %d\n", TOC_byte & 0x7f);
      if (!seeking) {
        uint8_t type = TOC_byte & 0x7f;  
        buffer_consume(flac->buf, 3);       
        
        if (type == FLAC_TYPE_VORBIS_COMMENT) {
          DEBUG_TRACE("Parsing vorbis_comment\n");
          _parse_vorbis_comments(infile, flac->buf, tags, 0);
        } else if (type == FLAC_TYPE_PICTURE) {
          DEBUG_TRACE("Parsing picture\n");
          if (!_flac_parse_picture(flac)) {
            err = -1;
            goto out;
          }
        }
      }  
      if (TOC_byte & 0x80 || (num_headers && packets == num_headers + 1)) {
          DEBUG_TRACE("Last header\n");
          break;
      }
    }

    // this page belongs to a new packet
    buffer_clear(flac->buf);      
    buffer_consume( &ogg_buf, pagelen );
  }
  
  DEBUG_TRACE("All headers parsed, now doing audio\n");
  
  // from the first packet past the comments
  my_hv_store( info, "audio_offset", newSViv(audio_offset) );
  
  audio_size = file_size - audio_offset;
  my_hv_store( info, "audio_size", newSVuv(audio_size) );
  
  my_hv_store( info, "serial_number", newSVuv(our_serialno) );
  
  song_length_ms = SvIV( *( my_hv_fetch(info, "song_length_ms") ) );
   
  if (song_length_ms > 0) {
     my_hv_store( info, "bitrate", newSVuv( _bitrate(audio_size, song_length_ms) ) );
  } 

  // find the last Ogg page

#define BUF_SIZE 8500 // from vlc
  
  seek_position = file_size - BUF_SIZE;
  while (1) {
    if ( seek_position < audio_offset ) {
      seek_position = audio_offset;
    }

    // calculate average bitrate and duration
    DEBUG_TRACE("Seeking to %d to calculate bitrate/duration\n", (int)seek_position);
    PerlIO_seek(infile, seek_position, SEEK_SET);

    buffer_clear(&ogg_buf);

    if ( !_check_buf(infile, &ogg_buf, OGG_HEADER_SIZE, BUF_SIZE) ) {
      err = -1;
      goto out;
    }

    // Find sync
    bptr = (unsigned char *)buffer_ptr(&ogg_buf);
    buf_size = buffer_len(&ogg_buf);
    last_bptr = bptr;
    // make sure we have room for at least the one ogg page header
    while (buf_size >= OGG_HEADER_SIZE) {
      if (bptr[0] == 'O' && bptr[1] == 'g' && bptr[2] == 'g' && bptr[3] == 'S') {
        bptr += 6;

        // Get absolute granule value
        granule_pos = (uint64_t)CONVERT_INT32LE(bptr);
        bptr += 4;
        granule_pos |= (uint64_t)CONVERT_INT32LE(bptr) << 32;
        bptr += 4;
        DEBUG_TRACE("found granule_pos %llu / samplerate %d to calculate bitrate/duration\n", granule_pos, flac->samplerate);
        //XXX: jump the header size
        last_bptr = bptr;
      }
      else {
        bptr++;
        buf_size--;
      }
    }
    bptr = last_bptr;

    // Get serial number of this page, if the serial doesn't match the beginning of the file
    // we have changed logical bitstreams and can't use the granule_pos for bitrate
    final_serialno = CONVERT_INT32LE((bptr));

    if ( granule_pos && flac->samplerate && our_serialno == final_serialno ) {
      // XXX: needs to adjust for initial granule value if file does not start at 0 samples
      int length = (int)(((granule_pos) * 1.0 / flac->samplerate) * 1000);
      if (!song_length_ms) my_hv_store( info, "song_length_ms", newSVuv(length) );
      my_hv_store( info, "bitrate_ogg", newSVuv( _bitrate(audio_size, length) ) );

      DEBUG_TRACE("Using granule_pos %llu / samplerate %d to calculate bitrate/duration\n", granule_pos, flac->samplerate);
      break;
    }
    if ( !song_length_ms && seek_position == audio_offset ) {
      DEBUG_TRACE("Packet not found we won't be able to determine the length\n");
      break;
    }
    // seek backwards by BUF_SIZE - OGG_HEADER_SIZE so that if our previous sync happened to include the end
    // of page header we will include it in the next read
    seek_position -= (BUF_SIZE - OGG_HEADER_SIZE);
  }

out:
  buffer_free(&ogg_buf);
  
  buffer_free(flac->buf);
  Safefree(flac->buf);

  DEBUG_TRACE("Err %d\n", err);
  if (err) return err;

  return 0;
}

static int
ogf_find_frame(PerlIO *infile, char *file, int offset)
{
  int frame_offset = -1;
  uint32_t samplerate;
  uint32_t song_length_ms;
  uint64_t target_sample;
  HV *info = newHV();
  HV *tags = newHV();

  if (offset < 0) {
    return -1;
  }

   DEBUG_TRACE("Find_frame %d in %s\n", offset, file);
  // We need to read all metadata first to get some data we need to calculate
  if ( _ogf_parse(infile, file, info, tags, 1) != 0 ) {
    goto out;
  }

  song_length_ms = SvUV( *(my_hv_fetch( info, "song_length_ms" )) );
  if (offset >= song_length_ms) {
    goto out;
  }

  // Determine target sample we're looking for
  samplerate = SvIV( *(my_hv_fetch( info, "samplerate" )) );
  target_sample = (uint64_t)offset * samplerate / 1000;

  DEBUG_TRACE("Looking for target sample %llu\n", target_sample);
  frame_offset = _ogg_binary_search_sample(infile, file, info, target_sample);

out:  
  // Don't leak
  SvREFCNT_dec(info);
  SvREFCNT_dec(tags);

  return frame_offset;
}
