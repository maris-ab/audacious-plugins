/*  FileWriter-Plugin DSF
 *  (C) copyright 2024 Maris Abele
 *  Plugin structure based on wav plugin by Michał Lipski
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "filewriter.h"
#include <string.h>
#include <audacious/audtag.h>
#include <libaudcore/runtime.h>

#pragma pack(push) // must be byte-aligned
#pragma pack(1)
struct dsfhead
{
  const char     dsd_chunk[4] = {'D','S','D',' '};
  const uint64_t head_chunkize = 28;
        uint64_t file_size = 0;
        uint64_t id3_offset = 0;
  const char     fmt_chunk[4] = {'f','m','t',' '};
  const uint64_t fmt_chunkize = 52;
  const uint32_t format_vers = 1;
  const uint32_t format_id = 0;
        uint32_t channel_type = 2;
        uint32_t channel_num = 2;
        uint32_t sample_freq = 2822400;
  const uint32_t bitorder = 1; // 1-LSB, 8-MSB
        uint64_t sample_count = 0;
  const uint32_t block_size = 4096;
  const uint32_t reserved = 0;
  const char     data_chunk[4] = {'d','a','t','a'};
        uint64_t data_size = 0; // size + 12
};
// static const char id3_empty[10] = {'I','D','3',3,0,0,0,0,0,0};
#pragma pack(pop)

static struct dsfhead header;

static int format;
static const Tuple * dsf_tuple;
static Index<uint8_t> pack_buf;
static Index<uint8_t> dsfbuf;
static uint32_t dsf_frame_pos;
static uint64_t written;

// Sony DSF format
// Bit reverse DSF LSB Least Significant Bit first
static const unsigned char reverse_tab[16] = {
  0x0, 0x8, 0x4, 0xc, 0x2, 0xa, 0x6, 0xe,
  0x1, 0x9, 0x5, 0xd, 0x3, 0xb, 0x7, 0xf};

// Deinterlace DSD data to DSF
void dsf_deinterlace_loop(uint8_t * in, uint8_t * out, bool is_lsb_first,
        int channels, uint32_t block_size, uint32_t frames)
{
    const uint8_t *end = in + (frames * channels);
    while (in < end) {
        for (int ch = 0; ch < channels; ch++) {
            uint8_t val = *(in++);
            if (is_lsb_first) // Bit reverse DSD LSB Least Significant Bit first
                val = ((reverse_tab[val & 0b1111] << 4) | reverse_tab[val >> 4]);
            *(out + (ch * block_size)) = val;
        }
        out++;
    }
}

static bool dsf_open(VFSFile & file, const format_info & info, const Tuple & tuple)
{
    if (!is_dsd(info.format)) {
        AUDERR("The input data is not in DSD format!\n");
        return false;
    }

    header.channel_type = info.channels;
    header.channel_num = info.channels;
    header.sample_freq = info.frequency;
    header.sample_freq <<= 5; // *32

    if (file.fwrite(& header, 1, sizeof(header)) != sizeof(header)) {
        AUDERR ("Error writing initial ID3 header\n");
        return false;
    }
    format = info.format;
    dsf_tuple = &tuple;
    written = 0;
    dsfbuf.resize(header.block_size * header.channel_num);
    dsf_frame_pos=0;
    return true;
}

static void dsf_write(VFSFile & file, const void * data, int len)
{
    pack_buf.resize(len);
    dsdaudio_from_in(data, format, pack_buf.begin(), len / FMT_SIZEOF(format), header.channel_num);
    uint32_t pack_pos = 0;
    uint32_t pack_frames = len / header.channel_num;
    uint32_t frames_left;
    while ((frames_left = aud::min(header.block_size - dsf_frame_pos, pack_frames)) > 0) {
        dsf_deinterlace_loop(pack_buf.begin() + pack_pos, dsfbuf.begin() + dsf_frame_pos,
            header.bitorder == 1, header.channel_num, header.block_size, frames_left);
        pack_pos += frames_left * header.channel_num;
        pack_frames -= frames_left;
        dsf_frame_pos += frames_left;
        if (dsf_frame_pos >= header.block_size) {
            uint32_t filewrb;
            if ((filewrb = file.fwrite (dsfbuf.begin(), 1, header.block_size * header.channel_num))
                    != header.block_size * header.channel_num)
                AUDERR ("Error while writing to .dsf output file\n");
            written += filewrb;
            dsf_frame_pos=0;
        }
    }
}

static void dsf_close(VFSFile & file)
{
    pack_buf.clear();
    header.sample_count = (written / header.channel_num) + dsf_frame_pos;
    header.sample_count <<= 3; // *8
    if (dsf_frame_pos > 0) {
        // Fill buffer with 0 till block_size
        for (uint32_t ch = 0; ch < header.channel_num; ch++) {
            memset(dsfbuf.begin() + dsf_frame_pos + (ch * header.block_size),
                0, header.block_size - dsf_frame_pos);
        }
        uint32_t filewrb; // Write last block
        if ((filewrb = file.fwrite (dsfbuf.begin(), 1, header.block_size * header.channel_num))
                != header.block_size * header.channel_num)
            AUDERR("Error writing last block to .dsf output file\n");
        written += filewrb;
        dsf_frame_pos=0;
    }
    dsfbuf.clear();
//    header.id3_offset = written + sizeof(header);
    header.data_size = written + 12;
/*
    if (file.fwrite(id3_empty, 1, sizeof(id3_empty)) != sizeof(id3_empty))
        AUDERR ("Error writing ID3 header\n");
    // ID3 tag requires change in audacious core to add witing ID3 tag at the particular offset
    audtag::write_tuple(file, *dsf_tuple, audtag::TagType::ID3v2, header.id3_offset);
*/
    header.file_size = file.fsize();

    if (file.fseek(0, VFS_SEEK_SET) ||
     file.fwrite (& header, 1, sizeof(header)) != sizeof(header))
        AUDERR ("Error writing .dsf output file header\n");
}

static int dsf_format_required (int fmt)
{
    switch (fmt)
    {
        case FMT_DSD_MSB8:
        case FMT_DSD_LSB8:
        case FMT_DSD_MSB16_LE:
        case FMT_DSD_MSB16_BE:
        case FMT_DSD_MSB32_LE:
        case FMT_DSD_MSB32_BE:
            return fmt;
        default:
            return FMT_DSD_LSB8;
    }
}

FileWriterImpl dsf_plugin = {
    nullptr,  // init
    dsf_open,
    dsf_write,
    dsf_close,
    dsf_format_required,
};
