#pragma once

#include <stdbool.h>

// WiFi-ı station rejimində başladır (qoşulma arxa planda gedir, qopsa özü yenidən qoşulur)
void wifi_start(void);

// IP alınana qədər gözləyir. Qoşulubsa true qaytarır.
bool wifi_wait_connected(int timeout_ms);

// Hazırda WiFi-a qoşuluymu?
bool wifi_is_connected(void);
