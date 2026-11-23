#include "hw.h"

#if !defined(BOARD_ES3C28P)

void hwBegin() {
  auto cfg = M5.config();
  M5.begin(cfg);
}

void hwUpdate() { M5.update(); }

const char* hwInfo() { return "CoreS3"; }

bool hwTouchTap(int& x, int& y) {
  auto t = M5.Touch.getDetail();
  if (!t.wasPressed()) return false;
  x = t.x;
  y = t.y;
  return true;
}

bool hwTouchHeld(int& x, int& y) {
  auto t = M5.Touch.getDetail();
  if (!t.isPressed()) return false;
  x = t.x;
  y = t.y;
  return true;
}

#else  // ---------------------------------------------------------------- ES3C28P

#include <lgfx/v1/panel/Panel_LCD.hpp>
#include <lgfx/v1/touch/Touch_FT5x06.hpp>

// Pinos (LCDWiki "2.8inch ESP32-S3 Display", ES3C28P):
//  LCD  CS 10, DC 46, SCK 12, MOSI 11, MISO 13, RST = EN da placa, luz 45 (alto = acesa)
//  Toque FT6336G  SDA 16, SCL 15, RST 18, INT 17 (I2C compartilhado com o ES8311)
//  Áudio ES8311  MCLK 4, BCLK 5, LRCK 7, DOUT(ESP->codec) 8, DIN(codec->ESP) 6
//  Amplificador FM8002E: IO1, nível BAIXO liga
namespace {
constexpr int PIN_I2C_SDA = 16, PIN_I2C_SCL = 15, I2C_PORT = 1;
constexpr int PIN_AMP_EN = 1;
constexpr uint8_t ES8311_ADDR = 0x18;
// Ganho digital do DAC do ES8311: 0xBF = 0 dB, passos de 0,5 dB. O volume do M5.Speaker
// (SPEAKER_VOLUME) atua antes disto.
#ifndef ES8311_DAC_VOL
#define ES8311_DAC_VOL 0xE7  // +20 dB
#endif

// ILI9341V IPS: sequência do fabricante (ILI9341V_Init.txt). Inversão (0x21), MADCTL (0x36)
// e formato de cor (0x3A) ficam com o Panel_LCD (cfg.invert / rgb_order / setRotation).
struct Panel_ILI9341V : public lgfx::Panel_LCD {
  Panel_ILI9341V() {
    _cfg.memory_width = _cfg.panel_width = 240;
    _cfg.memory_height = _cfg.panel_height = 320;
  }

 protected:
  const uint8_t* getInitCommands(uint8_t listno) const override {
    static constexpr uint8_t list0[] = {
        0xCF, 3, 0x00, 0xC1, 0x30,
        0xED, 4, 0x64, 0x03, 0x12, 0x81,
        0xE8, 3, 0x85, 0x00, 0x78,
        0xCB, 5, 0x39, 0x2C, 0x00, 0x34, 0x02,
        0xF7, 1, 0x20,
        0xEA, 2, 0x00, 0x00,
        0xC0, 1, 0x13,
        0xC1, 1, 0x13,
        0xC5, 2, 0x22, 0x35,
        0xC7, 1, 0xBD,
        0xB6, 2, 0x0A, 0xA2,
        0xF6, 2, 0x01, 0x30,
        0xB1, 2, 0x00, 0x1B,
        0xF2, 1, 0x00,
        0x26, 1, 0x01,
        0xE0, 15, 0x0F, 0x35, 0x31, 0x0B, 0x0E, 0x06, 0x49, 0xA7, 0x33, 0x07, 0x0F, 0x03, 0x0C, 0x0A, 0x00,
        0xE1, 15, 0x00, 0x0A, 0x0F, 0x04, 0x11, 0x08, 0x36, 0x58, 0x4D, 0x07, 0x10, 0x0C, 0x32, 0x34, 0x0F,
        0x11, 0 + CMD_INIT_DELAY, 120,
        0x29, 0 + CMD_INIT_DELAY, 20,
        0xFF, 0xFF,
    };
    return listno == 0 ? list0 : nullptr;
  }
};

class LGFX_ES3C28P : public lgfx::LGFX_Device {
  Panel_ILI9341V _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;
  lgfx::Touch_FT5x06 _touch;

 public:
  LGFX_ES3C28P() {
    {
      auto c = _bus.config();
      c.spi_host = SPI2_HOST;
      c.spi_mode = 0;
      c.freq_write = 40000000;
      c.freq_read = 16000000;
      c.spi_3wire = false;
      c.use_lock = true;
      c.dma_channel = SPI_DMA_CH_AUTO;
      c.pin_sclk = 12;
      c.pin_mosi = 11;
      c.pin_miso = 13;
      c.pin_dc = 46;
      _bus.config(c);
      _panel.setBus(&_bus);
    }
    {
      auto c = _panel.config();
      c.pin_cs = 10;
      c.pin_rst = -1;  // reset da tela é o EN da placa
      c.pin_busy = -1;
      c.offset_rotation = 0;
      c.readable = true;
      c.invert = true;      // IPS: o fabricante liga a inversão (0x21)
      c.rgb_order = false;  // BGR
      c.dlen_16bit = false;
      c.bus_shared = false;
      _panel.config(c);
    }
    {
      auto c = _light.config();
      c.pin_bl = 45;
      c.invert = false;
      c.freq = 12000;
      c.pwm_channel = 7;
      _light.config(c);
      _panel.setLight(&_light);
    }
    {
      auto c = _touch.config();
      c.x_min = 0;
      c.x_max = 239;
      c.y_min = 0;
      c.y_max = 319;
      c.pin_int = -1;  // leitura por I2C a cada quadro: o INT (17) do FT6336G pulsa e o LGFX perdia toques
      c.pin_rst = 18;
      c.bus_shared = false;
      c.offset_rotation = 0;
      c.i2c_port = I2C_PORT;
      c.i2c_addr = 0x38;
      c.pin_sda = PIN_I2C_SDA;
      c.pin_scl = PIN_I2C_SCL;
      c.freq = 400000;
      _touch.config(c);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};

LGFX_ES3C28P& lcd() {
  static LGFX_ES3C28P d;
  return d;
}

// O I2C é dividido entre o toque (tarefa própria) e o ES8311 (tarefa de voz).
SemaphoreHandle_t s_i2c = nullptr;

void esWrite(const uint8_t* pairs, size_t n) {
  if (s_i2c) xSemaphoreTake(s_i2c, portMAX_DELAY);
  for (size_t i = 0; i + 1 < n; i += 2) {
    if (!lgfx::i2c::writeRegister8(I2C_PORT, ES8311_ADDR, pairs[i], pairs[i + 1], 0, 100000).has_value()) {
      Serial.printf("[audio] ES8311 sem resposta (reg 0x%02X)\n", pairs[i]);
      break;
    }
  }
  if (s_i2c) xSemaphoreGive(s_i2c);
}

// Toque lido a cada 8 ms numa tarefa: o loop principal leva 60-100 ms por
// quadro (e mais quando chega áudio), e um toque rápido cabia entre duas leituras.
portMUX_TYPE s_touchMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool s_tapPending = false, s_held = false;
volatile int16_t s_tapX = 0, s_tapY = 0, s_curX = 0, s_curY = 0;

void touchTask(void*) {
  bool was = false;
  uint8_t ups = 0;
  for (;;) {
    lgfx::touch_point_t tp;
    xSemaphoreTake(s_i2c, portMAX_DELAY);
    uint_fast8_t n = lcd().getTouch(&tp, 1);
    xSemaphoreGive(s_i2c);
    bool down = n > 0;
    // Soltar só conta depois de 3 leituras sem dedo (o FT6336G às vezes falha uma).
    if (down) ups = 0; else if (was && ++ups < 3) down = true;
    portENTER_CRITICAL(&s_touchMux);
    if (n > 0) {
      s_curX = tp.x;
      s_curY = tp.y;
    }
    if (down && !was) {
      s_tapX = s_curX;
      s_tapY = s_curY;
      s_tapPending = true;
    }
    s_held = down;
    portEXIT_CRITICAL(&s_touchMux);
    was = down;
    vTaskDelay(pdMS_TO_TICKS(8));
  }
}

// Mesmo arranjo que o M5Unified usa no AtomS3R/Echo S3R (ES8311 escravo, MCLK tirado do
// BCLK x8): comprovado com as classes Speaker/Mic. Fala e microfone se alternam (voice.cpp).
bool spkEnable(void*, bool on) {
  static const uint8_t up[] = {
      0x00, 0x80,  // CSM ligado, escravo
      0x01, 0xB5,  // MCLK = BCLK; clocks do DAC
      0x02, 0x18,  // multiplicador x8
      0x0D, 0x01,  // analógico ligado
      0x12, 0x00,  // DAC ligado
      0x13, 0x10,  // saída para o amplificador
      0x32, ES8311_DAC_VOL,
      0x37, 0x08,  // equalizador do DAC desligado
  };
  if (on) {
    esWrite(up, sizeof(up));
    digitalWrite(PIN_AMP_EN, LOW);
  } else {
    digitalWrite(PIN_AMP_EN, HIGH);
  }
  return true;
}

bool micEnable(void*, bool on) {
  static const uint8_t up[] = {
      0x00, 0x80,
      0x01, 0xBA,  // MCLK = BCLK; clocks do ADC
      0x02, 0x18,
      0x0D, 0x01,
      0x0E, 0x02,  // PGA e modulador do ADC ligados
      0x14, 0x1A,  // microfone 1 + ganho analógico alto (+30 dB)
      0x17, 0xBF,  // volume do ADC 0 dB
      0x1C, 0x6A,  // sem equalizador, remove DC
  };
  static const uint8_t down[] = {0x0D, 0xFC, 0x0E, 0x6A, 0x00, 0x00};
  esWrite(on ? up : down, on ? sizeof(up) : sizeof(down));
  return true;
}
// setCallback é protegido (o M5Unified só o chama dentro do M5.begin, que esta placa não usa).
struct SpeakerAccess : m5::Speaker_Class { using m5::Speaker_Class::setCallback; };
struct MicAccess : m5::Mic_Class { using m5::Mic_Class::setCallback; };
}  // namespace

static char s_info[64] = "ES3C28P";
const char* hwInfo() { return s_info; }

lgfx::LGFX_Device& hwDisplay() { return lcd(); }

void hwBegin() {
  pinMode(PIN_AMP_EN, OUTPUT);
  digitalWrite(PIN_AMP_EN, HIGH);  // amplificador desligado até tocar algo
  s_i2c = xSemaphoreCreateMutex();
  lcd().init();
  lgfx::i2c::init(I2C_PORT, PIN_I2C_SDA, PIN_I2C_SCL);

  auto s = M5.Speaker.config();
  s.pin_bck = 5;
  s.pin_ws = 7;
  s.pin_data_out = 8;
  s.pin_mck = I2S_PIN_NO_CHANGE;
  s.magnification = 1;
  s.i2s_port = I2S_NUM_1;
  M5.Speaker.config(s);
  static_cast<SpeakerAccess&>(M5.Speaker).setCallback(nullptr, spkEnable);

  auto m = M5.Mic.config();
  m.pin_bck = 5;
  m.pin_ws = 7;
  m.pin_data_in = 6;
  m.pin_mck = I2S_PIN_NO_CHANGE;
  m.i2s_port = I2S_NUM_1;
  m.magnification = 1;
  m.over_sampling = 1;
  m.stereo = false;
  M5.Mic.config(m);
  static_cast<MicAccess&>(M5.Mic).setCallback(nullptr, micEnable);

  uint8_t id = 0;
  bool ok = lgfx::i2c::readRegister(I2C_PORT, ES8311_ADDR, 0xFD, &id, 1, 100000).has_value();
  snprintf(s_info, sizeof(s_info), "ES3C28P, ES8311 %s (0x%02X), tela %dx%d", ok ? "ok" : "AUSENTE", id,
           lcd().width(), lcd().height());
  Serial.printf("[placa] %s\n", s_info);
  xTaskCreatePinnedToCore(touchTask, "toque", 3072, nullptr, 3, nullptr, 0);
}

void hwUpdate() {}

bool hwTouchTap(int& x, int& y) {
  bool got = false;
  portENTER_CRITICAL(&s_touchMux);
  if (s_tapPending) {
    s_tapPending = false;
    x = s_tapX;
    y = s_tapY;
    got = true;
  }
  portEXIT_CRITICAL(&s_touchMux);
  return got;
}

bool hwTouchHeld(int& x, int& y) {
  portENTER_CRITICAL(&s_touchMux);
  bool h = s_held;
  x = s_curX;
  y = s_curY;
  portEXIT_CRITICAL(&s_touchMux);
  return h;
}

#endif
