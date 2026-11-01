#include "sensors.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/temperature_sensor.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_attr.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "sensors";

/* ================================================================== */
/*  Oxunaqlı etiketlər (ədədi dəyərdən mətn)                           */
/*  Hədləri öz mühitinə görə dəyişə bilərsən.                          */
/* ================================================================== */
static float round1(float v) { return roundf(v * 10.0f) / 10.0f; }

static const char *vib_label(uint32_t c)
{
    if (c == 0) return "none";
    if (c <= 2) return "weak";
    if (c <= 9) return "medium";
    return "strong";
}

static const char *light_label(float pct)
{
    if (pct < 8.0f)  return "dark";
    if (pct < 25.0f) return "dim";
    if (pct < 50.0f) return "a little bright";
    if (pct < 80.0f) return "bright";
    return "very bright";
}

static const char *temp_label(float c)
{
    if (c < 15.0f) return "cold";
    if (c < 20.0f) return "cool";
    if (c < 26.0f) return "comfortable";
    if (c < 30.0f) return "warm";
    return "hot";
}

static const char *hum_label(float h)
{
    if (h < 30.0f) return "dry";
    if (h < 60.0f) return "comfortable";
    if (h < 80.0f) return "humid";
    return "very humid";
}

static const char *tilt_label(float deg)
{
    if (deg < 10.0f)  return "flat";
    if (deg < 30.0f)  return "slightly tilted";
    if (deg < 60.0f)  return "tilted";
    if (deg < 120.0f) return "on its side";
    return "upside down";
}

static const char *rot_label(float dps)
{
    if (dps < 3.0f)   return "still";
    if (dps < 30.0f)  return "slow movement";
    if (dps < 120.0f) return "moving";
    return "fast rotation";
}

/* ================================================================== */
/*  1) Vibrasiya sensoru (rəqəmsal, GPIO kəsməsi ilə sayır)            */
/*     Vibrasiya qısa olur, 5 saniyədən bir baxsaq qaçırardıq. Ona görə  */
/*     hər impulsu kəsmə (interrupt) sayır, biz isə "son ölçmədən bəri   */
/*     neçə vibrasiya oldu" rəqəmini göndəririk.                        */
/* ================================================================== */
#if CONFIG_GW_VIB_ENABLE

static volatile uint32_t s_vib_count = 0;
static volatile int64_t  s_vib_last_us = 0;

// Kəsmə funksiyası: çox qısa olmalıdır, log yazmaq olmaz
static void IRAM_ATTR vib_isr(void *arg)
{
    int64_t now = esp_timer_get_time();
    if (now - s_vib_last_us > 20000) {            // 20 ms-dən sıx impulslar bir sayılır
        __atomic_fetch_add(&s_vib_count, 1, __ATOMIC_RELAXED);
        s_vib_last_us = now;
    }
}

static void vib_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_GW_VIB_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
#if CONFIG_GW_VIB_ACTIVE_HIGH
        .intr_type = GPIO_INTR_POSEDGE,
#else
        .intr_type = GPIO_INTR_NEGEDGE,
#endif
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    esp_err_t e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {   // INVALID_STATE = artıq qurulub
        ESP_ERROR_CHECK(e);
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(CONFIG_GW_VIB_GPIO, vib_isr, NULL));
}

// Son oxunuşdan bəri vibrasiya sayını qaytarır və sayğacı sıfırlayır
static uint32_t vib_take_count(void)
{
    return __atomic_exchange_n(&s_vib_count, 0, __ATOMIC_RELAXED);
}

#endif /* CONFIG_GW_VIB_ENABLE */

/* ================================================================== */
/*  2) İşıq sensoru (analoq ADC1 və ya rəqəmsal)                       */
/*     Nəticə 0..100 % və mətn etiketi kimi qaytarılır.                */
/* ================================================================== */
#if CONFIG_GW_LIGHT_ENABLE

static bool s_light_ok = false;

#if CONFIG_GW_LIGHT_ANALOG

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
#define LIGHT_ATTEN ADC_ATTEN_DB_12
#else
#define LIGHT_ATTEN ADC_ATTEN_DB_11
#endif

static adc_oneshot_unit_handle_t s_adc;
static adc_channel_t s_light_ch;

static void light_init(void)
{
    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(CONFIG_GW_LIGHT_GPIO, &unit, &s_light_ch) != ESP_OK
        || unit != ADC_UNIT_1) {
        ESP_LOGE(TAG, "GPIO %d is not an ADC1 pin (use GPIO 1..10), light sensor disabled",
                 CONFIG_GW_LIGHT_GPIO);
        return;
    }
    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc));

    adc_oneshot_chan_cfg_t ch_cfg = {
        .atten = LIGHT_ATTEN,                 // 0..~3.1V diapazonu
        .bitwidth = ADC_BITWIDTH_DEFAULT,     // 12 bit: 0..4095
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, s_light_ch, &ch_cfg));
    s_light_ok = true;
}

// 16 ölçmənin ortalaması (səs-küyü azaldır), nəticə 0..100 %
static bool light_read_percent(float *pct)
{
    int sum = 0, ok = 0;
    for (int i = 0; i < 16; i++) {
        int raw;
        if (adc_oneshot_read(s_adc, s_light_ch, &raw) == ESP_OK) {
            sum += raw;
            ok++;
        }
    }
    if (ok == 0) {
        return false;
    }
    float p = ((float)sum / (float)ok) / 4095.0f * 100.0f;
#if !CONFIG_GW_LIGHT_BRIGHT_IS_HIGH
    p = 100.0f - p;                            // tərs modul: yüksək gərginlik = qaranlıq
#endif
    *pct = p < 0.0f ? 0.0f : (p > 100.0f ? 100.0f : p);
    return true;
}

#else /* rəqəmsal çıxış */

static void light_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_GW_LIGHT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    s_light_ok = true;
}

static bool light_read_percent(float *pct)
{
    int level = gpio_get_level(CONFIG_GW_LIGHT_GPIO);
#if !CONFIG_GW_LIGHT_BRIGHT_IS_HIGH
    level = !level;
#endif
    *pct = level ? 100.0f : 0.0f;
    return true;
}

#endif /* CONFIG_GW_LIGHT_ANALOG */
#endif /* CONFIG_GW_LIGHT_ENABLE */

/* ================================================================== */
/*  3) DHT11 (bir məftilli protokol, mikrosaniyə dəqiqliyi lazımdır)   */
/* ================================================================== */
#if CONFIG_GW_DHT11_ENABLE

#define DHT_PIN CONFIG_GW_DHT11_GPIO

static portMUX_TYPE s_dht_mux = portMUX_INITIALIZER_UNLOCKED;

static void dht_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << DHT_PIN,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,   // open-drain: xətti ya aşağı çəkir, ya buraxır
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(DHT_PIN, 1);
}

// Pin "level" səviyyəsində qaldığı müddəti (µs) qaytarır, timeout olsa -1
static int dht_pulse_us(int level, int timeout_us)
{
    int64_t t0 = esp_timer_get_time();
    while (gpio_get_level(DHT_PIN) == level) {
        if (esp_timer_get_time() - t0 > timeout_us) {
            return -1;
        }
    }
    return (int)(esp_timer_get_time() - t0);
}

// Bir cəhd. Uğurludursa true.
static bool dht_try_once(float *temp_c, float *hum)
{
    uint8_t d[5] = { 0 };
    bool ok = true;

    // Başlanğıc siqnalı: ≥18 ms aşağı çək
    gpio_set_level(DHT_PIN, 0);
    esp_rom_delay_us(20000);

    // Zaman həssas hissə (~5 ms): kəsmələri bu nüvədə söndürürük
    portENTER_CRITICAL(&s_dht_mux);
    gpio_set_level(DHT_PIN, 1);                       // xətti burax (pull-up yuxarı qaldırır)
    if (dht_pulse_us(0, 100) < 0 ||                   // xəttin yuxarı qalxmasını gözlə
        dht_pulse_us(1, 100) < 0 ||                   // sensor xətti aşağı çəkənə qədər (~20-40 µs)
        dht_pulse_us(0, 120) < 0 ||                   // sensor ~80 µs aşağı
        dht_pulse_us(1, 120) < 0) {                   // sensor ~80 µs yuxarı
        ok = false;
    }
    for (int i = 0; ok && i < 40; i++) {
        if (dht_pulse_us(0, 100) < 0) {               // ~50 µs aşağı (bit başlanğıcı)
            ok = false;
            break;
        }
        int high = dht_pulse_us(1, 150);              // yuxarı: ~27 µs = 0, ~70 µs = 1
        if (high < 0) {
            ok = false;
            break;
        }
        d[i / 8] = (uint8_t)((d[i / 8] << 1) | (high > 40 ? 1 : 0));
    }
    portEXIT_CRITICAL(&s_dht_mux);

    if (!ok) {
        return false;
    }
    if (((d[0] + d[1] + d[2] + d[3]) & 0xFF) != d[4]) {
        return false;                                 // yoxlama cəmi səhvdir
    }

    *hum = (float)d[0] + (float)d[1] * 0.1f;
    float t = (float)d[2] + (float)(d[3] & 0x7F) * 0.1f;
    *temp_c = (d[3] & 0x80) ? -t : t;
    return true;
}

// DHT11 ara-sıra səhv edir, ona görə 2 dəfə cəhd edirik (cəhdlər arası ≥1 s)
static bool dht_read(float *temp_c, float *hum)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        if (dht_try_once(temp_c, hum)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1100));
    }
    return false;
}

#endif /* CONFIG_GW_DHT11_ENABLE */

/* ================================================================== */
/*  4) MPU6050 (I2C: akselerometr + giroskop)                          */
/*     Xam oxlar əvəzinə: əyilmə bucağı və fırlanma sürəti.            */
/* ================================================================== */
#if CONFIG_GW_MPU_ENABLE

typedef struct {
    float ax, ay, az;      // g
    float gx, gy, gz;      // °/s (sıfır sürüşməsi çıxılıb)
    float tilt;            // dərəcə, Z oxu ilə şaquli arasındakı bucaq (NAN = ölçülə bilmir)
    float rot;             // °/s, fırlanma sürətinin ümumi qiyməti
} mpu_data_t;

static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_mpu;
static bool s_mpu_ok = false;
static float s_gyro_off[3] = { 0.0f, 0.0f, 0.0f };

static esp_err_t mpu_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_mpu, buf, sizeof(buf), 100);
}

static esp_err_t mpu_read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_mpu, &reg, 1, data, len, 100);
}

// Cihaz yerində dayananda giroskopun sıfır sürüşməsini ölçür (~1 saniyə)
static void mpu_calibrate_gyro(void)
{
    float sum[3] = { 0.0f, 0.0f, 0.0f };
    int n = 0;
    for (int i = 0; i < 100; i++) {
        uint8_t d[6];
        if (mpu_read_regs(0x43, d, sizeof(d)) == ESP_OK) {      // GYRO_XOUT_H
            sum[0] += (float)(int16_t)((d[0] << 8) | d[1]) / 131.0f;
            sum[1] += (float)(int16_t)((d[2] << 8) | d[3]) / 131.0f;
            sum[2] += (float)(int16_t)((d[4] << 8) | d[5]) / 131.0f;
            n++;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (n >= 20) {
        for (int k = 0; k < 3; k++) {
            s_gyro_off[k] = sum[k] / (float)n;
        }
        ESP_LOGI(TAG, "MPU gyro offset (dps): %.2f %.2f %.2f", s_gyro_off[0], s_gyro_off[1], s_gyro_off[2]);
    }
}

static void mpu_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = CONFIG_GW_I2C_SDA_GPIO,
        .scl_io_num = CONFIG_GW_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &s_i2c_bus) != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed, MPU6050 disabled");
        return;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CONFIG_GW_MPU_ADDR,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_mpu) != ESP_OK) {
        ESP_LOGE(TAG, "I2C add device failed, MPU6050 disabled");
        return;
    }

    uint8_t who = 0;
    if (mpu_read_regs(0x75, &who, 1) != ESP_OK) {      // WHO_AM_I
        ESP_LOGE(TAG, "MPU6050 not responding (check SDA/SCL wires, power, address)");
        return;
    }
    ESP_LOGI(TAG, "MPU WHO_AM_I = 0x%02X (0x68 expected for MPU6050)", who);

    if (mpu_write_reg(0x6B, 0x00) != ESP_OK) {         // PWR_MGMT_1: yuxudan oyat
        ESP_LOGE(TAG, "MPU6050 wake-up failed");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    s_mpu_ok = true;
    mpu_calibrate_gyro();
}

static bool mpu_read(mpu_data_t *m)
{
    uint8_t d[14];
    if (mpu_read_regs(0x3B, d, sizeof(d)) != ESP_OK) {
        return false;
    }
    int16_t ax = (int16_t)((d[0] << 8) | d[1]);
    int16_t ay = (int16_t)((d[2] << 8) | d[3]);
    int16_t az = (int16_t)((d[4] << 8) | d[5]);
    // d[6], d[7] = temperatur (istifadə olunmur)
    int16_t gx = (int16_t)((d[8] << 8) | d[9]);
    int16_t gy = (int16_t)((d[10] << 8) | d[11]);
    int16_t gz = (int16_t)((d[12] << 8) | d[13]);

    m->ax = ax / 16384.0f;               // ±2 g diapazonu
    m->ay = ay / 16384.0f;
    m->az = az / 16384.0f;
    m->gx = gx / 131.0f - s_gyro_off[0]; // ±250 °/s diapazonu
    m->gy = gy / 131.0f - s_gyro_off[1];
    m->gz = gz / 131.0f - s_gyro_off[2];

    float mag = sqrtf(m->ax * m->ax + m->ay * m->ay + m->az * m->az);
    if (mag > 0.3f) {
        float c = m->az / mag;
        c = c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c);
        m->tilt = acosf(c) * 57.29578f;  // radian -> dərəcə
    } else {
        m->tilt = NAN;                   // sərbəst düşmə: cazibə ölçülə bilmir
    }
    m->rot = sqrtf(m->gx * m->gx + m->gy * m->gy + m->gz * m->gz);
    return true;
}

#endif /* CONFIG_GW_MPU_ENABLE */

/* ================================================================== */
/*  5) İxtiyari: çipin daxili temperaturu                              */
/* ================================================================== */
#if CONFIG_GW_TEMP_ENABLE

static temperature_sensor_handle_t s_tsens;

static void temp_init(void)
{
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    ESP_ERROR_CHECK(temperature_sensor_install(&cfg, &s_tsens));
    ESP_ERROR_CHECK(temperature_sensor_enable(s_tsens));
}

#endif /* CONFIG_GW_TEMP_ENABLE */

/* ================================================================== */
/*  Ümumi hissə                                                        */
/* ================================================================== */
static void add_reading(reading_t *out, size_t *n, size_t max,
                        const char *name, const char *unit, float value, const char *label)
{
    if (*n >= max) {
        return;
    }
    reading_t *r = &out[*n];
    snprintf(r->name, sizeof(r->name), "%s", name);
    snprintf(r->unit, sizeof(r->unit), "%s", unit);
    snprintf(r->label, sizeof(r->label), "%s", label ? label : "");
    r->value = value;
    r->uptime_ms = esp_timer_get_time() / 1000;
    (*n)++;
}

void sensors_init(void)
{
#if CONFIG_GW_VIB_ENABLE
    vib_init();
    ESP_LOGI(TAG, "Vibration sensor ready (GPIO %d)", CONFIG_GW_VIB_GPIO);
#endif
#if CONFIG_GW_LIGHT_ENABLE
    light_init();
    ESP_LOGI(TAG, "Light sensor %s (GPIO %d)", s_light_ok ? "ready" : "NOT ready", CONFIG_GW_LIGHT_GPIO);
#endif
#if CONFIG_GW_DHT11_ENABLE
    dht_init();
    ESP_LOGI(TAG, "DHT11 ready (GPIO %d)", DHT_PIN);
#endif
#if CONFIG_GW_MPU_ENABLE
    mpu_init();
    ESP_LOGI(TAG, "MPU6050 %s", s_mpu_ok ? "ready" : "NOT ready");
#endif
#if CONFIG_GW_TEMP_ENABLE
    temp_init();
#endif
    // YENİ SENSOR: öz init funksiyanı burada çağır (1/3)
}

size_t sensors_read_all(reading_t *out, size_t max)
{
    size_t n = 0;

#if CONFIG_GW_VIB_ENABLE
    uint32_t vc = vib_take_count();
    add_reading(out, &n, max, "vibration_count", "count", (float)vc, vib_label(vc));
#endif

#if CONFIG_GW_LIGHT_ENABLE
    float pct;
    if (s_light_ok && light_read_percent(&pct)) {
        pct = round1(pct);
        add_reading(out, &n, max, "light_level", "%", pct, light_label(pct));
    }
#endif

#if CONFIG_GW_DHT11_ENABLE
    float dht_t, dht_h;
    if (dht_read(&dht_t, &dht_h)) {
        add_reading(out, &n, max, "air_temperature", "C", round1(dht_t), temp_label(dht_t));
        add_reading(out, &n, max, "air_humidity", "%", round1(dht_h), hum_label(dht_h));
    } else {
        ESP_LOGW(TAG, "DHT11: no valid data (check wiring / GPIO %d)", DHT_PIN);
    }
#endif

#if CONFIG_GW_MPU_ENABLE
    mpu_data_t m;
    if (s_mpu_ok && mpu_read(&m)) {
        if (!isnan(m.tilt)) {
            add_reading(out, &n, max, "tilt_angle", "deg", round1(m.tilt), tilt_label(m.tilt));
        }
        add_reading(out, &n, max, "rotation_rate", "dps", round1(m.rot), rot_label(m.rot));
#if CONFIG_GW_MPU_SEND_RAW
        add_reading(out, &n, max, "accel_x", "g", m.ax, "");
        add_reading(out, &n, max, "accel_y", "g", m.ay, "");
        add_reading(out, &n, max, "accel_z", "g", m.az, "");
        add_reading(out, &n, max, "gyro_x", "dps", m.gx, "");
        add_reading(out, &n, max, "gyro_y", "dps", m.gy, "");
        add_reading(out, &n, max, "gyro_z", "dps", m.gz, "");
#endif
    } else if (s_mpu_ok) {
        ESP_LOGW(TAG, "MPU6050: read failed");
    }
#endif

#if CONFIG_GW_TEMP_ENABLE
    float c;
    if (temperature_sensor_get_celsius(s_tsens, &c) == ESP_OK) {
        add_reading(out, &n, max, "chip_temp", "C", c, "");
    }
#endif

    // YENİ SENSOR: oxu və add_reading(...) ilə əlavə et (2/3)
    // YENİ SENSOR: Kconfig.projbuild-ə enable düyməsi əlavə et (3/3)

    return n;
}
