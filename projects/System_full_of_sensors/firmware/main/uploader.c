#include "uploader.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "uploader";

#define BUF_CAP   400   // RAM-da saxlanılan maksimum ölçmə sayı (server yoxdursa)
#define BATCH_MAX 40    // bir HTTP sorğusunda göndərilən maksimum ölçmə sayı

// Dairəvi bufer: s_head = ən köhnə element, s_count = neçə element var
static reading_t s_buf[BUF_CAP];
static size_t s_head = 0;
static size_t s_count = 0;

void uploader_push(const reading_t *r, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (!isfinite(r[i].value)) {
            continue;                         // NaN/inf JSON-u pozar, atırıq
        }
        if (s_count == BUF_CAP) {             // dolub: ən köhnəni at
            s_head = (s_head + 1) % BUF_CAP;
            s_count--;
            ESP_LOGW(TAG, "Buffer full, dropping oldest reading");
        }
        s_buf[(s_head + s_count) % BUF_CAP] = r[i];
        s_count++;
    }
}

/* ---- JSON-u əl ilə yığırıq (cJSON lazım deyil: ESP-IDF v6-da IDF-dən çıxarılıb) ---- */

// buf-a formatlı mətn əlavə edir. Yer çatmasa false qaytarır.
static bool json_append(char *buf, size_t cap, size_t *len, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + *len, cap - *len, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= cap - *len) {
        return false;
    }
    *len += (size_t)w;
    return true;
}

// JSON string üçün " və \ simvollarını qaçırır, idarəedici simvolları atır
static void json_escape(const char *in, char *out, size_t out_size)
{
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0' && o + 2 < out_size; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c >= 0x20) {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

// float -> "12.345" (ən çox 3 onluq, sondakı sıfırlar atılır: 3.000 -> 3, 62.500 -> 62.5).
// float printf-dən asılı olmasın deyə tam ədədlə yazırıq.
static void format_value(char *out, size_t size, float v)
{
    long long m = (long long)(v * 1000.0f + (v >= 0.0f ? 0.5f : -0.5f));
    bool neg = m < 0;
    if (neg) {
        m = -m;
    }
    snprintf(out, size, "%s%lld.%03lld", neg ? "-" : "", m / 1000, m % 1000);
    size_t len = strlen(out);
    while (len > 0 && out[len - 1] == '0') {
        out[--len] = '\0';
    }
    if (len > 0 && out[len - 1] == '.') {
        out[--len] = '\0';
    }
}

// Bufferin əvvəlindən n ölçməni JSON mətninə çevirir.
// Nəticəni free() ilə azad etmək lazımdır, yer çatmasa NULL.
static char *build_json(size_t n)
{
    size_t cap = 256 + n * 260;
    char *buf = malloc(cap);
    if (!buf) {
        return NULL;
    }
    size_t len = 0;

    char dev[2 * 64 + 1];
    json_escape(CONFIG_GW_DEVICE_ID, dev, sizeof(dev));

    // Cihazın indiki uptime-ı: server bununla hər ölçmənin real vaxtını hesablayır
    bool ok = json_append(buf, cap, &len,
                          "{\"device_id\":\"%s\",\"uptime_ms\":%lld,\"readings\":[",
                          dev, (long long)(esp_timer_get_time() / 1000));

    for (size_t i = 0; ok && i < n; i++) {
        const reading_t *r = &s_buf[(s_head + i) % BUF_CAP];
        char name[2 * 24 + 1], unit[2 * 8 + 1], label[2 * 20 + 1], val[32];
        json_escape(r->name, name, sizeof(name));
        json_escape(r->unit, unit, sizeof(unit));
        json_escape(r->label, label, sizeof(label));
        format_value(val, sizeof(val), r->value);
        ok = json_append(buf, cap, &len, "%s{\"sensor\":\"%s\",\"value\":%s,\"unit\":\"%s\"",
                         i ? "," : "", name, val, unit);
        if (ok && label[0] != '\0') {
            ok = json_append(buf, cap, &len, ",\"label\":\"%s\"", label);
        }
        if (ok) {
            ok = json_append(buf, cap, &len, ",\"uptime_ms\":%lld}", (long long)r->uptime_ms);
        }
    }
    if (ok) {
        ok = json_append(buf, cap, &len, "]}");
    }
    if (!ok) {
        free(buf);
        return NULL;
    }
    return buf;
}

// JSON-u serverə POST edir. 2xx cavab gəlsə true.
static bool http_post(const char *body)
{
    esp_http_client_config_t cfg = {
        .url = CONFIG_GW_SERVER_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return false;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "X-API-Key", CONFIG_GW_API_KEY);
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = -1;
    if (err == ESP_OK) {
        status = esp_http_client_get_status_code(client);
    } else {
        ESP_LOGW(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);

    if (status >= 200 && status < 300) {
        return true;
    }
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "Server answered HTTP %d (401 = wrong API key, 400 = bad data)", status);
    }
    return false;
}

bool uploader_flush(void)
{
    while (s_count > 0) {
        size_t n = (s_count < BATCH_MAX) ? s_count : BATCH_MAX;

        char *body = build_json(n);
        if (!body) {
            ESP_LOGE(TAG, "Out of memory while building JSON");
            return false;
        }
        bool ok = http_post(body);
        free(body);

        if (!ok) {
            ESP_LOGW(TAG, "Send failed, %u readings kept for retry", (unsigned)s_count);
            return false;
        }

        s_head = (s_head + n) % BUF_CAP;
        s_count -= n;
        ESP_LOGI(TAG, "Sent %u readings, %u left in buffer", (unsigned)n, (unsigned)s_count);
    }
    return true;
}
