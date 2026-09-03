#include "hw/audio.h"
#include "hw/pins.h"
#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <driver/i2c.h>
#include <math.h>

// Use legacy i2s.h API (driver/deprecated). The new i2s_std.h driver
// rejects DMA setup when PSRAM is enabled because internal allocations
// can land in PSRAM ("user context not in internal RAM" GDMA error).
// The legacy driver allocates DMA structs directly via heap_caps with
// MALLOC_CAP_DMA which forces internal RAM.
#include <driver/i2s.h>

extern "C" {
#include "es8311.h"
}

static constexpr int AUDIO_SR  = 16000;
static constexpr int AUDIO_VOL = 60;
static constexpr int AUDIO_AMP = 6000;

static QueueHandle_t s_beepQ = nullptr;
struct BeepReq { uint16_t freq; uint16_t dur; };

static bool i2sInit() {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = AUDIO_SR;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 4;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;
  cfg.fixed_mclk = AUDIO_SR * 256;
  cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr) != ESP_OK) return false;

  i2s_pin_config_t pins = {};
  pins.mck_io_num   = PIN_I2S_MCLK;
  pins.bck_io_num   = PIN_I2S_BCLK;
  pins.ws_io_num    = PIN_I2S_WS;
  pins.data_out_num = PIN_I2S_DO;
  pins.data_in_num  = I2S_PIN_NO_CHANGE;
  if (i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) return false;
  return true;
}

static es8311_handle_t s_codec = nullptr;

static bool es8311CodecInit() {
  s_codec = es8311_create((i2c_port_t)0, ES8311_ADDRRES_0);
  if (!s_codec) { Serial.println("hwAudio: es8311_create failed"); return false; }
  const es8311_clock_config_t clk = {
    .mclk_inverted      = false,
    .sclk_inverted      = false,
    .mclk_from_mclk_pin = true,
    .mclk_frequency     = AUDIO_SR * 256,
    .sample_frequency   = AUDIO_SR,
  };
  if (es8311_init(s_codec, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) return false;
  if (es8311_sample_frequency_config(s_codec, clk.mclk_frequency, clk.sample_frequency) != ESP_OK) return false;
  es8311_microphone_config(s_codec, false);
  es8311_voice_volume_set(s_codec, AUDIO_VOL, nullptr);
  return true;
}

// 0..100, same scale as AUDIO_VOL — the codec's own hardware volume stage,
// not a digital amplitude scale, so low settings don't lose bit depth.
void hwAudioSetVolume(uint8_t pct0to100) {
  if (!s_codec) return;
  if (pct0to100 > 100) pct0to100 = 100;
  es8311_voice_volume_set(s_codec, pct0to100, nullptr);
}

static void beepTask(void*) {
  BeepReq r;
  int16_t buf[256];
  while (xQueueReceive(s_beepQ, &r, portMAX_DELAY) == pdTRUE) {
    int total = AUDIO_SR * r.dur / 1000;
    // 4ms linear fade in/out — softens the on/off click of a raw sine burst.
    // Capped at total/2 so a very short beep fades to silence, not negative.
    int fade = AUDIO_SR * 4 / 1000;
    if (fade > total / 2) fade = total / 2;
    float phase = 0.0f;
    float dphase = 2.0f * (float)M_PI * r.freq / (float)AUDIO_SR;
    for (int n = 0; n < total; n += 256) {
      int chunk = total - n;
      if (chunk > 256) chunk = 256;
      for (int i = 0; i < chunk; i++) {
        int sampleIdx = n + i;
        float env = 1.0f;
        if (fade > 0) {
          if (sampleIdx < fade)              env = (float)sampleIdx / fade;
          else if (sampleIdx >= total - fade) env = (float)(total - 1 - sampleIdx) / fade;
        }
        buf[i] = (int16_t)(AUDIO_AMP * env * sinf(phase));
        phase += dphase;
        if (phase > 2*M_PI) phase -= 2*M_PI;
      }
      size_t written;
      i2s_write(I2S_NUM_0, buf, chunk * sizeof(int16_t), &written, portMAX_DELAY);
    }
  }
}

bool hwAudioInit() {
#if BOARD_HAS_PA_CTRL
  pinMode(PIN_PA_CTRL, OUTPUT);
  digitalWrite(PIN_PA_CTRL, HIGH);
#endif

  if (!i2sInit())          { Serial.println("hwAudio: I2S init failed");   return false; }
  if (!es8311CodecInit())  { Serial.println("hwAudio: ES8311 init failed"); return false; }

  s_beepQ = xQueueCreate(8, sizeof(BeepReq));
  if (!s_beepQ) return false;
  xTaskCreatePinnedToCore(beepTask, "beep", 4096, nullptr, 5, nullptr, tskNO_AFFINITY);
  return true;
}

void hwBeep(uint16_t freqHz, uint16_t durMs) {
  if (!s_beepQ) return;
  BeepReq r{ freqHz, durMs };
  xQueueSend(s_beepQ, &r, 0);
}
