/*
 * Silence Removal Plugin for Audacious
 * Copyright 2014 John Lindgren
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

#include <libaudcore/i18n.h>
#include <libaudcore/plugin.h>
#include <libaudcore/preferences.h>
#include <libaudcore/ringbuf.h>
#include <libaudcore/runtime.h>

#include <math.h>

#define MAX_BUFFER_SECS  10

class SilenceRemoval : public EffectPlugin
{
public:
    static const char about[];
    static const char * const defaults[];
    static const PreferencesWidget widgets[];
    static const PluginPreferences prefs;

    static constexpr PluginInfo info = {
        N_("Silence Removal"),
        PACKAGE,
        about,
        & prefs
    };

    constexpr SilenceRemoval () : EffectPlugin (info, 0, true) {}

    bool init ();
    void cleanup ();

    void start (int & channels, int & rate);
    Index<audio_sample> & process (Index<audio_sample> & data);
    bool flush (bool force);
};

EXPORT SilenceRemoval aud_plugin_instance;

const char SilenceRemoval::about[] =
 N_("Silence Removal Plugin for Audacious\n"
    "Copyright 2014 John Lindgren");

const char * const SilenceRemoval::defaults[] = {
    "threshold", "-40",
    nullptr
};

const PreferencesWidget SilenceRemoval::widgets[] = {
    WidgetLabel (N_("<b>Silence Removal</b>")),
    WidgetSpin (N_("Threshold:"),
        WidgetInt ("silence-removal", "threshold"),
        {-60, -20, 1, N_("dB")})
};

const PluginPreferences SilenceRemoval::prefs = {{widgets}};

static RingBuf<audio_sample> buffer;
static Index<audio_sample> output;
static int current_channels;
static bool initial_silence;

bool SilenceRemoval::init ()
{
    aud_config_set_defaults ("silence-removal", defaults);
    return true;
}

void SilenceRemoval::cleanup ()
{
    buffer.destroy ();
    output.clear ();
}

void SilenceRemoval::start (int & channels, int & rate)
{
    buffer.discard ();
    buffer.alloc (channels * rate * MAX_BUFFER_SECS);
    output.resize (0);

    current_channels = channels;
    initial_silence = true;
}

static audio_sample * align_to_frame (audio_sample * begin, audio_sample * sample, bool align_to_end)
{
    if (! sample)
        return nullptr;

    int offset = sample - begin;
    if (align_to_end)
        offset += current_channels;

    return begin + (offset - offset % current_channels);
}

static void buffer_with_overflow (const audio_sample * data, int len)
{
    int max = buffer.size ();

    if (len > max)
    {
        buffer.move_out (output, -1, -1);
        output.insert (data, -1, len - max);
        buffer.copy_in (data + len - max, max);
    }
    else
    {
        int cur = buffer.len ();
        if (cur + len > max)
            buffer.move_out (output, -1, cur + len - max);

        buffer.copy_in (data, len);
    }
}

Index<audio_sample> & SilenceRemoval::process (Index<audio_sample> & data)
{
    const int threshold_db = aud_get_int ("silence-removal", "threshold");
    const audio_sample threshold = pow (10.0, threshold_db / 20.0);

    audio_sample * first_sample = nullptr;
    audio_sample * last_sample = nullptr;

    for (audio_sample & sample : data)
    {
        if (sample > threshold || sample < -threshold)
        {
            if (! first_sample)
                first_sample = & sample;

            last_sample = & sample;
        }
    }

    first_sample = align_to_frame (data.begin (), first_sample, false);
    last_sample = align_to_frame (data.begin (), last_sample, true);

    output.resize (0);

    if (first_sample)
    {
        /* do not skip leading silence if non-silence has been seen */
        if (! initial_silence)
            first_sample = data.begin ();

        initial_silence = false;

        /* copy any saved silence from previous call */
        buffer.move_out (output, -1, -1);

        /* copy non-silent portion */
        output.insert (first_sample, -1, last_sample - first_sample);

        /* save trailing silence */
        buffer_with_overflow (last_sample, data.end () - last_sample);
    }
    else
    {
        /* if non-silence has been seen, save entire silent chunk */
        if (! initial_silence)
            buffer_with_overflow (data.begin (), data.len ());
    }

    return output;
}

bool SilenceRemoval::flush (bool force)
{
    buffer.discard ();
    output.resize (0);

    initial_silence = true;
    return true;
}
