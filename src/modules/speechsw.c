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
#include <ctype.h>
#include <glib.h>
#include <pthread.h>
#include <string.h>

/* SpeechSwitch header file. */
#include <speechsw/speechsw.h>
#define SW_DEBUG 1  // Causes Speech Switch to call swLog for loging.
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
#define DBG_WARN(e, msg) do { \
  if (Debug && !(e)) { \
    LOG("Warning:  " msg); \
  } \
} while (0)

typedef enum {
  FATAL_ERROR = -1,
  OK = 0,
  ERROR = 1
} TSpeechswSuccess;

static swEngine sw_engine = NULL;
static uint32_t sw_sample_rate = 0;
static char *sw_engine_name = NULL;
static char *sw_voice_name = NULL;
static SPDVoice **sw_voice_list;
static uint32_t sw_num_voices;
static volatile bool sw_cancel = false;
static char **sw_engines;
uint32_t sw_num_engines;

/* Directory paths */
static char *sw_exe_path;
static char *sw_lib_dir;

// Log a voice.
static void log_voice(const SPDVoice *voice) {
  swLog("voice->name = %s, voice->language = %s, voice->variant = %s\n",
      voice->name, voice->language, voice->variant);
}

// Log the voices in sw_voice_list.
static void log_voice_list(void) {
  if (sw_voice_list == NULL) {
    swLog("sw_voice_list is NULL.\n");
    return;
  }
  for (SPDVoice **voice_ptr = sw_voice_list; *voice_ptr != NULL; voice_ptr++) {
    SPDVoice *voice = *voice_ptr;
    log_voice(voice);
  }
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

// This callback is called by Speech Switch to process audio samples from the
// speech engine.  Return true to cancel speech synthesis.
static bool audio_callback(swEngine engine, int16_t *samples, uint32_t num_samples,
    bool cancel, void *callbackContext) {
  if (cancel || sw_cancel) {
    module_speak_queue_stop();
    swLog("Canceling\n");
    return true;
  }
  if (module_speak_queue_stop_requested()) {
    sw_cancel = true;
    swLog("Canceling\n");
    return true;
  }
  if (num_samples == 0) {
    // This indicates end of synthesis.
    swLog("End of speech samples\n");
    module_speak_queue_before_play();
    module_speak_queue_add_end();
    return false;
  }
  AudioTrack track = {
    .bits = 16,
    .num_channels = 1,
    .sample_rate = sw_sample_rate,
    .num_samples = num_samples,
    .samples = samples
  };
  swLog("Speaking before play\n");
  module_speak_queue_before_play();
  swLog("Sending %u samples to audio player\n", num_samples);
  if (!module_speak_queue_add_audio(&track, SPD_AUDIO_LE)) {
    swLog("module_speak_queue_add_audio failed for some reason\n");
    return true;  // Causes current synthesis to end.
  }
  swLog("Completed sending samples to audio player\n");
  return sw_cancel;
}

static void set_rate(signed int rate) {
  swLog("Called set_rate with rate = %d\n", rate);
  if (sw_engine == NULL) {
    swLog("No engine to set rate on.\n");
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
    swLog("Speed set to %f.\n", speed);
  } else {
    swLog("Unable to set speed to %f.\n", speed);
  }
}

static void set_volume(signed int volume) {
  // TODO: Should we support this?
  swLog("Called set_volume = %d\n", volume);
}

static void set_pitch(signed int pitch) {
  swLog("Called set_pitch = %d\n", pitch);
  if (sw_engine == NULL) {
    swLog("No engine to set pitch on.\n");
    return;
  }
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
    swLog("Pitch set to %f.\n", rel_pitch);
  } else {
    swLog("Unable to set pitch to %f.\n", rel_pitch);
  }
}

static void set_pitch_range(signed int pitch_range) {
  // We don't need this, I think.
  swLog("Called set_pitch_range = %d\n", pitch_range);
}

static void set_punctuation_mode(SPDPunctuation punct_mode) {
  swLog("Called set_punctuation_mode = %d\n", punct_mode);
  if (sw_engine == NULL) {
    swLog("No engine to set punctuation mode on.\n");
    return;
  }
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
    swLog("Punctuation level set to %u.\n", sw_punct_mode);
  } else {
    swLog("Unable to set punctuation level set to %u.\n", sw_punct_mode);
  }
}

static void set_cap_let_recogn(SPDCapitalLetters cap_mode) {
  // TODO: deal with this.
  swLog("Called set_cap_let_recogn = %d\n", cap_mode);
}

static void set_voice(SPDVoiceType voice) {
  // TODO: Is this still called?
  swLog("Called set_voice with voice code=%d\n", voice);
}

static void set_language(char *lang) {
  // The language is set when setting the voice.
  // TODO: Is this still called?
  swLog("Called set_lang with lange=%s\n", lang);
}

// Start the engine with the given name.
static void start_engine(const char *engine_name) {
  if (sw_engine != NULL) {
    if (!strcmp(engine_name, sw_engine_name)) {
      // Already started.
      swLog("Engine %s already started\n", engine_name);
      return;
    }
    stop_engine();
  }
  swLog("Starting engine %s\n", engine_name);
  sw_engine = swStart(sw_lib_dir, engine_name, audio_callback, NULL);
  if (sw_engine == NULL) {
    swLog("Unable to start engine %s\n", engine_name);
    return;
  }
  sw_engine_name = swCopyString(engine_name);
  sw_sample_rate = swGetSampleRate(sw_engine);
  // Looks like CLEAN_OLD_SETTINGS_TABLE does not free these.
  g_free(msg_settings_old.voice.name);
  g_free(msg_settings_old.voice.language);
  CLEAN_OLD_SETTINGS_TABLE();
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
static void set_synthesis_voice(char *synthesis_voice) {
  swLog("Called set_synthesis_voice with voice=%s\n", synthesis_voice);
  SPDVoice *voice = find_voice(synthesis_voice);
  if (voice == NULL) {
    swLog("In set_synthesis_voice: Unknown synthesis voice: %s\n", synthesis_voice);
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

MOD_OPTION_1_STR(SpeechswPunctuationList)
    MOD_OPTION_1_INT(SpeechswCapitalPitchRise)
    MOD_OPTION_1_INT(SpeechswIndexing)

    MOD_OPTION_1_INT(SpeechswAudioChunkSize)
    MOD_OPTION_1_INT(SpeechswAudioQueueMaxSize)
    MOD_OPTION_1_STR(SpeechswSoundIconFolder)
    MOD_OPTION_1_INT(SpeechswSoundIconVolume)

// Find the path to lib/speechsw.
static void set_directories(void) {
  swLog("Called set_directories\n");
  const char exe_link[] = "/proc/self/exe";
  /* First, get the path length. */
  char exe_path[1024 + 1];
  ssize_t path_len = readlink(exe_link, exe_path, sizeof(exe_path) - 1);
  exe_path[path_len] = '\0';
  sw_exe_path = swCopyString(exe_path);
  swLog("Exe path: %s\n", sw_exe_path);
  char *modules_dir = g_path_get_dirname(sw_exe_path);
  char *lib_exec_dir = g_path_get_dirname(modules_dir);
  g_free(modules_dir);
  sw_lib_dir = swCatStrings(lib_exec_dir, "/speechsw");
  g_free(lib_exec_dir);
  swLog("lib dir: %s\n", sw_lib_dir);
}

// Select a default engine if none was set.
static void set_default_engine(void) {
  swLog("Setting default engine\n");
  if (sw_num_engines == 0) {
    swLog("No engines found.\n");
    return;
  }
  start_engine("espeak");
  if (sw_engine != NULL) {
    return;
  }
  for (uint32_t i = 0; i < sw_num_engines; i++) {
    start_engine(sw_engines[i]);
    if (sw_engine != NULL) {
      return;
    }
  }
  swLog("All engines fail to start.\n");
}

// All backend engines report locale in lower case, e.g. en-us.  However, Orca
// requires locale to be upper case, e.g. en-US.  Onvert everything after the -
// to upper case for Orca.  Caller is responsible for freeing result.
static char *capitalizeLocale(char *language) {
  char *l = swCopyString(language);
  char *p = strchr(l, '-');
  if (p == NULL) {
    return l;
  }
  while (*p != '\0') {
    *p++ = toupper(*p);
  }
  return l;
}

// List voices from all engines and set sw_voice_list.  Concatenate the
// engine name, then the voice.
static void find_all_synthesis_voices(void) {
  swLog("find_all_synthesis_voices called.\n");
  uint32_t allocated_voices = 42;
  sw_num_voices = 0;
  sw_voice_list = g_new0(SPDVoice*, allocated_voices);
  sw_engines = swListEngines(sw_lib_dir, &sw_num_engines);
  for (uint32_t i = 0; i < sw_num_engines; i++) {
    const char *engine_name = sw_engines[i];
    swEngine engine = swStart(sw_lib_dir, engine_name, NULL, NULL);
    if (engine == NULL) {
      swLog("Could not start %s\n", engine_name);
    } else {
      uint32_t num_voices;
      char **voices = swListVoices(engine, &num_voices);
      for (uint32_t j = 0; j < num_voices; j++) {
        char *voice_name = voices[j];
        // Strip off the language code.
        char *language = strrchr(voice_name, ',');
        *language++ = '\0';
        SPDVoice *voice = g_new0(SPDVoice, 1);
        voice->name = swSprintf("%s %s", engine_name, voice_name);
        voice->language = capitalizeLocale(language);
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
  // Speech-Dispatcher expects there to be a NULL SPDVoice to indicate the end
  // of the array.
  if (sw_num_voices == allocated_voices) {
    allocated_voices <<= 1;
    sw_voice_list = g_renew(SPDVoice*, sw_voice_list, allocated_voices);
  }
  sw_voice_list[sw_num_voices++] = NULL;
  if (SW_DEBUG) {
    log_voice_list();
  }
}

/* Public functions */
int module_load(void) {
  swLog("Called module_load\n");
  set_directories();
  find_all_synthesis_voices();
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
  swLog("module_init_called.\n");
  INIT_INDEX_MARKING();
  *status_info = NULL;
  /* Report versions. */
  swLog("speechsw Output Module version %s, speechsw API version %u\n",
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
  swLog("module_list_voices called.\n");
  return sw_voice_list;
}

static void speak(char *data, size_t bytes, SPDMessageType msgtype) {
  // Oddly, data appears to not be zero-terminated, so make a copy and terminate it.
  char *text = NULL;
  if (bytes != 0) {
    text = swCalloc(bytes + 1, sizeof(char));
  }
  memcpy(text, data, bytes);
  text[bytes] = '\0';
  swLog("Called speak\n");
  bool result = true;
  switch (msgtype) {
  case SPD_MSGTYPE_TEXT: {
    char *out = module_strip_ssml(text);
    swLog("SPEAK %s\n", out);
    sw_cancel = false;
    result = swSpeak(sw_engine, out, true);
    swLog("Sent '%s' to synthesizer speech\n", out);
    swFree(out);
    break;
  }
  case SPD_MSGTYPE_SOUND_ICON:
    // TODO: Play the icon.
    swLog("Ignoring sound icon\n");
    break;
  case SPD_MSGTYPE_CHAR: {
    char *utf8Char = text;
    if (bytes == 5 && !strncmp(text, "space", bytes)) {
      utf8Char = " ";
    }
    sw_cancel = false;
    swLog("Calling swSpeakChar with %s\n", utf8Char);
    result = swSpeakChar(sw_engine, utf8Char, bytes);
    swLog("Finished swSpeakCahr\n", utf8Char);
    break;
  }
  case SPD_MSGTYPE_KEY: {
    // TODO: Convert unspeakable keys to speakable form.
    sw_cancel = false;
    swLog("Calling swSpeak for key %s\n", text);
    result = swSpeak(sw_engine, text, true);
    break;
  }
  case SPD_MSGTYPE_SPELL:
    /* TODO: use a generic engine */
    swLog("Ignoring spell message\n");
    break;
  default:
    swLog("Ignoring event %u\n", msgtype);
  }
  if (!result) {
    swLog("Returning FALSE from speak.\n");
  }
  swFree(text);
  swLog("Leaving speak() normally.\n");
}

// These globals are used to pass parameters to module_speak to speak.
static char *sw_speak_data;
static size_t sw_speak_bytes;
static SPDMessageType sw_speak_msgtype;

static void *speak_wrapper(void *nothing) {
  speak(sw_speak_data, sw_speak_bytes, sw_speak_msgtype);
  swFree(sw_speak_data);
  pthread_exit(NULL);
}

// These are used by module_speak.
static bool sw_speaking = false;
static pthread_t sw_speak_thread;

// module_speak is required to return before speech is synthesized.  This
// wrapper enables this, since swSpeak blocks.
int module_speak(char *data, size_t bytes, SPDMessageType msgtype) {
  swLog("Called module_speak.\n");
  module_speak_queue_before_synth();
  if (sw_speaking) {
    pthread_join(sw_speak_thread, NULL);
    sw_speaking = false;
  }
  // Try to select the engine and voice first.
  UPDATE_STRING_PARAMETER(voice.name, set_synthesis_voice);
  if (sw_engine == NULL) {
    set_default_engine();
    if (sw_engine == NULL) {
      swLog("No engine set\n");
      return FALSE;
    }
  }
  // Set speech parameters.
  UPDATE_STRING_PARAMETER(voice.language, set_language);
  UPDATE_PARAMETER(voice_type, set_voice);
  UPDATE_PARAMETER(rate, set_rate);
  UPDATE_PARAMETER(volume, set_volume);
  UPDATE_PARAMETER(pitch, set_pitch);
  UPDATE_PARAMETER(pitch_range, set_pitch_range);
  UPDATE_PARAMETER(punctuation_mode, set_punctuation_mode);
  UPDATE_PARAMETER(cap_let_recogn, set_cap_let_recogn);
  // Pass parameters to speak thread through globals.
  sw_speak_data = swCalloc(bytes, sizeof(char));
  memcpy(sw_speak_data, data, bytes);
  sw_speak_bytes = bytes;
  sw_speak_msgtype = msgtype;
  if (pthread_create(&sw_speak_thread, NULL, speak_wrapper, NULL) != 0) {
    return FALSE;
  }
  sw_speaking = true;
  return bytes;
}

int module_stop(void) {
  swLog("called module_stop\n");
  if (sw_engine != NULL) {
    sw_cancel = true;
  }
  module_speak_queue_stop();
  return OK;
}

size_t module_pause(void) {
  swLog("module_pause().");
  module_speak_queue_pause();
  return OK;
}

void module_speak_queue_cancel(void) {
  swLog("Called module_speak_queue_cancel\n");
  if (sw_engine != NULL) {
    sw_cancel = true;
  }
}

static void free_voice_list(void) {
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
  swLog("called module_close\n");
  stop_engine();
  module_speak_queue_terminate();
  swLog("terminating synthesis.");
  module_speak_queue_free();
  free_voice_list();
  swFreeStringList(sw_engines, sw_num_engines);
  sw_engines = NULL;
  sw_num_engines = 0;
  return 0;
}
