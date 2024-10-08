/*
 * Dynamic Range Compression Plugin for Audacious
 * Copyright 2010-2014 John Lindgren
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions, and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions, and the following disclaimer in the documentation
 *    provided with the distribution.
 *
 * This software is provided "as is" and without any warranty, express or
 * implied. In no event shall the authors be liable for any damages arising from
 * the use of this software.
 */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <libaudcore/i18n.h>
#include <libaudcore/plugin.h>
#include <libaudcore/preferences.h>
#include <libaudcore/ringbuf.h>
#include <libaudcore/runtime.h>

/* Response time adjustments.  Maybe this should be adjustable? */
#define CHUNK_TIME 0.2f /* seconds */
#define CHUNKS 5
#ifdef DEF_AUDIO_FLOAT64
#define DECAY 0.3
#else
#define DECAY 0.3f
#endif

/* What is a "normal" volume?  Replay Gain stuff claims to use 89 dB, but what
 * does that translate to in our PCM range? */
static const char * const compressor_defaults[] = {
    "center", "0.5",
    "range", "0.5",
     nullptr
};

static const PreferencesWidget compressor_widgets[] = {
    WidgetLabel (N_("<b>Compression</b>")),
    WidgetSpin (N_("Center volume:"),
        WidgetFloat ("compressor", "center"),
        {0.1, 1, 0.1}),
    WidgetSpin (N_("Dynamic range:"),
        WidgetFloat ("compressor", "range"),
        {0.0, 3.0, 0.1})
};

static const PluginPreferences compressor_prefs = {{compressor_widgets}};

static const char compressor_about[] =
 N_("Dynamic Range Compression Plugin for Audacious\n"
    "Copyright 2010-2014 John Lindgren");

class Compressor : public EffectPlugin
{
public:
    static constexpr PluginInfo info = {
        N_("Dynamic Range Compressor"),
        PACKAGE,
        compressor_about,
        & compressor_prefs
    };

    constexpr Compressor () : EffectPlugin (info, 0, true) {}

    bool init ();
    void cleanup ();

    void start (int & channels, int & rate);
    Index<audio_sample> & process (Index<audio_sample> & data);
    bool flush (bool force);
    Index<audio_sample> & finish (Index<audio_sample> & data, bool end_of_playlist);
    int adjust_delay (int delay);
};

EXPORT Compressor aud_plugin_instance;

/* The read pointer of the ring buffer is kept aligned to the chunk size at all
 * times.  To preserve the alignment, each read from the buffer must either (a)
 * read a multiple of the chunk size or (b) empty the buffer completely.  Writes
 * to the buffer need not be aligned to the chunk size. */

static RingBuf<audio_sample> buffer, peaks;
static Index<audio_sample> output;
static int chunk_size;
static audio_sample current_peak;
static int current_channels, current_rate;

/* I used to find the maximum sample and take that as the peak, but that doesn't
 * work well on badly clipped tracks.  Now, I use the highly sophisticated
 * method of averaging the absolute value of the samples and multiplying by 6, a
 * number proved by experiment (on exactly three files) to best approximate the
 * actual peak. */

static audio_sample calc_peak (audio_sample * data, int length)
{
    audio_sample sum = 0;

    const audio_sample * end = data + length;
    while (data < end)
        sum += fabs (* data ++);

#ifdef DEF_AUDIO_FLOAT64
    return aud::max (0.01, sum / length * 6);
#else
    return aud::max (0.01f, sum / length * 6);
#endif
}

static void do_ramp (audio_sample * data, int length, audio_sample peak_a, audio_sample peak_b)
{
    audio_sample center = aud_get_double ("compressor", "center");
    audio_sample range = aud_get_double ("compressor", "range");
    audio_sample a = pow (peak_a / center, range - 1);
    audio_sample b = pow (peak_b / center, range - 1);

    for (int count = 0; count < length; count ++)
    {
        * data = (* data) * (a * (length - count) + b * count) / length;
        data ++;
    }
}

bool Compressor::init ()
{
    aud_config_set_defaults ("compressor", compressor_defaults);
    return true;
}

void Compressor::cleanup ()
{
    buffer.destroy ();
    peaks.destroy ();
    output.clear ();
}

void Compressor::start (int & channels, int & rate)
{
    current_channels = channels;
    current_rate = rate;

    chunk_size = channels * (int) (rate * CHUNK_TIME);

    buffer.alloc (chunk_size * CHUNKS);
    peaks.alloc (CHUNKS);

    flush (true);
}

Index<audio_sample> & Compressor::process (Index<audio_sample> & data)
{
    output.resize (0);

    int offset = 0;
    int remain = data.len ();

    while (1)
    {
        int writable = aud::min (remain, buffer.space ());

        buffer.copy_in (& data[offset], writable);

        offset += writable;
        remain -= writable;

        if (buffer.space ())
            break;

        while (peaks.len () < CHUNKS)
            peaks.push (calc_peak (& buffer[chunk_size * peaks.len ()], chunk_size));

        if (current_peak == 0.0)
        {
            for (int i = 0; i < CHUNKS; i ++)
                current_peak = aud::max (current_peak, peaks[i]);
        }

#ifdef DEF_AUDIO_FLOAT64
        audio_sample new_peak = aud::max (peaks[0], current_peak * (1.0 - DECAY));
#else
        audio_sample new_peak = aud::max (peaks[0], current_peak * (1.0f - DECAY));
#endif
        for (int count = 1; count < CHUNKS; count ++)
            new_peak = aud::max (new_peak, current_peak + (peaks[count] - current_peak) / count);

        do_ramp (& buffer[0], chunk_size, current_peak, new_peak);

        buffer.move_out (output, -1, chunk_size);

        current_peak = new_peak;
        peaks.pop ();
    }

    return output;
}

bool Compressor::flush (bool force)
{
    buffer.discard ();
    peaks.discard ();

    current_peak = 0.0;
    return true;
}

Index<audio_sample> & Compressor::finish (Index<audio_sample> & data, bool end_of_playlist)
{
    output.resize (0);

    peaks.discard ();

    while (buffer.len ())
    {
        int writable = buffer.linear ();

        if (current_peak != 0.0)
            do_ramp (& buffer[0], writable, current_peak, current_peak);

        buffer.move_out (output, -1, writable);
    }

    if (current_peak != 0.0)
        do_ramp (data.begin (), data.len (), current_peak, current_peak);

    output.insert (data.begin (), -1, data.len ());

    return output;
}

int Compressor::adjust_delay (int delay)
{
    return delay + aud::rescale<int64_t> (buffer.len () / current_channels, current_rate, 1000);
}
