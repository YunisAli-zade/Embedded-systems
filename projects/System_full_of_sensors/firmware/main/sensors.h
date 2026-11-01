#pragma once

#include <stddef.h>
#include <stdint.h>

// Bir ölçmə = bir sensorun bir dəyəri
typedef struct {
    char    name[24];    // sensor adı, məs: "distance"
    char    unit[8];     // vahid, məs: "%", "C"
    char    label[20];   // oxunaqlı etiket, məs: "bright", "comfortable" (boş ola bilər)
    float   value;       // ədədi dəyər
    int64_t uptime_ms;   // ölçmə anı (cihaz işə düşəndən bəri, millisaniyə)
} reading_t;

// Aktiv sensorları hazırlayır (GPIO, daxili sensor və s.)
void sensors_init(void);

// Aktiv sensorların hamısını oxuyub out massivinə yazır.
// Yazılan ölçmələrin sayını qaytarır (oxunmayan sensor atlanır).
size_t sensors_read_all(reading_t *out, size_t max);
