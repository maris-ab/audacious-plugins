/*
 * Sample Rate Converter Plugin for Audacious
 * Copyright 2010-2012 John Lindgren
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

#include <samplerate.h>

#include <libaudcore/i18n.h>
#include <libaudcore/runtime.h>
#include <libaudcore/plugin.h>
#include <libaudcore/preferences.h>
#include <libaudcore/audstrings.h>

#define MIN_RATE 8000
#define MAX_RATE 192000
#define RATE_STEP 50

#define RESAMPLE_ERROR(e) AUDERR ("%s\n", src_strerror (e))

class Resampler : public EffectPlugin
{
public:
    static const char about[];
    static const char * const defaults[];
    static const PreferencesWidget widgets[];
    static const PluginPreferences prefs;

    static constexpr PluginInfo info = {
        N_("Sample Rate Converter"),
        PACKAGE,
        about,
        & prefs
    };

    /* order #2: must be before crossfade */
    constexpr Resampler () : EffectPlugin (info, 2, false) {}

    bool init ();
    void cleanup ();

    void start (int & channels, int & rate);
    bool flush (bool force);

    Index<audio_sample> & process (Index<audio_sample> & data)
        { return resample (data, false); }
    Index<audio_sample> & finish (Index<audio_sample> & data, bool end_of_playlist)
        { return resample (data, true); }

private:
    Index<audio_sample> & resample (Index<audio_sample> & data, bool finish);
};

EXPORT Resampler aud_plugin_instance;

const char * const Resampler::defaults[] = {
 "method", aud::numeric_string<SRC_SINC_FASTEST>::str,
 "default-rate", "44100",
 "use-mappings", "FALSE",
 "8000", "48000",
 "16000", "48000",
 "22050", "44100",
 "32000", "48000",
 "44100", "44100",
 "48000", "48000",
 "88200", "44100",
 "96000", "48000",
 "176400", "44100",
 "192000", "48000",
 nullptr};

static SRC_STATE * state;
static int stored_channels;
static double ratio;
static Index<audio_sample> outbuffer;

bool Resampler::init ()
{
    aud_config_set_defaults ("resample", defaults);
    return true;
}

void Resampler::cleanup ()
{
    if (state)
    {
        src_delete (state);
        state = nullptr;
    }

    outbuffer.clear ();
}

void Resampler::start (int & channels, int & rate)
{
    if (state)
    {
        src_delete (state);
        state = nullptr;
    }

    int new_rate = 0;

    if (aud_get_bool ("resample", "use-mappings"))
        new_rate = aud_get_int ("resample", int_to_str (rate));

    if (! new_rate)
        new_rate = aud_get_int ("resample", "default-rate");

    new_rate = aud::clamp (new_rate, MIN_RATE, MAX_RATE);

    if (new_rate == rate)
        return;

    int method = aud_get_int ("resample", "method");
    int error;

    if ((state = src_new (method, channels, & error)) == nullptr)
    {
        RESAMPLE_ERROR (error);
        return;
    }

    stored_channels = channels;
    ratio = (double) new_rate / rate;
    rate = new_rate;
}

Index<audio_sample> & Resampler::resample (Index<audio_sample> & data, bool finish)
{
    if (! state || ! data.len ())
        return data;

#ifdef DEF_AUDIO_FLOAT64
    static Index<float> floatbuf_in;
    floatbuf_in.resize (data.len ());
    float * fout = floatbuf_in.begin ();
    audio_sample * fin = data.begin ();
    const audio_sample * fend = data.end ();
    while (fin < fend)
    {
        *(fout++) = *(fin++);
    }
    static Index<float> floatbuf_out;
    floatbuf_out.resize ((int) (data.len () * ratio) + 256);

    SRC_DATA srcd = SRC_DATA ();
    srcd.data_in = floatbuf_in.begin ();
    srcd.data_out = floatbuf_out.begin ();
    srcd.output_frames = floatbuf_out.len () / stored_channels;
#else
    outbuffer.resize ((int) (data.len () * ratio) + 256);

    SRC_DATA srcd = SRC_DATA ();

    srcd.data_in = data.begin ();
    srcd.data_out = outbuffer.begin ();
    srcd.output_frames = outbuffer.len () / stored_channels;
#endif

    srcd.input_frames = data.len () / stored_channels;
    srcd.src_ratio = ratio;
    srcd.end_of_input = finish;

    int error;
    if ((error = src_process (state, & srcd)))
    {
        RESAMPLE_ERROR (error);
        return data;
    }

#ifdef DEF_AUDIO_FLOAT64
    floatbuf_in.resize (0);
    outbuffer.resize (stored_channels * srcd.output_frames_gen);
    float * ain = floatbuf_out.begin ();
    audio_sample * aout = outbuffer.begin ();
    const audio_sample * aend = aout + (stored_channels * srcd.output_frames_gen);
    while (aout < aend)
    {
        *(aout++) = *(ain++);
    }
    floatbuf_out.resize (0);
#else
    outbuffer.resize (stored_channels * srcd.output_frames_gen);
#endif

    if (finish)
        flush (true);

    return outbuffer;
}

bool Resampler::flush (bool force)
{
    int error;
    if (state && (error = src_reset (state)))
        RESAMPLE_ERROR (error);

    return true;
}

const char Resampler::about[] =
 N_("Sample Rate Converter Plugin for Audacious\n"
    "Using Float32 bit\n"
    "Copyright 2010-2012 John Lindgren");

static const ComboItem method_list[] = {
    ComboItem(N_("Skip/repeat samples"), SRC_ZERO_ORDER_HOLD),
    ComboItem(N_("Linear interpolation"), SRC_LINEAR),
    ComboItem(N_("Fast sinc interpolation"), SRC_SINC_FASTEST),
    ComboItem(N_("Medium sinc interpolation"), SRC_SINC_MEDIUM_QUALITY),
    ComboItem(N_("Best sinc interpolation"), SRC_SINC_BEST_QUALITY)
};

const PreferencesWidget Resampler::widgets[] = {
    WidgetLabel (N_("<b>Conversion</b>")),
    WidgetCombo (N_("Method:"),
        WidgetInt ("resample", "method"),
        {{method_list}}),
    WidgetSpin (N_("Rate:"),
        WidgetInt ("resample", "default-rate"),
        {MIN_RATE, MAX_RATE, RATE_STEP, N_("Hz")}),
    WidgetLabel (N_("<b>Rate Mappings</b>")),
    WidgetCheck (N_("Use rate mappings"),
        WidgetBool ("resample", "use-mappings")),
    WidgetSpin (N_("8 kHz:"),
        WidgetInt ("resample", "8000"),
        {MIN_RATE, MAX_RATE, RATE_STEP, N_("Hz")},
        WIDGET_CHILD),
    WidgetSpin (N_("16 kHz:"),
        WidgetInt ("resample", "16000"),
        {MIN_RATE, MAX_RATE, RATE_STEP, N_("Hz")},
        WIDGET_CHILD),
    WidgetSpin (N_("22.05 kHz:"),
        WidgetInt ("resample", "22050"),
        {MIN_RATE, MAX_RATE, RATE_STEP, N_("Hz")},
        WIDGET_CHILD),
    WidgetSpin (N_("32.0 kHz:"),
        WidgetInt ("resample", "32000"),
        {MIN_RATE, MAX_RATE, RATE_STEP, N_("Hz")},
        WIDGET_CHILD),
    WidgetSpin (N_("44.1 kHz:"),
        WidgetInt ("resample", "44100"),
        {MIN_RATE, MAX_RATE, RATE_STEP, N_("Hz")},
        WIDGET_CHILD),
    WidgetSpin (N_("48 kHz:"),
        WidgetInt ("resample", "48000"),
        {MIN_RATE, MAX_RATE, RATE_STEP, N_("Hz")},
        WIDGET_CHILD),
    WidgetSpin (N_("88.2 kHz:"),
        WidgetInt ("resample", "88200"),
        {MIN_RATE, MAX_RATE, RATE_STEP, N_("Hz")},
        WIDGET_CHILD),
    WidgetSpin (N_("96 kHz:"),
        WidgetInt ("resample", "96000"),
        {MIN_RATE, MAX_RATE, RATE_STEP, N_("Hz")},
        WIDGET_CHILD),
    WidgetSpin (N_("176.4 kHz:"),
        WidgetInt ("resample", "176400"),
        {MIN_RATE, MAX_RATE, RATE_STEP, N_("Hz")},
        WIDGET_CHILD),
    WidgetSpin (N_("192 kHz:"),
        WidgetInt ("resample", "192000"),
        {MIN_RATE, MAX_RATE, RATE_STEP, N_("Hz")},
        WIDGET_CHILD)
};

const PluginPreferences Resampler::prefs = {{widgets}};
