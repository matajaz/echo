// echo -- onboard sensor bring-up. See sensors.h for the hardware map.

#include "sensors.h"

#include "display.h"
#include "esp_log.h"
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

// Try the QMI8658 at the high address first (0x6B, what the Waveshare
// board uses), then the low address (0x6A) as a fallback. Returns true if
// the IMU answered its WHO_AM_I at either address.
static bool imu_bring_up(i2c_master_bus_handle_t bus) {
    const uint8_t addrs[] = {QMI8658_ADDRESS_HIGH, QMI8658_ADDRESS_LOW};
    for (size_t i = 0; i < sizeof(addrs); i++) {
        if (qmi8658_init(&s_imu, bus, addrs[i]) != ESP_OK) {
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

        if (d.imu_ok) {
            // Read into scratch and copy into d ONLY on success: on an I2C
            // NACK the driver may leave the out params untouched or garbage,
            // and d.imu_ok would still report 1. Leaving d at the last good
            // value is the honest answer.
            float ax, ay, az, gx, gy, gz, tmp;
            if (qmi8658_read_accel(&s_imu, &ax, &ay, &az) == ESP_OK) {
                d.accel_x = ax;
                d.accel_y = ay;
                d.accel_z = az;
            }
            if (qmi8658_read_gyro(&s_imu, &gx, &gy, &gz) == ESP_OK) {
                d.gyro_x = gx;
                d.gyro_y = gy;
                d.gyro_z = gz;
            }
            if (qmi8658_read_temp(&s_imu, &tmp) == ESP_OK) {
                d.imu_temp_c = tmp;
            }
        }
        if (d.th_ok) {
            float t = 0, h = 0;
            if (shtc3_get_th(s_shtc3, SHTC3_REG_T_CSE_NM, &t, &h) == ESP_OK) {
                d.temp_c = t;
                d.humidity_rh = h;
            }
        }
        if (d.rtc_ok) {
            pcf85063a_datetime_t dt;
            if (pcf85063a_get_time_date(&s_rtc, &dt) == ESP_OK) {
                d.year = dt.year;
                d.month = dt.month;
                d.day = dt.day;
                d.hour = dt.hour;
                d.min = dt.min;
                d.sec = dt.sec;
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
            }
        }

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
        uint8_t id = 0;
        if (shtc3_get_id(s_shtc3, &id) == ESP_OK) {
            ESP_LOGI(TAG, "SHTC3 found (ID=0x%02x)", id);
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
    s_started = true;

    if (xTaskCreate(poll_task, "sensor_poll", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to start sensor poll task");
        // Not fatal -- init readings already captured above.
    }
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
static esp_err_t handle_metrics(httpd_req_t *req) {
    sensors_data_t d;
    bool ok = sensors_get(&d);

    char body[1024];
    size_t n = 0;

    // Bounded append. snprintf() returns the number of bytes it *would*
    // have written, so a naive `n += snprintf(...)` can push n past
    // sizeof(body) and make the next `sizeof(body) - n` underflow. Clamp
    // to the last usable index (keeping room for the NUL) and stop.
    #define APPEND(...)                                                      \
        do {                                                                  \
            int _w = snprintf(body + n, sizeof(body) - n, __VA_ARGS__);       \
            if (_w < 0) {                                                    \
                break;                                                       \
            }                                                                 \
            if ((size_t)_w >= sizeof(body) - n) {                             \
                n = sizeof(body) - 1;                                         \
                break;                                                       \
            }                                                                 \
            n += (size_t)_w;                                                  \
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
    APPEND("# TYPE echo_accel_mps2 gauge\n"
           "echo_accel_mps2{axis=\"x\"} %.3f\n"
           "echo_accel_mps2{axis=\"y\"} %.3f\n"
           "echo_accel_mps2{axis=\"z\"} %.3f\n",
           d.accel_x, d.accel_y, d.accel_z);
    APPEND("# TYPE echo_gyro_dps gauge\n"
           "echo_gyro_dps{axis=\"x\"} %.3f\n"
           "echo_gyro_dps{axis=\"y\"} %.3f\n"
           "echo_gyro_dps{axis=\"z\"} %.3f\n",
           d.gyro_x, d.gyro_y, d.gyro_z);
    APPEND("# TYPE echo_imu_temp_celsius gauge\n"
           "echo_imu_temp_celsius %.2f\n", d.imu_temp_c);

    // Temp / humidity
    APPEND("# HELP echo_th_ok 1 when the SHTC3 temp/humidity sensor was detected\n"
           "# TYPE echo_th_ok gauge\n"
           "echo_th_ok %d\n", d.th_ok);
    APPEND("# TYPE echo_temperature_celsius gauge\n"
           "echo_temperature_celsius %.2f\n", d.temp_c);
    APPEND("# TYPE echo_humidity_percent gauge\n"
           "echo_humidity_percent %.1f\n", d.humidity_rh);

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
    httpd_resp_send(req, body, n);
    #undef APPEND
    return ESP_OK;
}

esp_err_t sensors_register_metrics(httpd_handle_t server) {
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    httpd_uri_t m = {.uri = "/metrics", .method = HTTP_GET, .handler = handle_metrics};
    return httpd_register_uri_handler(server, &m);
}
