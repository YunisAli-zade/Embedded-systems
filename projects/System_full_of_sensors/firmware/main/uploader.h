#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "sensors.h"

// Ölçmələri RAM bufferinə əlavə edir (dolarsa ən köhnələr silinir)
void uploader_push(const reading_t *r, size_t count);

// Bufferdəki hər şeyi serverə göndərməyə çalışır.
// Hamısı göndərilibsə true qaytarır; uğursuz olsa qalanlar bufferdə qalır.
bool uploader_flush(void);
