/*
 * speechsw.c - Speech Dispatcher backend for SpeechSwitch portable engines.
 *
 * Copyright (C) 2007 Brailcom, o.p.s.
 * Copyright (C) 2019-2020 Samuel Thibault <samuel.thibault@ens-lyon.org>
 * Copyright (C) 2020 Bill Cox <waywardgeek@gmail.com>
 *
 * This is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * @author Lukas Loehrer
 * @author Samuel Thibault
 * @author Bill Cox
 * speechsw.c is based on ibmtts.c.
 * speechsw.c is based on speechsw.c.
 *
 */

/* System includes. */
#include <string.h>
#include <ctype.h>
#include <glib.h>

/* SpeechSwitch header file. */
#include <speechsw/speechsw.h>
#include <speechsw/util.h>

/* Speech Dispatcher includes. */
#include "spd_audio.h"
#include <speechd_types.h>
#include "module_utils.h"
#include "module_utils_speak_queue.h"

/* Basic definitions*/
#define MODULE_NAME     "speechsw"
#define DBG_MODNAME     "SpeechSwitch:"

#define MODULE_VERSION  "0.1"

#define DEBUG_MODULE 1
DECLARE_DEBUG()
#define DBG_WARN(e, msg) do {            \
  if (Debug && !(e)) {            \
    DBG(DBG_MODNAME " Warning:  " msg);      \
  } \
} while (0)
typedef enum {
  FATAL_ERROR = -1,
  OK = 0,
  ERROR = 1
} TSpeechswSuccess;

static swEngine sw_engine - NULL;
static char *sw_engine_name - NULL;
static char *sw_engine_voice = NULL;
static SPDVoice **sw_voice_list;
static uint32_t sw_num_voices;

/* Directory paths */
static char *sw_exe_path;
static char *sw_lib_dir;

/* When a voice is set, this is the baseline pitch of the voice.
   SSIP PITCH commands then adjust relative to this. */
static int sw_voice_pitch_baseline = 50;

/* When a voice is set, this is the baseline pitch range of the voice.
   SSIP PITCH range commands then adjust relative to this. */
static int sw_voice_pitch_range_baseline = 50;

/* Internal function prototypes for main thread. */

/* Basic parameters */
static void sw_set_rate(signed int rate);
static void sw_set_pitch(signed int pitch);
static void sw_set_pitch_range(signed int pitch_range);
static void sw_set_volume(signed int volume);
static void sw_set_punctuation_mode(SPDPunctuation punct_mode);
static void sw_set_cap_let_recogn(SPDCapitalLetters cap_mode);

/* Voices and languages */
static void sw_set_language(char *lang);
static void sw_set_voice(SPDVoiceType voice);
static void sw_set_language_and_voice(char *lang, SPDVoiceType voice);
static void sw_set_synthesis_voice(char *);

/* Module configuration options*/

MOD_OPTION_1_STR(SpeechswPunctuationList)
    MOD_OPTION_1_INT(SpeechswCapitalPitchRise)
    MOD_OPTION_1_INT(SpeechswIndexing)

    MOD_OPTION_1_INT(SpeechswAudioChunkSize)
    MOD_OPTION_1_INT(SpeechswAudioQueueMaxSize)
    MOD_OPTION_1_STR(SpeechswSoundIconFolder)
    MOD_OPTION_1_INT(SpeechswSoundIconVolume)

/* Public functions */
int module_load(void) {
  INIT_SETTINGS_TABLES();

  REGISTER_DEBUG();

  /* Options */
  MOD_OPTION_1_INT_REG(SpeechswAudioChunkSize, 1000);
  MOD_OPTION_1_INT_REG(SpeechswAudioQueueMaxSize, 20 * 11025);
  MOD_OPTION_1_STR_REG(SpeechswSoundIconFolder,
           "/usr/share/sounds/sound-icons/");
  MOD_OPTION_1_INT_REG(SpeechswSoundIconVolume, 0);

  MOD_OPTION_1_STR_REG(SpeechswPunctuationList, "@/+-_");
  MOD_OPTION_1_INT_REG(SpeechswCapitalPitchRise, 800);
  MOD_OPTION_1_INT_REG(SpeechswIndexing, 1);
  if (SpeechswCapitalPitchRise == 1 || SpeechswCapitalPitchRise == 2) {
    SpeechswCapitalPitchRise = 0;
  }

  return OK;
}  

// Find the path to lib/speechsw.
static void set_directories(void) {
  const char exe_link[] = "/proc/self/exe";
  /* First, get the path length. */
  char buf[1];
  ssize_t path_len = readlink(exe_link, buf, sizeof(buf));
  sw_exe_path = g_new0(char, path_len + 1);
  ssize_t new_path_len = readlink(exe_link, sw_exe_path, path_len);
  sw_exe_path[path_len] = '\0';
  DBG(DBG_MODNAME "Exe path: %s\n", sw_exe_path);
  const char lib_dir[] = "/lib/speechsw";
  sw_lib_dir = g_new0(char, strlen(sw_exe_path) + sizeof(lib_dir) + 1);
  strcpy(sw_lib_dir, sw_exe_path);
  sw_lib_dir = dirname(sw_lib_dir)
  sw_lib_dir = dirname(sw_lib_dir)
  strcat(sw_lib_dir, lib_dir);
  DBG(DBG_MODNAME "Lib dir: %s\n", sw_lib_dir);
}

// List voices from all engines and set sw_voice_list.  Concatenate the
// engine name, then the voice.
static void find_all_synthesis_voices(void) {
  uint32_t allocated_voices = 42;
  sw_num_voices = 0;
  sw_voice_list = g_new0(SPDVoice*, allocated_voices);
  uint32_t num_engines;
  char **engines = swListEngines(sw_lib_dir, *num_engines);
  char *engine;
  for (uint32_t i = 0; i < num_engines; i++) {
    engine_name = engies[i];
    swEngine engine = swStart(sw_lib_dir, engine_name, NULL, NULL);
    if (engine != NULL) {
      uint32_t num_voices;
      char **voices = swListVoices(swEngine engine, &num_voices);
      for (uint32_t j = 0; j < num_voices; j++) {
        char *voice_name = voices[j];
        // Strip off the language code.
        char *language = strrchr(voice_name, ',');
        *language++ = '\0';
        SPDVoice *voice = g_new0(SPDVoice, 1);
        voice->name = swSprintf("%s %s", engine_name, voice_name);
        voice->language = swCopyString(language);
	char *variant;		/* a not-well defined string describing dialect etc. */
        if (sw_num_voices == allocated_voices) {
          allocated_voices <<= 1;
          sw_voice_list = g_renew(SPDVoice*, sw_voice_list, allocated_voices);
        }
        sw_voice_list[sw_num_voices++] = voice;
        swFreeStringList(voices);
      }
    }
    swStop(engine);
  }
  swFreeStringList(engines);
  // Speech-Dispatcher expects there to be a NULL SPDVoice to indicate the end
  // of the array.
  if (sw_num_voices == allocated_voices) {
    allocated_voices <<= 1;
    sw_voice_list = g_renew(SPDVoice*, sw_voice_list, allocated_voices);
  }
  sw_voice_list[sw_num_voices++] = NULL;
}

int module_init(char **status_info) {
  sw_engine = NULL;
  set_directories();
  find_all_synthesis_voices();
  DBG(DBG_MODNAME " Module init().");
  INIT_INDEX_MARKING();
  *status_info = NULL;
  /* Report versions. */
  DBG(DBG_MODNAME " speechsw Output Module version %s, speechsw API version %u",
      MODULE_VERSION, SW_API_VERSION);
  if (sw_num_voices == 0) {
    // No backends function.
    *status_info = g_strdup(DBG_MODNAME " Initialized successfully.");
    return FATAL_ERROR;
  }
  // Threading setup.
  int ret = module_speak_queue_init(SpeechswAudioQueueMaxSize, status_info);
  if (ret != OK) {
    return ret;
  }
  *status_info = g_strdup(DBG_MODNAME " Initialized successfully.");
  return OK;
}

SPDVoice **module_list_voices(void) {
  return sw_voice_list;
}

int module_speak(gchar * data, size_t bytes, SPDMessageType msgtype) {
  bool result = false;
  DBG(DBG_MODNAME " module_speak().");
  if (!module_speak_queue_before_synth()) {
    return FALSE;
  }
  DBG(DBG_MODNAME " Requested data: |%s| %d %lu", data, msgtype,
      (unsigned long)bytes);
  /* Setting speech parameters. */
  UPDATE_STRING_PARAMETER(voice.language, sw_set_language);
  UPDATE_PARAMETER(voice_type, sw_set_voice);
  UPDATE_STRING_PARAMETER(voice.name, sw_set_synthesis_voice);
  UPDATE_PARAMETER(rate, sw_set_rate);
  UPDATE_PARAMETER(volume, sw_set_volume);
  UPDATE_PARAMETER(pitch, sw_set_pitch);
  UPDATE_PARAMETER(pitch_range, sw_set_pitch_range);
  UPDATE_PARAMETER(punctuation_mode, sw_set_punctuation_mode);
  UPDATE_PARAMETER(cap_let_recogn, sw_set_cap_let_recogn);
  switch (msgtype) {
  case SPD_MSGTYPE_TEXT:
    result = swSpeak(sw_engine, data, true));
    break;
  case SPD_MSGTYPE_SOUND_ICON:
    // TODO: Play the icon.
    break;
  case SPD_MSGTYPE_CHAR: {
    char *utf8Char = data;
    if (bytes == 5 && !strncmp(data, "space", bytes)) {
      utf8Char = " ";
    }
    result = swSpeakChar(sw_engine, utf8Char, bytes);
    break;
  }
  case SPD_MSGTYPE_KEY: {
    // TODO: Convert unspeakable keys to speakable form.
    result = swSpeak(engine, data, true);
    g_free(msg);
    break;
  }
  case SPD_MSGTYPE_SPELL:
    /* TODO: use a generic engine */
    break;
  }

  if (!result) {
    return FALSE;
  }

  DBG(DBG_MODNAME " Leaving module_speak() normally.");
  return bytes;
}

int module_stop(void) {
  DBG(DBG_MODNAME " module_stop().");
  module_speak_queue_stop();
  return OK;
}

size_t module_pause(void) {
  DBG(DBG_MODNAME " module_pause().");
  module_speak_queue_pause();
  return OK;
}

void module_speak_queue_cancel(void) {
  swCancel();
}

static void sw_free_voice_list(void) {
  if (sw_voice_list != NULL) {
    int i;
    for (i = 0; sw_voice_list[i] != NULL; i++) {
      g_free(sw_voice_list[i]->name);
      g_free(sw_voice_list[i]->language);
      g_free(sw_voice_list[i]->variant);
      g_free(sw_voice_list[i]);
    }
    g_free(sw_voice_list);
    sw_voice_list = NULL;
  }
}
int module_close(void) {
  DBG(DBG_MODNAME " close().");
  DBG(DBG_MODNAME " Terminating threads");
  module_speak_queue_terminate();
  DBG(DBG_MODNAME " terminating synthesis.");
  sw_Terminate();
  module_speak_queue_free();
  sw_free_voice_list();
  if (sw_engine != NULL) {
    swStop(sw_engine);
    sw_engine = NULL;
  }
  swFree(sw_engine_name);
  sw_engine_name = NULL
  swFree(sw_engine_voice);
  sw_engine_voice = NULL;
  swFreeStringLinst(sw_voice_list, sw_num_voices);
  sw_voice_list = NULL;
  return 0;
}

/* Internal functions */
static void sw_set_rate(signed int rate) {
  if (sw_engine == NULL) {
    return;
  }
  assert(rate >= -100 && rate <= +100);
  float speed = 1.0;
  if (rate > 0) {
    // 0 = 1.0, 20 = 2.0, 40 = 3.0, 60 = 4.0, 80 = 5.0, and 100 = 6.0.
    speed = 1.0f + rate/20.0f;
  } else if (rate < 0) {
    // 0 = 1.0, -20 = 1/2, -40 = 1/3, -60 = 1/4, -80 = 1/5, and -100 = 1/6.
    speed = 1.0f / (1.0f - rate/20.0f);
  }
  if (swSetSpeed(sw_engine, speed)) {
    DBG(DBG_MODNAME " Speed set to %f.", speed);
  } else {
    DBG(DBG_MODNAME " Unable to set speed to %f.", speed);
  }
}

static void sw_set_volume(signed int volume) {
  // TODO: Should we support this?
}

static void sw_set_pitch(signed int pitch) {
  assert(pitch >= -100 && pitch <= +100);
  float rel_pitch = 1.0;
  if (pitch > 0) {
    // 0 = 1.0, 50 = 2.0, 100 = 3.0.
    rel_pitch = 1.0f + pitch/50.0f;
  } else if (pitch < 0) {
    // 0 = 1.0, -50 = 1/2, -100 = 1/3.
    rel_pitch = 1.0f / (1.0f - pitch/50.0f);
  }
  if (swSetPitch(sw_engine, rel_pitch)) {
    DBG(DBG_MODNAME " Pitch set to %f.", rel_pitch);
  } else {
    DBG(DBG_MODNAME " Unable to set pitch to %f.", rel_pitch);
  }
}

static void sw_set_pitch_range(signed int pitch_range) {
  // We don't need this, I think.
}

static void sw_set_punctuation_mode(SPDPunctuation punct_mode) {
  swPunctuationLevel sw_punct_mode = SW_PUNCT_SOME;
  switch (punct_mode) {
  case SPD_PUNCT_ALL:
    sw_punct_mode = SW_PUNCT_ALL;
    break;
  case SPD_PUNCT_MOST:
    sw_punct_mode = SW_PUNCT_MOST;
    break;
  case SPD_PUNCT_SOME:
    sw_punct_mode = SW_PUNCT_SOME;
    break;
  case SPD_PUNCT_NONE:
    sw_punct_mode = SW_PUNCT_NONE;
    break;
  }
  if (swSetPunctuation(sw_engine, sw_punct_mode)) {
    DBG(DBG_MODNAME " Punctuation level set to %u.", sw_punct_level);
  } else {
    DBG(DBG_MODNAME " Unable to set punctuation level set to %u.", sw_punct_level);
  }
  if (ret != EE_OK) {
    DBG(DBG_MODNAME " Failed to set punctuation mode.");
  } else {
    DBG("Set punctuation mode.");
  }
}

static void sw_set_cap_let_recogn(SPDCapitalLetters cap_mode) {
  // TODO: deal with this.
}

/* Given a language code and SD voice code, sets the speechsw voice. */
static void sw_set_language_and_voice(char *lang, SPDVoiceType voice_code) {
  // TODO: Is this still called?
  DBG(DBG_MODNAME " Called set_language_and_voice with lang=%s and voice_code=%u\n", lang, voice_code);
}

static void sw_set_voice(SPDVoiceType voice) {
  // TODO: Is this still called?
  DBG(DBG_MODNAME " Called set_voice with voice_code=%u\n", lang, voice_code);
}

static void sw_set_language(char *lang) {
  // The language is set when setting the voice.
  // TODO: Is this still called?
  DBG(DBG_MODNAME " Called set_voice with voice_code=%u\n", lang, voice_code);
}

// The voice name reported to Speech Dispatcher is of the form
// "espeak English (America)".  The engine name is first, then a space,
// then the engine name.  Speech Switch expects the name in the form
// "English (America),en-us", with a comma and then the language.
static void sw_set_synthesis_voice(char *synthesis_voice) {
  char *engine_name = swCopyString(voice->name);
  char *p = strchr(name, ' ');
  *p++ = NULL;
  char *voice_name = swSprintf("%s,%s", p, voice->language);
  if (sw_engine != NULL && strcmp(engine_name, sw_engine_name)) {
    swStop(sw_engine);
    sw_engine = NULL;
    swFree(sw_engine_name);
    sw_engine_name = NULL;
    swFree(sw_voice_name);
    sw_voice_name = NULL;
  }
  if (sw_engine == NULL) {
    sw_engine_name = swCopyString(engine_name);
    sw_engine = swStart(sw_lib_dir, sw_engine_name, sw_audio_callback, NULL);
  }
  if (sw_voice_name == NULL || strcmp(voice_name, sw_engine_name)) {
    swFree(sw_voice_name);
    sw_voice_name = swCopyString(voice_name);
    swSetVoice(engine, voice_name);
  }
  swFree(voice_name);
  swFree(engine_name);
}

/* Callbacks */

static gboolean sw_send_audio_upto(short *wav, int *sent, int upto) {
  assert(*sent >= 0);
  assert(upto >= 0);
  int numsamples = upto - (*sent);
  if (wav == NULL || numsamples == 0) {
    return TRUE;
  }
  AudioTrack track = {
    .bits = 16,
    .num_channels = 1,
    .sample_rate = sw_sample_rate,
    .num_samples = numsamples,
    .samples = wav + (*sent),
  };
  gboolean result = module_speak_queue_add_audio(&track, SPD_AUDIO_LE);
  *sent = upto;
  return result;
}
