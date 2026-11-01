# System_full_of_sensors — ESP32-S3 → WiFi → API → SQL

```
[sensorlar] → ESP32-S3 (ESP-IDF) ──WiFi──► modem ──► noutbuk:  Flask API (port 5000) ──► SQL baza
                 JSON POST + X-API-Key                           server/app.py            server/db.py
```

ESP32 SQL serverə **birbaşa yazmır** — arada kiçik API (Flask) durur. Bu, "API və s. üçün nə əlavə etmək lazımdır" sualının cavabıdır: ESP32 tərəfində əlavə kitabxana lazım deyil (hamısı ESP-IDF-in öz komponentləridir), noutbuk tərəfində isə `server/` qovluğu.

## 0. Qovluq

```
firmware/            ESP-IDF layihəsi
  main/main.c        əsas dövr: ölç → bufferə at → göndər → gözlə
  main/wifi.c        WiFi (qopsa özü yenidən qoşulur)
  main/sensors.c     sensorlar (vibrasiya, işıq, DHT11, MPU6050; ixtiyari: çip temperaturu), oxunaqlı etiketlər və yeni sensor şablonu
  main/uploader.c    JSON yığır, HTTP POST edir; server əlçatmazdırsa RAM-da saxlayıb təkrar göndərir
  main/Kconfig.projbuild   WiFi / server / pin ayarları (menuconfig)
server/
  app.py             Flask API  (POST /api/readings, GET /api/readings, /health, /)
  db.py              SQL qatı (SQLAlchemy): SQLite / MySQL / PostgreSQL / MS SQL Server
  send_test_data.py  ESP32-ni təqlid edir (firmware-siz test)
```

## 1. Nə əlavə etmək / hazırlamaq lazımdır (yoxlama siyahısı)

- [ ] **Noutbukda Python 3.9+** və paketlər: `pip install -r requirements.txt` (bu addım bir dəfə internet istəyir. Modemdə internet yoxdursa: internetli yerdə `pip download -r requirements.txt -d wheels`, sonra noutbukda `pip install --no-index --find-links wheels -r requirements.txt`).
- [ ] **SQL serverin drayveri** (yalnız SQLite-dan başqa baza istəsən): MySQL → `pymysql`, PostgreSQL → `psycopg2-binary`, MS SQL Server → `pyodbc` + "ODBC Driver 17/18 for SQL Server".
- [ ] **Boş baza yarat** (cədvəli proqram özü yaradır; SQLite-da bu da lazım deyil): məs. `CREATE DATABASE sensors;`
- [ ] **Noutbukun IP-sini sabitlə** (modemdə DHCP reservation və ya statik IP). IP dəyişsə ESP32 serveri tapa bilməz.
- [ ] **Firewall-da 5000 portunu aç** (Windows, admin CMD):
  `netsh advfirewall firewall add rule name="SensorAPI" dir=in action=allow protocol=TCP localport=5000`
  (Linux: `sudo ufw allow 5000/tcp`)
- [ ] **ESP-IDF v6.0.x** (v6.0.2 üçün hazırlanıb; v5.3+ də işləməlidir). Əlavə managed component lazım deyil (cJSON də istifadə olunmur, çünki v6-da IDF-dən çıxarılıb), yəni internetsiz də build olur. Versiyanı `idf.py --version` ilə yoxla.
- [ ] **API açarını dəyiş** (`change-me` əvəzinə öz sözün) — həm serverdə, həm menuconfig-də eyni olsun.

## 2. Server

```bash
cd server
pip install -r requirements.txt

# Windows (CMD):
set API_KEY=mening-gizli-acarim
python app.py

# Linux / macOS:
API_KEY=mening-gizli-acarim python app.py
```

Başqa SQL server istəyirsənsə `DATABASE_URL` ver (bax: `server/db.py`-nin başındakı nümunələr), məs.:

```
set DATABASE_URL=mssql+pyodbc://sa:PAROL@localhost/sensors?driver=ODBC+Driver+17+for+SQL+Server
set DATABASE_URL=mysql+pymysql://root:PAROL@localhost/sensors
set DATABASE_URL=postgresql+psycopg2://postgres:PAROL@localhost/sensors
```

Yoxla (firmware-siz): başqa terminaldə `python send_test_data.py http://127.0.0.1:5000 mening-gizli-acarim` → `201 {"inserted":3,"ok":true}` görməlisən. Brauzerdə `http://127.0.0.1:5000/` açanda 3 sətir görünməlidir. Telefonu eyni WiFi-a qoş və `http://<noutbukun-IP>:5000/health` aç — açılırsa ESP32 də qoşula bilər.

Cədvəl `readings`: `id, device_id, sensor, value, unit, label, measured_at (UTC), received_at (UTC)`.

**Cədvəl artıq yaradılıbsa**, yeni `label` sütununu SSMS-də bir dəfə əlavə et (`USE sensors;` ilə `sensors` bazasında):

```sql
ALTER TABLE dbo.readings ADD label VARCHAR(32) NULL;
```

Köhnə adlarla (`light`, `dht_temp`...) yazılmış sətirlər istəsən qala bilər, istəsən `DELETE FROM dbo.readings;` ilə təmizlə.

## 3. Firmware

```powershell
cd firmware                       # idf.py YALNIZ bu qovluqda işlədilir (CMakeLists.txt burada durur)
$env:IDF_TARGET = "esp32s3"       # mühitdə köhnə IDF_TARGET=esp32 qalıbsa, bu vacibdir
idf.py set-target esp32s3
idf.py menuconfig                 # "Sensor Gateway" menyusu: WiFi SSID/parol, Server URL, API key, pinlər
idf.py build flash monitor
```

Server URL formatı: `http://<noutbukun-IP>:5000/api/readings` (məs. `http://192.168.1.70:5000/api/readings`)

Monitorda görməlisən: `Got IP: ...`, `light_level = 62.5 %  [bright]`, `air_temperature = 24.0 C  [comfortable]`, `Sent 6 readings, 0 left in buffer`.

### Qoşulma (default pinlər)

| Sensor | Sensorun pini | ESP32-S3 | Göndərilən ad (vahid) → etiket |
|---|---|---|---|
| Vibrasiya (3 pinli, rəqəmsal) | VCC / GND / OUT(DO) | 3V3 / GND / **GPIO 39** | `vibration_count` (count) → `none`, `weak`, `medium`, `strong` |
| İşıq (3 pinli) | VCC / GND / OUT(AO) | 3V3 / GND / **GPIO 4** | `light_level` (0–100 %) → `dark`, `dim`, `a little bright`, `bright`, `very bright` |
| DHT11 (3 pinli) | VCC / GND / DATA | 3V3 / GND / **GPIO 5** | `air_temperature` (C) → `cold`…`hot`; `air_humidity` (%) → `dry`…`very humid` |
| MPU6050 (GY-521) | VCC / GND / SDA / SCL | 3V3 / GND / **GPIO 40** / **GPIO 41** | `tilt_angle` (deg) → `flat`, `slightly tilted`, `tilted`, `on its side`, `upside down`; `rotation_rate` (dps) → `still`, `slow movement`, `moving`, `fast rotation` |

Hər dövrdə **6 sətir** yazılır. Hər sətirdə həm ədədi `value`, həm də oxunaqlı `label` olur. Hədlər (məs. işıq 8 / 25 / 50 / 80 %) `firmware/main/sensors.c`-nin əvvəlindəki etiket funksiyalarındadır, öz mühitinə görə dəyişə bilərsən. MPU6050 işə düşəndə giroskopun sıfır sürüşməsi ~1 saniyə ərzində ölçülür: **cihazı o vaxt tərpətmə**. Xam oxları (`accel_x/y/z`, `gyro_x/y/z`) da istəsən, menuconfig-də "Also send raw..." seçimini aç.

**Pin toqquşması:** İşıq sensoru və DHT11 eyni anda GPIO 4-də ola bilməz (biri analoq, biri rəqəmsal protokol işlədir, ikisi də pini tutur). İşıq sensoru GPIO 4-də qalır (bu ADC1 pinidir), DHT11-in DATA telini **GPIO 5-ə** keçir. Başqa pinə qoşsan, menuconfig-də həmin pin nömrəsini dəyiş.

Qaydalar: analoq işıq sensoru yalnız **ADC1 pinlərində** (GPIO 1..10) oxunur, çünki ADC2 pinləri WiFi ilə birlikdə etibarsızdır. Vibrasiya sensoru çıxışı vibrasiyada LOW verirsə, menuconfig-də "Output goes HIGH..." seçimini söndür. MPU6050-də AD0 pini 3.3V-a qoşulubsa, ünvan 0x69 olur. Bütün sensorları 3.3V ilə qidalandır (DHT11 və modullar 3.3V ilə işləyir).

İxtiyari (default söndürülüb): çipin daxili temperaturu (`chip_temp`).

## 4. Necə işləyir (qısa)

- ESP32 hər `Sample period` (default 5 s) bir sensorları oxuyur, ölçmələri RAM bufferinə yazır və WiFi varsa serverə göndərir.
- Server əlçatmaz olsa ölçmələr bufferdə (120 ədədə qədər) qalır və növbəti dövrdə təkrar göndərilir.
- ESP32-nin real saatı yoxdur (internet/NTP yoxdur), ona görə hər ölçmə **uptime** ilə göndərilir; server real vaxtı özü hesablayır (`measured_at`). Bufer yeni göndərilsə belə vaxtlar düzgün qalır.

## 5. Problemlər

| Əlamət | Səbəb / həll |
|---|---|
| `Target 'esp32s3' ... not consistent with target 'esp32' in the environment` | Terminalda `IDF_TARGET=esp32` qalıb. `$env:IDF_TARGET = "esp32s3"` yaz, `build` və `sdkconfig` qovluq/faylını sil, `idf.py set-target esp32s3` təkrarla |
| `CMakeLists.txt not found in project directory` | `idf.py`-ni `firmware` qovluğunda işlət, layihənin kökündə yox |
| `Got IP` yoxdur | SSID/parol səhvdir, ya da şəbəkə 5 GHz-dir. ESP32 yalnız **2.4 GHz** dəstəkləyir |
| `HTTP request failed` | IP səhvdir, server işləmir, firewall 5000-i bağlayıb, ya da ESP32 və noutbuk fərqli şəbəkədədir |
| `Server answered HTTP 401` | API key serverdəki ilə eyni deyil |
| `HTTP 400` | Göndərilən JSON formatı səhvdir (serverin cavab mətninə bax) |
| `HTTP 500` | Baza xətasıdır: serverin terminalındakı xəta mətninə bax (baza yoxdur / parol səhvdir / drayver yoxdur) |
| `DHT11: no valid data` | DATA tel yanlış pindədir (GPIO 5), eyni pinə başqa sensor da qoşulub, qidalanma 3.3V deyil |
| `MPU6050 not responding` | SDA/SCL tellərini (40/41) yoxla, ya da ünvan 0x69-dur |
| `light_level` həmişə `dark`/`very bright` | Modul rəqəmsal çıxışlıdır (D0): menuconfig-də "analog output" seçimini söndür |
| İşıqda `dark`, qaranlıqda `bright` yazır | Modulun çıxışı tərsdir: menuconfig-də "Higher reading = more light" seçimini söndür |
| `vibration_count` həmişə 0 | Çıxış səviyyəsi tərsdir ("Output goes HIGH..." seçimini dəyiş) və ya modulun potensiometri çox həssasdır/həssas deyil |

## 6. Yeni sensor əlavə etmək

`firmware/main/sensors.c` faylında `YENİ SENSOR` şərhləri olan 3 yer var:
1. `sensors_init()` — sensorun init funksiyasını çağır.
2. `sensors_read_all()` — oxu və `add_reading(out, &n, max, "ad", "vahid", dəyər);` yaz.
3. `Kconfig.projbuild` — istəsən enable düyməsi və pin ayarları əlavə et.

Server dəyişməz qalır: istənilən yeni `sensor` adı avtomatik `readings` cədvəlinə düşür. Etiket istəmirsənsə, `add_reading(..., "")` yaz (`label` boş qalır).

## Test statusu

Server (`app.py`: giriş yoxlaması, JSON yoxlaması, vaxt hesabı, endpointlər) test edilib və SQL Server ilə işlədiyin təsdiqlənib. DHT11 oxuma məntiqi kompüterdə təqlid olunmuş siqnalla yoxlanıb (real cihazda zamanlama hələ yoxlanmayıb). Firmware-in C kodu sintaksis baxımından yoxlanıb, lakin real ESP-IDF və cihazla kompilyasiya/yükləmə edilməyib, Build və ya işə salma zamanı xəta çıxsa, xəta mətnini göndər, düzəldək.
