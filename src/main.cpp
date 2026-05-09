// ESP32-S3 Fox Hunt Voice Beacon
//
// Plays speech audio over I2S or PWM along with configurable tones for
// amateur radio direction finding (fox hunting).
//
// Speech source (controlled by USE_FLITE):
//   USE_FLITE=1: Synthesizes speech on-device using Flite TTS, caches result
//                to SPIFFS. Subsequent boots load from cache (~ms vs ~24s).
//   USE_FLITE=0: Loads a pre-generated WAV file from SPIFFS. Use the
//                scripts/wav2speech.py tool to convert any WAV file to the
//                required format, then upload with: pio run --target uploadfs
//
// Cache/speech format: 4 little-endian int32 header
//   [version, sample_rate, num_samples, num_channels]
//   followed by raw int16 PCM sample data.
//   In Flite mode, bump SPEECH_VERSION to invalidate the cache.
//
// LED states (RGB built-in, runs on Core 0):
//   Red blink   - Synthesizing speech (Flite mode, no cache)
//   Blue blink  - Loading speech from SPIFFS
//   Green blink - Waiting between playback cycles
//   Off         - Playing audio
//
// This is the command to use in the pioarduino terminal to
// process the WAV file to the proper format to store in flash:
// scripts/wav2speech.py ve1fo-adam.wav
//
// This is the command to upload upload the processed WAV file to flash:
// pio run --target uploadfs  (will pause at 99% for about 60 seconds)    


#include <math.h>
#include "AudioTools.h"
#include <SPIFFS.h>

#define USE_FLITE 0             // 1 = Flite TTS synthesis, 0 = pre-uploaded WAV on SPIFFS
#define SPEECH_VERSION 12       // bump to force re-synthesis (USE_FLITE=1 only)
#define DURATION_STRETCH 1.15f  // >1.0 = slower speech (USE_FLITE=1 only)
#define SPEECH_GAIN 0.10f        // volume: <1.0 = quieter, >1.0 = louder

//#define OUTPUT_PWM  // Comment out to use I2S with PCM5102

#if USE_FLITE
#include "flite_arduino.h"
#else
// Minimal cst_wave-compatible struct for pre-built speech mode.
// Mirrors the Flite cst_wave layout so loadCachedWave() and playback
// code work unchanged regardless of USE_FLITE setting.
struct cst_wave
{
    int sample_rate;
    int num_samples;
    int num_channels;
    short *samples;
};
static cst_wave *new_wave()
{
    cst_wave *w = (cst_wave *)calloc(1, sizeof(cst_wave));
    return w;
}
static int cst_wave_num_samples(cst_wave *w) { return w->num_samples; }
static int cst_wave_sample_rate(cst_wave *w) { return w->sample_rate; }
#endif

#define PTT_PIN  2

#ifdef OUTPUT_PWM
#define PWM_PIN 40
#else
// I2S pins for ESP32-S3
#define I2S_BCK  21
#define I2S_WS   47
#define I2S_DOUT 38
#endif



#define CACHE_PATH "/speech.raw"
#define TONE_FREQ   600     // tone frequency in Hz
#define TONE_AMP    4000    // tone amplitude (max 32767)
#define SAMPLE_RATE 16000   // must match I2S/PWM config

#ifdef OUTPUT_PWM
// Simple blocking PWM audio output using ESP32 LEDC.
// Converts 16-bit signed samples to 8-bit duty cycle and busy-waits
// to maintain sample rate timing. Avoids the library's broken timer code.
class SimplePWMAudio
{
    uint8_t _pin;
    uint32_t _samplePeriodUs;

public:
    void begin(uint8_t pin, uint32_t sampleRate)
    {
        _pin = pin;
        _samplePeriodUs = 1000000 / sampleRate;
        ledcAttach(pin, 156250, 8);  // 156kHz PWM, 8-bit resolution
        ledcWrite(pin, 128);         // idle at midpoint (silence)
    }

    size_t write(const uint8_t *data, size_t len)
    {
        const int16_t *samples = (const int16_t *)data;
        size_t numSamples = len / sizeof(int16_t);
        uint32_t nextTime = micros();

        for(size_t i = 0; i < numSamples; i++)
        {
            // Convert signed 16-bit (-32768..32767) to unsigned 8-bit (0..255)
            uint8_t duty = (uint8_t)((samples[i] + 32768) >> 8);
            ledcWrite(_pin, duty);
            nextTime += _samplePeriodUs;
            while((int32_t)(nextTime - micros()) > 0)
            {
                // busy-wait for sample timing
            }
            // Feed watchdog every 1024 samples (~64ms at 16kHz)
            if((i & 0x3FF) == 0)
            {
                yield();
            }
        }

        ledcWrite(_pin, 128);  // return to silence
        return len;
    }
};

SimplePWMAudio out;
#else
I2SStream out;
#endif
#if USE_FLITE
cst_voice *voice = nullptr;
#endif
cst_wave *wave = nullptr;
enum LedState { LED_OFF, LED_SYNTH, LED_WAITING, LED_PLAYING, LED_LOADING };
volatile LedState ledState = LED_OFF;

#if USE_FLITE
// Speech parts are synthesized separately and concatenated with silence gaps.
// This allows inserting pauses between phrases since Flite's built-in
// punctuation pauses are very short.
const char* parts[] = {
  "This is an amateur radio competition",
  "from Victor , Echo , One , Fox Trot , Oscar.",
  "V , E , 1 , F , O",
  "Please report when found!",
};
const int numParts = sizeof(parts) / sizeof(parts[0]);
const float pauseSeconds = 0.5;  // silence between parts
#endif

// LED blink task runs on Core 0
void ledTask(void *pvParameters)
{
  while(true)
  {
    if(ledState == LED_SYNTH)
    {
      rgbLedWrite(RGB_BUILTIN, 16, 0, 0);
      vTaskDelay(pdMS_TO_TICKS(500));
      rgbLedWrite(RGB_BUILTIN, 0, 0, 0);
      vTaskDelay(pdMS_TO_TICKS(500));
    }
    else if(ledState == LED_LOADING)
    {
      rgbLedWrite(RGB_BUILTIN, 0, 0, 16);
      vTaskDelay(pdMS_TO_TICKS(500));
      rgbLedWrite(RGB_BUILTIN, 0, 0, 0);
      vTaskDelay(pdMS_TO_TICKS(500));
    }
    else if(ledState == LED_WAITING)
    {
      rgbLedWrite(RGB_BUILTIN, 0, 16, 0);
      vTaskDelay(pdMS_TO_TICKS(500));
      rgbLedWrite(RGB_BUILTIN, 0, 0, 0);
      vTaskDelay(pdMS_TO_TICKS(500));
    }
    else if(ledState == LED_PLAYING)
    {
      rgbLedWrite(RGB_BUILTIN, 0, 0, 0);
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    else
    {
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
}

// Generate a tone for the given duration
void playTone(int durationMs)
{
#ifdef OUTPUT_PWM
  // Square wave directly on PWM pin via GPIO toggling
  int halfPeriodUs = 1000000 / (2 * TONE_FREQ);
  ledcDetach(PWM_PIN);
  pinMode(PWM_PIN, OUTPUT);
  unsigned long endTime = millis() + durationMs;
  while(millis() < endTime)
  {
    digitalWrite(PWM_PIN, HIGH);
    delayMicroseconds(halfPeriodUs);
    digitalWrite(PWM_PIN, LOW);
    delayMicroseconds(halfPeriodUs);
  }
  // Re-attach LEDC for speech playback
  ledcAttach(PWM_PIN, 156250, 8);
  ledcWrite(PWM_PIN, 128);
#else
  // Sine wave through I2S in 256-sample chunks
  int totalSamples = (SAMPLE_RATE * durationMs) / 1000;
  short buf[256];
  int written = 0;
  while(written < totalSamples)
  {
    int chunk = min(256, totalSamples - written);
    for(int i = 0; i < chunk; i++)
    {
      float t = (float)(written + i) / (float)SAMPLE_RATE;
      buf[i] = (short)(TONE_AMP * sinf(2.0f * M_PI * TONE_FREQ * t));
    }
    out.write((uint8_t*)buf, chunk * sizeof(short));
    written += chunk;
  }
#endif
}

// Silence for the given duration
void playSilence(int durationMs)
{
#ifdef OUTPUT_PWM
  delay(durationMs);
#else
  int totalSamples = (SAMPLE_RATE * durationMs) / 1000;
  short buf[256];
  memset(buf, 0, sizeof(buf));
  int written = 0;
  while(written < totalSamples)
  {
    int chunk = min(256, totalSamples - written);
    out.write((uint8_t*)buf, chunk * sizeof(short));
    written += chunk;
  }
#endif
}

// Load speech from SPIFFS. Returns true if a valid speech.raw file was
// found. In Flite mode, also checks SPEECH_VERSION and rejects mismatches
// so synthesis can be triggered. Samples are allocated in PSRAM.
bool loadCachedWave()
{
  if(!SPIFFS.exists(CACHE_PATH))
  {
    Serial.println("Cache file not found");
    return false;
  }

  File f = SPIFFS.open(CACHE_PATH, "r");
  if(!f)
  {
    Serial.println("Failed to open cache file");
    return false;
  }

  Serial.printf("Cache file size: %d bytes\n", f.size());

  // Header: [version, sample_rate, num_samples, num_channels]
  int header[4];
  if(f.read((uint8_t*)header, sizeof(header)) != sizeof(header))
  {
    Serial.println("Failed to read cache header");
    f.close();
    return false;
  }

#if USE_FLITE
  int version = header[0];
  Serial.printf("Cache version: %d, expected: %d\n", version, SPEECH_VERSION);
  if(version != SPEECH_VERSION)
  {
    Serial.println("Cache version mismatch, re-synthesizing");
    f.close();
    return false;
  }
#endif

  int sample_rate = header[1];
  int num_samples = header[2];
  int num_channels = header[3];
  size_t data_size = num_samples * num_channels * sizeof(short);

  if(sample_rate <= 0 || num_samples <= 0 || num_channels <= 0)
  {
    f.close();
    return false;
  }

  // Verify file size matches header to detect corruption
  if(f.size() != sizeof(header) + data_size)
  {
    f.close();
    return false;
  }

  // Allocate sample buffer in PSRAM
  short *samples = (short*)ps_malloc(data_size);
  if(!samples)
  {
    f.close();
    return false;
  }

  if(f.read((uint8_t*)samples, data_size) != data_size)
  {
    free(samples);
    f.close();
    return false;
  }

  f.close();

  // Populate a cst_wave struct pointing to the PSRAM buffer
  wave = new_wave();
  wave->sample_rate = sample_rate;
  wave->num_samples = num_samples;
  wave->num_channels = num_channels;
  wave->samples = samples;

  return true;
}

#if USE_FLITE
// Save synthesized wave to SPIFFS cache with version header
bool saveCachedWave(cst_wave *w)
{
  File f = SPIFFS.open(CACHE_PATH, "w");
  if(!f)
  {
    return false;
  }

  int header[4] = { SPEECH_VERSION, w->sample_rate, w->num_samples, w->num_channels };
  f.write((uint8_t*)header, sizeof(header));
  f.write((uint8_t*)w->samples, w->num_samples * w->num_channels * sizeof(short));
  f.close();

  return true;
}
#endif

void setup()
{
  Serial.begin(115200);
  delay(2000);  // wait for USB CDC serial to be ready

#ifdef OUTPUT_PWM
  Serial.println("using PWM");
#else
  Serial.println("using I2C");
#endif

  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Info);

  rgbLedWrite(RGB_BUILTIN, 0, 0, 0);

  pinMode(PTT_PIN, OUTPUT);
  digitalWrite(PTT_PIN, 1); // default to off

#ifdef OUTPUT_PWM
  out.begin(PWM_PIN, SAMPLE_RATE);
#else
  auto cfg = out.defaultConfig();
  cfg.pin_bck = I2S_BCK;
  cfg.pin_ws = I2S_WS;
  cfg.pin_data = I2S_DOUT;
  cfg.sample_rate = SAMPLE_RATE;
  cfg.channels = 1;
  cfg.bits_per_sample = 16;
  out.begin(cfg);
#endif

  // Start LED blink task on Core 0
  xTaskCreatePinnedToCore(ledTask, "LED", 2048, NULL, 1, NULL, 0);

  // Mount SPIFFS (format on first use)
  if(!SPIFFS.begin(true))
  {
    Serial.println("SPIFFS Mount Failed, formatting...");
    SPIFFS.format();
    if(!SPIFFS.begin())
      Serial.println("Can't format SPIFFS!");
  }

  unsigned long t0 = millis();

  // Load speech from SPIFFS
  ledState = LED_LOADING;
#if USE_FLITE
  Serial.printf("SPEECH_VERSION = %d\n", SPEECH_VERSION);
  if(loadCachedWave())
  {
    ledState = LED_OFF;
    Serial.printf("Loaded cached wave: %d samples at %dHz in %lums\n",
                  cst_wave_num_samples(wave),
                  cst_wave_sample_rate(wave),
                  millis() - t0);
  }
  else
  {
    ledState = LED_OFF;

    flite_init();
    voice = register_cmu_us_slt();
    feat_set_float(voice->features, "duration_stretch", DURATION_STRETCH);

    // Synthesize each part separately and concatenate with silence gaps
    Serial.println("Synthesizing...");
    t0 = millis();
    ledState = LED_SYNTH;

    for(int i = 0; i < numParts; i++)
    {
      cst_wave *part = flite_text_to_wave(parts[i], voice);
      if(!part)
      {
        Serial.printf("Failed to synthesize part %d\n", i);
        continue;
      }

      if(!wave)
      {
        wave = part;
      }
      else
      {
        // Insert silence gap before this part
        int gapSamples = (int)(part->sample_rate * pauseSeconds);
        cst_wave *gap = new_wave();
        gap->sample_rate = part->sample_rate;
        gap->num_channels = 1;
        gap->num_samples = gapSamples;
        gap->samples = (short*)calloc(gapSamples, sizeof(short));

        concat_wave(wave, gap);
        concat_wave(wave, part);
        delete_wave(gap);
        delete_wave(part);
      }
    }

    ledState = LED_OFF;

    if(wave)
    {
      Serial.printf("Synthesized %d samples at %dHz in %lums\n",
                    cst_wave_num_samples(wave),
                    cst_wave_sample_rate(wave),
                    millis() - t0);

      if(saveCachedWave(wave))
      {
        Serial.println("Saved wave to SPIFFS cache");
      }
      else
      {
        Serial.println("Failed to save wave to SPIFFS cache");
      }
    }
    else
    {
      Serial.println("Synthesis failed!");
    }
  }
#else
  // Load pre-uploaded speech.raw from SPIFFS (no Flite, no version check)
  if(loadCachedWave())
  {
    ledState = LED_OFF;
    Serial.printf("Loaded wave: %d samples at %dHz in %lums\n",
                  cst_wave_num_samples(wave),
                  cst_wave_sample_rate(wave),
                  millis() - t0);
  }
  else
  {
    ledState = LED_OFF;
    Serial.println("No speech.raw found on SPIFFS! Upload with: pio run --target uploadfs");
  }
#endif

  // Apply volume gain to samples in memory
  if(wave && SPEECH_GAIN != 1.0f)
  {
    Serial.printf("Applying gain: %.2f\n", SPEECH_GAIN);
    int n = cst_wave_num_samples(wave);
    for(int i = 0; i < n; i++)
    {
      int32_t s = (int32_t)(wave->samples[i] * SPEECH_GAIN);
      if(s > 32767) s = 32767;
      else if(s < -32768) s = -32768;
      wave->samples[i] = (short)s;
    }
  }

  Serial.println("Ready");
}

void loop()
{
  ledState = LED_PLAYING;

  digitalWrite(PTT_PIN, 0); // PTT on

  delay(500);

  playTone(2000);
  playSilence(1000);

  // Play cached/synthesized speech
  if(wave)
  {
    out.write((uint8_t*)wave->samples, cst_wave_num_samples(wave) * sizeof(short));

    delay(500);
    digitalWrite(PTT_PIN, 1); // PTT off

    delay(4000);

    digitalWrite(PTT_PIN, 0); // PTT on
    delay(500);

    playTone(2000);
    playSilence(1000);

    playTone(2000);
    playSilence(1000);

    out.write((uint8_t*)wave->samples, cst_wave_num_samples(wave) * sizeof(short));
  }

  ledState = LED_WAITING;

  delay(500);

  digitalWrite(PTT_PIN, 1); // PTT off

  delay(60000);
}
