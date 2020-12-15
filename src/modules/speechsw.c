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
    LOG("Warning:  " msg);      \
  } \
} while (0)
typedef enum {
  FATAL_ERROR = -1,
  OK = 0,
  ERROR = 1
} TSpeechswSuccess;

static FILE *dbg_file;
static swEngine sw_engine = NULL;
static uint32_t sw_sample_rate = 0;
static char *sw_engine_name = NULL;
static char *sw_voice_name = NULL;
static SPDVoice **sw_voice_list;
static uint32_t sw_num_voices;

/* Directory paths */
static char *sw_exe_path;
static char *sw_lib_dir;

// Create a formatted string.  Caller is responsible for freeing result.
void LOG(const char *format, ...) {
  va_list ap;
  va_start(ap, format);
  vfprintf(dbg_file, format, ap);
  va_end(ap);
  fflush(dbg_file);
}

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
static void sw_set_synthesis_voice(char *);

/* Module configuration options*/

MOD_OPTION_1_STR(SpeechswPunctuationList)
    MOD_OPTION_1_INT(SpeechswCapitalPitchRise)
    MOD_OPTION_1_INT(SpeechswIndexing)

    MOD_OPTION_1_INT(SpeechswAudioChunkSize)
    MOD_OPTION_1_INT(SpeechswAudioQueueMaxSize)
    MOD_OPTION_1_STR(SpeechswSoundIconFolder)
    MOD_OPTION_1_INT(SpeechswSoundIconVolume)

// Find the path to lib/speechsw.
static void set_directories(void) {
  LOG("Called set_directories\n");
  const char exe_link[] = "/proc/self/exe";
  /* First, get the path length. */
  char exe_path[1024 + 1];
  ssize_t path_len = readlink(exe_link, exe_path, sizeof(exe_path) - 1);
  exe_path[path_len] = '\0';
  sw_exe_path = swCopyString(exe_path);
  LOG("Exe path: %s\n", sw_exe_path);
  char *modules_dir = g_path_get_dirname(sw_exe_path);
  char *lib_exec_dir = g_path_get_dirname(modules_dir);
  g_free(modules_dir);
  sw_lib_dir = swCatStrings(lib_exec_dir, "/speechsw");
  g_free(lib_exec_dir);
  LOG("lib dir: %s\n", sw_lib_dir);
}

// This callback is called by Speech Switch to process audio samples from the
// speech engine.  Return true to cancel speech synthesis.
static bool audio_callback(swEngine engine, int16_t *samples, uint32_t num_samples,
    bool cancel, void *callbackContext) {
  if (cancel) {
    module_speak_queue_stop();
    return true;
  }
  AudioTrack track = {
    .bits = 16,
    .num_channels = 1,
    .sample_rate = sw_sample_rate,
    .num_samples = num_samples,
    .samples = samples
  };
  return !module_speak_queue_add_audio(&track, SPD_AUDIO_LE);
}

// Log a voice.
static void log_voice(const SPDVoice *voice) {
  LOG("voice->name = %s, voice->language = %s, voice->variant = %s\n",
      voice->name, voice->language, voice->variant);
}

// Log the voices in sw_voice_list.
static void log_voice_list(void) {
  if (sw_voice_list == NULL) {
    LOG("sw_voice_list is NULL.\n");
    return;
  }
  for (SPDVoice **voice_ptr = sw_voice_list; *voice_ptr != NULL; voice_ptr++) {
    SPDVoice *voice = *voice_ptr;
    log_voice(voice);
  }
}

// Start the engine with the given name.
static void start_engine(const char *engine_name) {
  LOG("Starting engine %s\n", engine_name);
  sw_engine_name = swCopyString(engine_name);
  sw_engine = swStart(sw_lib_dir, sw_engine_name, audio_callback, NULL);
  sw_sample_rate = swGetSampleRate(sw_engine);
}

// Stop the current engine.
static void stop_engine(void) {
  if (sw_engine == NULL) {
    return;
  }
  swStop(sw_engine);
  sw_engine = NULL;
  swFree(sw_engine_name);
  sw_engine_name = NULL;
  swFree(sw_voice_name);
  sw_voice_name = NULL;
  sw_sample_rate = 0;
}

// List voices from all engines and set sw_voice_list.  Concatenate the
// engine name, then the voice.
static void find_all_synthesis_voices(void) {
  LOG("find_all_synthesis_voices called.\n");
  uint32_t allocated_voices = 42;
  sw_num_voices = 0;
  sw_voice_list = g_new0(SPDVoice*, allocated_voices);
  uint32_t num_engines;
  char **engines = swListEngines(sw_lib_dir, &num_engines);
  const char *default_engine_name = NULL;
  for (uint32_t i = 0; i < num_engines; i++) {
    const char *engine_name = engines[i];
    swEngine engine = swStart(sw_lib_dir, engine_name, NULL, NULL);
    if (engine == NULL) {
      LOG("Could not start %s\n", engine_name);
    } else {
      uint32_t num_voices;
      char **voices = swListVoices(engine, &num_voices);
      for (uint32_t j = 0; j < num_voices; j++) {
        if (default_engine_name == NULL || !strcmp(engine_name, "espeak")) {
          default_engine_name = engine_name;
        }
        char *voice_name = voices[j];
        // Strip off the language code.
        char *language = strrchr(voice_name, ',');
        *language++ = '\0';
        SPDVoice *voice = g_new0(SPDVoice, 1);
        voice->name = swSprintf("%s %s", engine_name, voice_name);
        voice->language = swCopyString(language);
        voice->variant = "null";
        if (sw_num_voices == allocated_voices) {
          allocated_voices <<= 1;
          sw_voice_list = g_renew(SPDVoice*, sw_voice_list, allocated_voices);
        }
        sw_voice_list[sw_num_voices++] = voice;
      }
      swFreeStringList(voices, num_voices);
      swStop(engine);
    }
  }
  if (default_engine_name != NULL) {
    start_engine(default_engine_name);
  }
  swFreeStringList(engines, num_engines);
  // Speech-Dispatcher expects there to be a NULL SPDVoice to indicate the end
  // of the array.
  if (sw_num_voices == allocated_voices) {
    allocated_voices <<= 1;
    sw_voice_list = g_renew(SPDVoice*, sw_voice_list, allocated_voices);
  }
  sw_voice_list[sw_num_voices++] = NULL;
  if (DEBUG_MODULE) {
    log_voice_list();
  }
  fflush(dbg_file);
}

/* Public functions */
int module_load(void) {
  if (dbg_file == NULL) {
    dbg_file = fopen("/tmp/speechsw.log", "w");
  }
  set_directories();
  find_all_synthesis_voices();
  LOG("Called module_load\n");
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

int module_init(char **status_info) {
  LOG("module_init_called.\n");
  INIT_INDEX_MARKING();
  *status_info = NULL;
  /* Report versions. */
  LOG("speechsw Output Module version %s, speechsw API version %u\n",
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
  LOG("module_list_voices called.\n");
  return sw_voice_list;
}

int module_speak(gchar * data, size_t bytes, SPDMessageType msgtype) {
  LOG("Called module_speak\n");
  bool result = false;
  if (!module_speak_queue_before_synth()) {
    return FALSE;
  }
  LOG("Requested data: |%s| %d %lu\n", data, msgtype, (unsigned long)bytes);
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
    result = swSpeak(sw_engine, data, true);
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
    result = swSpeak(sw_engine, data, true);
    break;
  }
  case SPD_MSGTYPE_SPELL:
    /* TODO: use a generic engine */
    break;
  }

  if (!result) {
    return FALSE;
  }

  LOG("Leaving module_speak() normally.");
  return bytes;
}

int module_stop(void) {
  LOG("module_stop().");
  module_speak_queue_stop();
  return OK;
}

size_t module_pause(void) {
  LOG("module_pause().");
  module_speak_queue_pause();
  return OK;
}

void module_speak_queue_cancel(void) {
  LOG("Called module_speak_queue_cancel\n");
  swCancel(sw_engine);
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
  LOG("called close\n");
  LOG("Terminating threads");
  module_speak_queue_terminate();
  LOG("terminating synthesis.");
  module_speak_queue_free();
  sw_free_voice_list();
  stop_engine();
  if (dbg_file != NULL) {
    fclose(dbg_file);
  }
  return 0;
}

/* Internal functions */
static void sw_set_rate(signed int rate) {
  LOG("Called sw_set_rate with rate = %d\n", rate);
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
    LOG("Speed set to %f.\n", speed);
  } else {
    LOG("Unable to set speed to %f.\n", speed);
  }
}

static void sw_set_volume(signed int volume) {
  // TODO: Should we support this?
  LOG("Called sw_set_volume = %d\n", volume);
}

static void sw_set_pitch(signed int pitch) {
  LOG("Called sw_set_pitch = %d\n", pitch);
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
    LOG("Pitch set to %f.\n", rel_pitch);
  } else {
    LOG("Unable to set pitch to %f.\n", rel_pitch);
  }
}

static void sw_set_pitch_range(signed int pitch_range) {
  // We don't need this, I think.
  LOG("Called sw_set_pitch_range = %d\n", pitch_range);
}

static void sw_set_punctuation_mode(SPDPunctuation punct_mode) {
  LOG("Called sw_set_punctuation_mode = %d\n", punct_mode);
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
    LOG("Punctuation level set to %u.\n", sw_punct_mode);
  } else {
    LOG("Unable to set punctuation level set to %u.\n", sw_punct_mode);
  }
}

static void sw_set_cap_let_recogn(SPDCapitalLetters cap_mode) {
  // TODO: deal with this.
  LOG("Called sw_set_cap_let_recogn = %d\n", cap_mode);
}

static void sw_set_voice(SPDVoiceType voice) {
  // TODO: Is this still called?
  LOG("Called set_voice with voice code=%d\n", voice);
}

static void sw_set_language(char *lang) {
  // The language is set when setting the voice.
  // TODO: Is this still called?
  LOG("Called set_lang with lange=%s\n", lang);
}

// Find the voice by name from sw_voice_list;
static SPDVoice *find_voice(const char *synthesis_voice) {
  for (SPDVoice **voice_ptr = sw_voice_list; *voice_ptr != NULL; voice_ptr++) {
    SPDVoice *voice = *voice_ptr;
    if (!strcmp(voice->name, synthesis_voice)) {
      return voice;
    }
  }
  return NULL;
}

// The voice name reported to Speech Dispatcher is of the form
// "espeak English (America)".  The engine name is first, then a space,
// then the engine name.  Speech Switch expects the name in the form
// "English (America),en-us", with a comma and then the language.
static void sw_set_synthesis_voice(char *synthesis_voice) {
  LOG("Called set_synthesis_voice with voice=%s\n", synthesis_voice);
  SPDVoice *voice = find_voice(synthesis_voice);
  if (voice == NULL) {
    LOG("In sw_set_synthesis_voice: Unknown synthesis voice: %s\n", synthesis_voice);
    return;
  }
  char *engine_name = swCopyString(voice->name);
  char *p = strchr(engine_name, ' ');
  *p++ = '\0';
  char *voice_name = swSprintf("%s,%s", p, voice->language);
  if (sw_engine != NULL && strcmp(engine_name, sw_engine_name)) {
    stop_engine();
  }
  if (sw_engine == NULL) {
    start_engine(engine_name);
  }
  if (sw_voice_name == NULL || strcmp(voice_name, sw_engine_name)) {
    swFree(sw_voice_name);
    sw_voice_name = swCopyString(voice_name);
    swSetVoice(sw_engine, voice_name);
  }
  swFree(voice_name);
  swFree(engine_name);
}
