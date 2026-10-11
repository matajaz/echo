// echo -- onboard sensor bring-up. See sensors.h for the hardware map.

#include "sensors.h"

#include "display.h"
#include "esp_log.h"
#include "esp_rom_sys.h"   // esp_rom_delay_us: tick-independent us delays
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "pcf85063a.h"
#include "qmi8658.h"
#include "shtc3.h"
#include "custom_io_expander_ch32v003.h"

static const char *TAG = "sensors";

static qmi8658_dev_t s_imu;
static pcf85063a_dev_t s_rtc;
static i2c_master_dev_handle_t s_shtc3 = NULL;

static sensors_data_t s_data;
static SemaphoreHandle_t s_mtx = NULL;
static bool s_started = false;
static volatile bool s_paused = false;

void sensors_set_paused(bool paused) {
    s_paused = paused;
}

// --- SHTC3 CRC-validated read (#5b, 2026-10-10) ---------------------------
// The vendor shtc3_get_th() reads all 6 bytes [T,T,crc, H,H,crc] but IGNORES
// the two CRC bytes, so a corrupted I2C transfer is silently converted to a
// bogus temperature/humidity. This does the measurement our-side and verifies
// the Sensirion CRC-8 (polynomial 0x31, init 0xFF, no reflection) over each
// 16-bit word before trusting it. Returns ESP_OK + T/H only if BOTH CRCs pass.
static uint8_t shtc3_crc8(const uint8_t *d, int n) {
    uint8_t crc = 0xFF;
    for (int i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static esp_err_t shtc3_read_th_crc(i2c_master_dev_handle_t dev, float *t, float *h) {
    if (dev == NULL) return ESP_ERR_INVALID_STATE;
    uint8_t wake[2] = {0x35, 0x17};     // SHTC3_REG_WAKE
    uint8_t meas[2] = {0x78, 0x66};     // SHTC3_REG_T_CSD_NM (T first, clock-stretch DISABLED, normal)
    uint8_t sleep[2] = {0xB0, 0x98};    // SHTC3_REG_SLEEP
    uint8_t rx[6] = {0};
    esp_err_t wret = i2c_master_transmit(dev, wake, 2, 100);
    if (wret != ESP_OK) {
        ESP_LOGW(TAG, "SHTC3 wake failed: %s", esp_err_to_name(wret));
        return wret;
    }
    // Tick-independent delays: FREERTOS_HZ=100 makes pdMS_TO_TICKS(1)==0
    // and pdMS_TO_TICKS(15)==10ms, which is < the 12.1ms conversion -> the
    // sensor NAKs the early read (ESP_ERR_INVALID_RESPONSE). Use us delays.
    esp_rom_delay_us(300);              // >240us wake-up (SHTC3_WARMUP_US)
    // Clock-stretch-DISABLED measurement: the write STARTS the conversion and
    // the master must then WAIT it out and read the result separately. A single
    // transmit_receive (the CSE pattern) returns all-zero data here because the
    // sensor is not ready and this SoC's I2C controller does not hold SCL for
    // the sensor's clock stretch.
    esp_err_t ret = i2c_master_transmit(dev, meas, 2, 200);
    if (ret == ESP_OK) {
        esp_rom_delay_us(13000);        // >12.1ms max conversion (T first, normal)
        ret = i2c_master_receive(dev, rx, 6, 200);
    }
    (void)i2c_master_transmit(dev, sleep, 2, 100);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SHTC3 read failed: %s", esp_err_to_name(ret));
        return ret;
    }
    // An all-zero payload means the sensor returned NO measurement. Its CRC-8
    // is also 0x00, so the CRC check below would PASS VACUOUSLY and report a
    // bogus 0.0C / 0%RH as "valid". Reject it explicitly before the CRC test.
    if (rx[0] == 0 && rx[1] == 0 && rx[3] == 0 && rx[4] == 0) {
        ESP_LOGW(TAG, "SHTC3 all-zero read (raw %02x %02x %02x %02x %02x %02x)",
                 rx[0], rx[1], rx[2], rx[3], rx[4], rx[5]);
        return ESP_ERR_INVALID_RESPONSE;
    }
    // Validate both CRC-8s (byte 2 covers rx[0..1], byte 5 covers rx[3..4]).
    if (shtc3_crc8(&rx[0], 2) != rx[2] || shtc3_crc8(&rx[3], 2) != rx[5]) {
        ESP_LOGW(TAG, "SHTC3 CRC mismatch (raw %02x %02x %02x %02x %02x %02x)",
                 rx[0], rx[1], rx[2], rx[3], rx[4], rx[5]);
        return ESP_ERR_INVALID_CRC;
    }
    *t = ((((rx[0] * 256.0f) + rx[1]) * 175.0f) / 65535.0f) - 45.0f;
    *h = (((rx[3] * 256.0f) + rx[4]) * 100.0f) / 65535.0f;
    return ESP_OK;
}

// Try the QMI8658 at the high address first (0x6B, what the Waveshare
// board uses), then the low address (0x6A) as a fallback. Returns true if
// the IMU answered its WHO_AM_I at either address.
static bool imu_bring_up(i2c_master_bus_handle_t bus) {
    const uint8_t addrs[] = {QMI8658_ADDRESS_HIGH, QMI8658_ADDRESS_LOW};
    for (size_t i = 0; i < sizeof(addrs); i++) {
        // Start each attempt clean: a previous failed qmi8658_init may have
        // registered an I2C device handle on the bus.
        s_imu.dev_handle = NULL;
        if (qmi8658_init(&s_imu, bus, addrs[i]) != ESP_OK) {
            // qmi8658_init adds the device to the bus BEFORE probing
            // WHO_AM_I, so a failure after that point leaves the handle
            // registered. Remove it, otherwise the retry below would
            // overwrite (leak) the handle and leave a stale device on the bus.
            if (s_imu.dev_handle != NULL) {
                i2c_master_bus_rm_device(s_imu.dev_handle);
                s_imu.dev_handle = NULL;
            }
            continue;
        }
        uint8_t who = 0;
        if (qmi8658_get_who_am_i(&s_imu, &who) == ESP_OK) {
            ESP_LOGI(TAG, "QMI8658 found at 0x%02x (WHO_AM_I=0x%02x)", addrs[i], who);
            qmi8658_enable_sensors(&s_imu, QMI8658_ENABLE_ACCEL | QMI8658_ENABLE_GYRO);
            // Report accel in m/s^2 (driver default is mg); gyro default
            // is already deg/s, which is what sensors_data_t expects.
            qmi8658_set_accel_unit_mps2(&s_imu, true);
            return true;
        }
        // Init succeeded but the follow-up probe failed: tear the device
        // down before trying the next address so it isn't leaked.
        i2c_master_bus_rm_device(s_imu.dev_handle);
        s_imu.dev_handle = NULL;
    }
    return false;
}

static void poll_task(void *arg) {
    (void)arg;
    while (1) {
        // While paused (audio playing), do no I2C work -- keeps the single
        // core free for the audio task so I2S DMA never starves.
        if (s_paused) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        sensors_data_t d;
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        d = s_data; // start from last (keeps *_ok flags for failed sensors)
        xSemaphoreGive(s_mtx);

        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000); // ms since boot

        if (d.imu_ok) {
            // Read into scratch and copy into d ONLY on success: on an I2C
            // NACK the driver may leave the out params untouched or garbage,
            // and d.imu_ok would still report 1. Leaving d at the last good
            // value is the honest answer.
            float ax, ay, az, gx, gy, gz, tmp;
            bool accel_ok = false, gyro_ok = false;
            if (qmi8658_read_accel(&s_imu, &ax, &ay, &az) == ESP_OK) {
                d.accel_x = ax;
                d.accel_y = ay;
                d.accel_z = az;
                accel_ok = true;
            }
            if (qmi8658_read_gyro(&s_imu, &gx, &gy, &gz) == ESP_OK) {
                d.gyro_x = gx;
                d.gyro_y = gy;
                d.gyro_z = gz;
                gyro_ok = true;
            }
            if (qmi8658_read_temp(&s_imu, &tmp) == ESP_OK) {
                d.imu_temp_c = tmp;   // secondary; does NOT gate motion freshness
            }
            // V05b: imu_last_ms gates the MOTION values (accel/gyro) the UI
            // renders together. Refresh it only when BOTH motion reads
            // succeeded this cycle — a die-temperature-only success must not
            // make stale accel/gyro appear fresh (previously any-success did).
            if (accel_ok && gyro_ok) d.imu_last_ms = now;
        }
        if (d.th_ok) {
            float t = 0, h = 0;
            esp_err_t thr = shtc3_read_th_crc(s_shtc3, &t, &h);
            if (thr == ESP_OK) {
                d.temp_c = t;
                d.humidity_rh = h;
                d.th_last_ms = now;        // R10: fresh + CRC-valid
            } else if (thr == ESP_ERR_INVALID_CRC) {
                ESP_LOGW(TAG, "SHTC3 CRC mismatch — reading discarded");
                // leave th_last_ms unchanged -> reads STALE via R10
            }
        }
        if (d.rtc_ok) {
            pcf85063a_datetime_t dt;
            if (pcf85063a_get_time_date(&s_rtc, &dt) == ESP_OK) {
                // #5 (2026-10-10): check the oscillator-stop / clock-integrity
                // bit. The PCF85063A seconds register (0x04) bit 7 (OS) is set
                // when the oscillator has stopped (power loss / brown-out), i.e.
                // the time is NOT trustworthy. The vendor driver MASKS it off
                // (bufss[0] & 0x7F) and returns the bogus time as if valid. Read
                // the raw register ourselves and only accept the time if OS==0.
                uint8_t secreg = 0;
                uint8_t reg = 0x04;  // PCF85063A_RTC_SECOND_ADDR
                bool os_ok = false;
                if (i2c_master_transmit_receive(s_rtc.dev_handle, &reg, 1,
                                                &secreg, 1, 200) == ESP_OK) {
                    os_ok = ((secreg & 0x80) == 0);   // OS=0 -> clock integrity good
                }
                if (os_ok) {
                    d.year = dt.year;
                    d.month = dt.month;
                    d.day = dt.day;
                    d.hour = dt.hour;
                    d.min = dt.min;
                    d.sec = dt.sec;
                    d.rtc_last_ms = now;       // R10: fresh + trustworthy
                } else {
                    ESP_LOGW(TAG, "RTC oscillator-stop set (OS=1) — time untrusted");
                    // leave rtc_last_ms unchanged -> reads STALE via R10
                }
            }
        }

        // Battery: raw ADC (0..1023) from the CH32 expander, mapped to an
        // estimated Li-ion state-of-charge. The exact hardware divider
        // ratio is NOT documented in the vendor source, so the voltage
        // scaling below is a best-effort assumption (CH32 10-bit ADC, ~3.3V
        // ref, 1:2 divider -> full-scale ~ a 4.2V pack near adc=1023, with
        // a flat Li-ion curve clamp). TODO: calibrate adc_full/adc_empty
        // against a multimeter reading of the real pack. The RAW adc is
        // always honest even if the % needs trimming.
        {
            esp_io_expander_handle_t exp = display_get_io_expander();
            uint16_t adc = 0;
            if (exp != NULL && custom_io_expander_get_adc(exp, &adc) == ESP_OK) {
                d.bat_adc = adc;
                // Calibrated from an observed reading: adc~411 while the
                // board ran the full stack on battery with no cable (so
                // clearly well-charged / near-full). A 1-cell Li-ion is
                // 3.0V empty..4.2V full; mapping 4.2V->~410 implies ~98
                // ADC counts/volt, so empty(3.0V)~294. These bounds give a
                // plausible SOC; refine against a multimeter if exactness
                // is needed (the vendor doesn't publish the divider ratio).
                const int adc_empty = 300; // ~3.0V
                const int adc_full = 410;  // ~4.2V
                int pct = (int)(((long)(adc - adc_empty) * 100) / (adc_full - adc_empty));
                if (pct < 0) pct = 0;
                if (pct > 100) pct = 100;
                d.bat_soc_pct = pct;
                d.bat_ok = true;
                d.bat_last_ms = now;       // R10
            }
        }

        d.now_ms = now;  // R10: publish-time clock for age computation

        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_data = d;
        xSemaphoreGive(s_mtx);

        // Log a compact line so the readings are verifiable via /log.
        ESP_LOGI(TAG,
                 "imu[%d] a=%.2f,%.2f,%.2f g=%.1f,%.1f,%.1f t=%.1fC | th[%d] %.1fC %.0f%%RH | rtc[%d] %04u-%02u-%02u %02u:%02u:%02u | bat[%d] adc=%u soc=%d%%",
                 d.imu_ok, d.accel_x, d.accel_y, d.accel_z, d.gyro_x, d.gyro_y,
                 d.gyro_z, d.imu_temp_c, d.th_ok, d.temp_c, d.humidity_rh,
                 d.rtc_ok, d.year, d.month, d.day, d.hour, d.min, d.sec,
                 d.bat_ok, d.bat_adc, d.bat_soc_pct);

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

esp_err_t sensors_init(void) {
    if (s_started) {
        return ESP_OK;
    }
    i2c_master_bus_handle_t bus = display_get_i2c_bus();
    if (bus == NULL) {
        ESP_LOGE(TAG, "I2C bus not ready -- call display_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    s_mtx = xSemaphoreCreateMutex();
    if (s_mtx == NULL) {
        return ESP_ERR_NO_MEM;
    }

    sensors_data_t d = {0};

    // IMU (QMI8658)
    d.imu_ok = imu_bring_up(bus);
    if (!d.imu_ok) {
        ESP_LOGW(TAG, "QMI8658 IMU not found");
    }

    // Temp/humidity (SHTC3)
    s_shtc3 = shtc3_device_create(bus, SHTC3_I2C_ADDR, 400000);
    if (s_shtc3 != NULL) {
        // shtc3_get_id() writes TWO bytes (id[0] and id[1]) — the ID is 16-bit.
        // A single uint8_t here overflowed the adjacent stack byte (R2, ASan
        // stack-buffer-overflow, 2026-10-10). Give it the 2 bytes it writes.
        uint8_t id[2] = {0};
        if (shtc3_get_id(s_shtc3, id) == ESP_OK) {
            ESP_LOGI(TAG, "SHTC3 found (ID=0x%02x%02x)", id[0], id[1]);
            d.th_ok = true;
        } else {
            ESP_LOGW(TAG, "SHTC3 ID read failed");
            shtc3_device_delete(s_shtc3);
            s_shtc3 = NULL;
        }
    } else {
        ESP_LOGW(TAG, "SHTC3 device create failed");
    }

    // RTC (PCF85063A)
    if (pcf85063a_init(&s_rtc, bus, PCF85063A_ADDRESS) == ESP_OK) {
        pcf85063a_datetime_t dt;
        if (pcf85063a_get_time_date(&s_rtc, &dt) == ESP_OK) {
            ESP_LOGI(TAG, "PCF85063A RTC found (%04u-%02u-%02u %02u:%02u:%02u)",
                     dt.year, dt.month, dt.day, dt.hour, dt.min, dt.sec);
            d.rtc_ok = true;
        } else {
            ESP_LOGW(TAG, "PCF85063A time read failed");
        }
    } else {
        ESP_LOGW(TAG, "PCF85063A RTC init failed");
    }

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_data = d;
    xSemaphoreGive(s_mtx);
    // #4 (2026-10-10): create the poll task BEFORE marking started. The initial
    // one-shot readings above are already published, but if the poll task can't
    // start the values will never refresh — so s_started must reflect worker
    // liveness, not merely that init ran. (With the R10 freshness fields, a
    // never-refreshing group also correctly reads STALE in the UI.)
    if (xTaskCreate(poll_task, "sensor_poll", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to start sensor poll task — sensors will not refresh");
        return ESP_ERR_NO_MEM;   // caller logs degraded; s_started stays false
    }
    s_started = true;
    ESP_LOGI(TAG, "sensors init done (imu=%d th=%d rtc=%d)", d.imu_ok, d.th_ok, d.rtc_ok);
    return ESP_OK;
}

bool sensors_get(sensors_data_t *out) {
    if (!s_started || out == NULL) {
        return false;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *out = s_data;
    xSemaphoreGive(s_mtx);
    return true;
}

// GET /metrics -- Prometheus text exposition of all sensor + battery
// values. Each metric carries a HELP/TYPE line. Values from the mutex-
// guarded snapshot; *_ok flags are exposed too so a scraper can tell a
// real 0 from a missing sensor.
// Render a float as a fixed-point decimal string without relying on the
// C library's "%f" support. ESP-IDF's default newlib-nano printf is built
// without float formatting unless explicitly enabled, so "%f"/"%.2f" would
// silently emit empty or "nan" values in the /metrics response.
static void fmt_fixed(char *out, size_t outsz, double v, int decimals) {
    static const int scale[] = {1, 10, 100, 1000};
    int sc = (decimals >= 0 && decimals <= 3) ? scale[decimals] : 1;
    bool neg = v < 0;
    double av = neg ? -v : v;
    long long scaled = (long long)(av * (double)sc + 0.5);
    long long ip = scaled / sc;
    long long fp = scaled % sc;
    if (decimals == 0) {
        snprintf(out, outsz, "%s%lld", neg ? "-" : "", ip);
    } else {
        snprintf(out, outsz, "%s%lld.%0*lld", neg ? "-" : "", ip, decimals, fp);
    }
}

static esp_err_t handle_metrics(httpd_req_t *req) {
    sensors_data_t d;
    bool ok = sensors_get(&d);

    // 2048 B: the full exposition is ~1 KB and was silently truncating the
    // last (RTC) line at the old 1024 B cap (R9, 2026-10-10). Ample headroom
    // now; the truncation flag below turns any future overflow into a loud
    // 500 instead of a partial 200.
    char body[2048];
    size_t n = 0;
    bool truncated = false;

    // Bounded append. snprintf() returns the number of bytes it *would*
    // have written, so a naive `n += snprintf(...)` can push n past
    // sizeof(body) and make the next `sizeof(body) - n` underflow. Clamp
    // to the last usable index (keeping room for the NUL) and flag it.
    #define APPEND(...)                                                      \
        do {                                                                  \
            int _w = snprintf(body + n, sizeof(body) - n, __VA_ARGS__);       \
            if (_w < 0) {                                                    \
                truncated = true;                                            \
                break;                                                       \
            }                                                                 \
            if ((size_t)_w >= sizeof(body) - n) {                             \
                n = sizeof(body) - 1;                                         \
                truncated = true;                                            \
                break;                                                       \
            }                                                                 \
            n += (size_t)_w;                                                  \
        } while (0)

    // Float gauge line without depending on printf "%f" support.
    #define APPF(name, val, dec)                                             \
        do {                                                                  \
            char _fbuf[32];                                                   \
            fmt_fixed(_fbuf, sizeof(_fbuf), (val), (dec));                    \
            APPEND(name " %s\n", _fbuf);                                      \
        } while (0)

    APPEND("# HELP echo_sensors_ready 1 when the sensor subsystem is initialized\n"
           "# TYPE echo_sensors_ready gauge\n");
    if (!ok) {
        APPEND("echo_sensors_ready 0\n");
        httpd_resp_set_type(req, "text/plain; version=0.0.4");
        httpd_resp_send(req, body, n);
        return ESP_OK;
    }
    APPEND("echo_sensors_ready 1\n");

    // IMU
    APPEND("# HELP echo_imu_ok 1 when the QMI8658 IMU was detected\n"
           "# TYPE echo_imu_ok gauge\n"
           "echo_imu_ok %d\n", d.imu_ok);
    APPEND("# TYPE echo_accel_mps2 gauge\n");
    APPF("echo_accel_mps2{axis=\"x\"}", d.accel_x, 3);
    APPF("echo_accel_mps2{axis=\"y\"}", d.accel_y, 3);
    APPF("echo_accel_mps2{axis=\"z\"}", d.accel_z, 3);
    APPEND("# TYPE echo_gyro_dps gauge\n");
    APPF("echo_gyro_dps{axis=\"x\"}", d.gyro_x, 3);
    APPF("echo_gyro_dps{axis=\"y\"}", d.gyro_y, 3);
    APPF("echo_gyro_dps{axis=\"z\"}", d.gyro_z, 3);
    APPEND("# TYPE echo_imu_temp_celsius gauge\n");
    APPF("echo_imu_temp_celsius", d.imu_temp_c, 2);

    // Temp / humidity
    APPEND("# HELP echo_th_ok 1 when the SHTC3 temp/humidity sensor was detected\n"
           "# TYPE echo_th_ok gauge\n"
           "echo_th_ok %d\n", d.th_ok);
    APPEND("# TYPE echo_temperature_celsius gauge\n");
    APPF("echo_temperature_celsius", d.temp_c, 2);
    APPEND("# TYPE echo_humidity_percent gauge\n");
    APPF("echo_humidity_percent", d.humidity_rh, 1);

    // Battery
    APPEND("# HELP echo_battery_ok 1 when the battery ADC reading is valid\n"
           "# TYPE echo_battery_ok gauge\n"
           "echo_battery_ok %d\n", d.bat_ok);
    APPEND("# TYPE echo_battery_adc_raw gauge\n"
           "echo_battery_adc_raw %u\n", d.bat_adc);
    APPEND("# TYPE echo_battery_soc_percent gauge\n"
           "echo_battery_soc_percent %d\n", d.bat_soc_pct);

    // RTC (epoch-ish fields as a convenience)
    APPEND("# HELP echo_rtc_ok 1 when the PCF85063A RTC was detected\n"
           "# TYPE echo_rtc_ok gauge\n"
           "echo_rtc_ok %d\n", d.rtc_ok);

    httpd_resp_set_type(req, "text/plain; version=0.0.4");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (truncated) {
        // Never emit a partial exposition under HTTP 200 — a Prometheus
        // scrape would ingest a half-line. Surface it so it gets noticed and
        // the buffer is enlarged, rather than silently dropping metrics (R9).
        ESP_LOGE(TAG, "/metrics truncated at %u bytes -- enlarge body[]", (unsigned)n);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "metrics truncated\n");
        #undef APPEND
        #undef APPF
        return ESP_OK;
    }
    httpd_resp_send(req, body, n);
    #undef APPEND
    #undef APPF
    return ESP_OK;
}

esp_err_t sensors_register_metrics(httpd_handle_t server) {
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    httpd_uri_t m = {.uri = "/metrics", .method = HTTP_GET, .handler = handle_metrics};
    return httpd_register_uri_handler(server, &m);
}
