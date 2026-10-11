// echo -- LVGL UI layer. See ui.h for the design contract.

#include "ui.h"
#include "ui_graphs.h"

#include "display.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "touch.h"
#include "audio.h"
#include "sensors.h"
#include "ha_client.h"
#include "ota.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <math.h>
#include <stdio.h>

static const char *TAG = "ui";

// Panel geometry -- must match display.c (ST7789, 240x320 portrait).
#define UI_H_RES 240
#define UI_V_RES 320

static lv_disp_t *s_disp = NULL;
static lv_obj_t *s_vol_label = NULL;
static lv_obj_t *s_bat_label = NULL;
static lv_obj_t *s_sensor_label = NULL;
static lv_obj_t *s_status_label = NULL;

// Page 1 status bar: phone-style WiFi signal-strength icon (4 bars, left of
// the battery readout). Heights are fixed; only the colour changes with the
// live RSSI so the icon reads as a strength meter, not a fixed glyph.
// The bars live in a fixed-size box that is re-anchored to the battery
// label's left edge, because the label width varies with the SOC text.
static lv_obj_t *s_wifi_box = NULL;
static lv_obj_t *s_wifi_bars[4] = {0};

// The five charts that used to live here (temp/hum/accel/gyro/SOC) are now
// ONE selectable graph on the consolidated "graph" page (ui_graphs.c).

// Page 4 (Home Assistant temperatures): a scrollable list, one row per
// configured HA entity. Rows are rebuilt from the ha_get() snapshot by a
// timer (see ha_timer_cb) rather than by the network task -- only the LVGL
// task ever touches these objects.
static lv_obj_t *s_ha_list = NULL;
static lv_obj_t *s_ha_status = NULL;
// Tile 5: HA sensor-battery list (same physical sensors as the temp page,
// but a separate set of HA entities -- device_class=battery).
static lv_obj_t *s_batt_list = NULL;
static lv_obj_t *s_batt_status = NULL;

// A single long-lived player task consumes play-requests. A song-list
// press sets s_requested_index and (if something is playing) asks the
// current song to stop early -- so pressing a new song interrupts the
// current one and starts the new one. s_req_gen is the handoff counter:
// the player consumes a request by remembering the generation it last saw.
//
// Song playback is now owned by the audio service (audio.c): the UI only
// enqueues via audio_post_song() in song_btn_cb below. The former UI-owned
// player_task + request-handoff machinery was removed (V03: single owner).
// in-flight song first). The UI never calls blocking audio or touches I2C.
static void song_btn_cb(lv_event_t *e) {
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    const audio_song_t *song = audio_get_song(index);
    ESP_LOGI(TAG, "song button pressed: %s", song ? song->name : "?");
    if (!audio_post_song(index)) {
        ESP_LOGW(TAG, "audio service busy/unavailable -- song request dropped");
    }
}

// Refresh the volume label from the codec's current level.
static void update_vol_label(void) {
    if (s_vol_label == NULL) return;
    int v = audio_get_volume();
    if (v < 0) {
        lv_label_set_text(s_vol_label, "vol --");
    } else {
        lv_label_set_text_fmt(s_vol_label, "vol %d%%", v);
    }
}

// Narrow, thread-safe status-update entry point for other modules (WiFi
// status, OTA progress, ...). Reserving this API now keeps all LVGL object
// ownership inside ui.c -- callers must NOT grab lv_* objects directly.
// Safe from ANY task: it takes the LVGL port lock internally and no-ops if
// the UI has not been built yet.
void ui_set_status(const char *text) {
    if (s_status_label == NULL) return;
    if (lvgl_port_lock(0)) {
        lv_label_set_text(s_status_label, text ? text : "");
        lvgl_port_unlock();
    }
}

// Volume up/down -- audio_adjust_volume is fast (one I2C write), so it's
// safe to call directly on the LVGL event task (unlike the blocking song).
static void vol_up_cb(lv_event_t *e) {
    (void)e;
    audio_post_volume(+10);
    update_vol_label();
}

static void vol_down_cb(lv_event_t *e) {
    (void)e;
    audio_post_volume(-10);
    update_vol_label();
}

// Map RSSI (dBm) to a bar count 0..4. The esp_wifi convention used by ota.c
// is rssi==0 => "not associated / unknown"; a real RSSI is always negative,
// so 0 unambiguously means "no link". When associated, show at least 1 bar
// so a live-but-weak link never looks identical to "offline".
static int wifi_level_from_rssi(int rssi) {
    if (rssi == 0) return 0;
    if (rssi >= -55) return 4;
    if (rssi >= -65) return 3;
    if (rssi >= -75) return 2;
    return 1;
}

// Periodic WiFi-strength refresh (runs on the LVGL task). Recolours the 4
// bars to match the live RSSI; non-blocking (esp_wifi_sta_get_ap_info just
// reads cached driver state).
static void wifi_timer_cb(lv_timer_t *t) {
    (void)t;
    if (s_wifi_bars[0] == NULL) return;
    wifi_ap_record_t ap;
    int rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
    int level = wifi_level_from_rssi(rssi);
    for (int i = 0; i < 4; i++) {
        bool on = (i < level);
        lv_obj_set_style_bg_color(s_wifi_bars[i],
            lv_color_hex(on ? 0x90c0ff : 0x404040), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_wifi_bars[i],
            on ? LV_OPA_COVER : LV_OPA_50, LV_PART_MAIN);
    }
}

// Anchor the WiFi bar box immediately left of the battery label and centre
// it on the label's mid-line. Called every time the battery text changes
// (the label width grows when the SOC number replaces the "--" placeholder).
static void wifi_anchor_bars(void) {
    if (s_wifi_box != NULL && s_bat_label != NULL) {
        lv_obj_align_to(s_wifi_box, s_bat_label, LV_ALIGN_OUT_LEFT_MID, -5, 0);
    }
}

// Periodic battery readout refresh. Runs on the LVGL task (lv_timer), so
// it may touch LVGL objects directly. Reads the latest snapshot from the
// sensors module (non-blocking) and updates the small top-right label.
static void bat_timer_cb(lv_timer_t *t) {
    (void)t;
    if (s_bat_label == NULL) return;
    sensors_data_t d;
    bool have = sensors_get(&d);
    uint32_t bat_now = (uint32_t)(esp_timer_get_time() / 1000);
    bool bat_fresh = have && d.bat_ok && sensors_group_fresh_at(d.bat_last_ms, bat_now);
    if (bat_fresh) {
        // Pick an icon that matches the actual level (was always FULL).
        const char *icon;
        int p = d.bat_soc_pct;
        if (p >= 85) icon = LV_SYMBOL_BATTERY_FULL;
        else if (p >= 60) icon = LV_SYMBOL_BATTERY_3;
        else if (p >= 35) icon = LV_SYMBOL_BATTERY_2;
        else if (p >= 10) icon = LV_SYMBOL_BATTERY_1;
        else icon = LV_SYMBOL_BATTERY_EMPTY;
        lv_label_set_text_fmt(s_bat_label, "%s %d%%", icon, p);
    } else {
        lv_label_set_text(s_bat_label, LV_SYMBOL_BATTERY_EMPTY " --");
    }
    // (Battery SOC graph is on the consolidated graph page now.)
    wifi_anchor_bars();   // label width just changed; keep bars to its left

    // Sensor readout (same snapshot). NOTE: LVGL's lv_label_set_text_fmt
    // does NOT support %f (its printf is built without float), which
    // printed literal "%f"/"fff". Format floats with the full libc
    // snprintf first, then pass the finished string via %s.
    if (s_sensor_label != NULL) {
        sensors_data_t s;
        if (sensors_get(&s)) {
            // R10/V05a: distinguish fresh vs stale/never-read against the
            // READER's current clock (not the producer's frozen publish-time
            // now_ms — a stopped worker would otherwise read fresh forever).
            uint32_t reader_now = (uint32_t)(esp_timer_get_time() / 1000);
            bool th_fresh  = sensors_group_fresh_at(s.th_last_ms, reader_now);
            bool imu_fresh = sensors_group_fresh_at(s.imu_last_ms, reader_now);
            char tbuf[16], hbuf[16], abuf[40], gbuf[40];
            if (th_fresh) {
                snprintf(tbuf, sizeof(tbuf), "%.1f C", s.temp_c);
                snprintf(hbuf, sizeof(hbuf), "%.0f%%", s.humidity_rh);
            } else {
                snprintf(tbuf, sizeof(tbuf), "--");
                snprintf(hbuf, sizeof(hbuf), "--");
            }
            if (imu_fresh) {
                snprintf(abuf, sizeof(abuf), "%.1f %.1f %.1f", s.accel_x, s.accel_y, s.accel_z);
                snprintf(gbuf, sizeof(gbuf), "%.0f %.0f %.0f", s.gyro_x, s.gyro_y, s.gyro_z);
            } else {
                snprintf(abuf, sizeof(abuf), "--");
                snprintf(gbuf, sizeof(gbuf), "--");
            }
            char txt[160];
            snprintf(txt, sizeof(txt),
                     "temp %s   hum %s\n"
                     "accel %s\n"
                     "gyro  %s",
                     tbuf, hbuf, abuf, gbuf);
            lv_label_set_text(s_sensor_label, txt);

            // V06: the chart is a FIXED 2 s wall-time grid (60 pts = 2 min, matching
            // the -2m/-1m/now labels). Append EXACTLY ONE point per series per tick
            // so the timebase stays honest: the current value when fresh, else an
            // explicit gap (LV_CHART_POINT_NONE). Previously a stale group SKIPPED
            // insertion, which froze the chart and made the time labels lie; a fresh
            // group was sample-and-held onto the grid (correct for a fixed grid).
            // Graphs moved to the dedicated page (ui_graphs.c samples
            // sensors_get() itself, so no chart feeding is needed here).
            (void)th_fresh;
            (void)imu_fresh;
        }
    }
}

// Periodic Home-Assistant temperature readout refresh. Runs on the LVGL
// task (lv_timer), so it touches LVGL objects directly. Pulls the last
// snapshot from ha_client (non-blocking) -- this function NEVER does
// network I/O; the ha_poll task owns that, decoupled behind the mutex.
// A row shows "name  temp C" (or "name  --" while the reading is invalid),
// so a dead/unreachable HA degrades visibly instead of showing stale values.
static void ha_timer_cb(lv_timer_t *t) {
    (void)t;
    if (s_ha_list == NULL) return;

    ha_data_t d;
    if (!ha_get(&d)) return;

    // Rebuild the rows only when the count is stable; we simply refresh the
    // text of already-created rows and create any missing ones (the entity
    // list is fixed at compile time, so count is constant after start).
    for (int i = 0; i < d.count; i++) {
        const ha_sensor_t *s = &d.sensors[i];
        char txt[64];
        if (s->ok) {
            // lv_label_set_text_fmt has no %f -- format with libc first.
            snprintf(txt, sizeof(txt), "%-7s %5.1f C", s->name, s->temp_c);
        } else {
            snprintf(txt, sizeof(txt), "%-7s    --  ", s->name);
        }

        lv_obj_t *row = lv_obj_get_child(s_ha_list, i);
        if (row == NULL) {
            row = lv_label_create(s_ha_list);
            lv_obj_set_style_text_font(row, &lv_font_montserrat_14, LV_PART_MAIN);
        }
        // V11: validity color follows CURRENT quality on every refresh, not
        // only at creation — otherwise a recovered value stays grey and a lost
        // value keeps its normal color. (Cheap; LVGL no-ops an unchanged style.)
        lv_obj_set_style_text_color(row,
            s->ok ? lv_color_hex(0xd0d0d0) : lv_color_hex(0x808080), LV_PART_MAIN);
        lv_label_set_text(row, txt);
    }

    if (s_ha_status != NULL) {
        if (d.count == 0) {
            lv_label_set_text(s_ha_status, "no HA entities configured");
        } else if (d.last_http_code == 200) {
            lv_label_set_text_fmt(s_ha_status, "HA ok  (%d sensors)", d.count);
        } else if (!d.wifi_up) {
            lv_label_set_text(s_ha_status, "HA: no WiFi");
        } else {
            lv_label_set_text_fmt(s_ha_status, "HA err %d", d.last_http_code);
        }
    }
}

// Periodic HA sensor-battery refresh (tile 5). Mirrors ha_timer_cb: the
// lv_timer runs on the LVGL task and reads the ha_client battery snapshot
// non-blocking (no network I/O here -- the poll task owns that). Batteries are
// the SAME physical sensors as the temperature page, but they live in separate
// HA entities (device_class=battery), so they get their own list. NOTE: the
// ha_sensor_t value field (temp_c) carries a PERCENT here, not a temperature.
static void batt_page_timer_cb(lv_timer_t *t) {
    (void)t;
    if (s_batt_list == NULL) return;

    ha_data_t d;
    if (!ha_get(&d)) return;

    for (int i = 0; i < d.batt_count; i++) {
        const ha_sensor_t *s = &d.batteries[i];
        char txt[64];
        if (s->ok) {
            snprintf(txt, sizeof(txt), "%-9s  %4.0f%%", s->name, s->temp_c);
        } else {
            snprintf(txt, sizeof(txt), "%-9s    --  ", s->name);
        }
        lv_obj_t *row = lv_obj_get_child(s_batt_list, i);
        if (row == NULL) {
            row = lv_label_create(s_batt_list);
            lv_obj_set_style_text_font(row, &lv_font_montserrat_14, LV_PART_MAIN);
        }
        // Validity colour follows CURRENT quality on every refresh.
        lv_obj_set_style_text_color(row,
            s->ok ? lv_color_hex(0xd0d0d0) : lv_color_hex(0x808080), LV_PART_MAIN);
        lv_label_set_text(row, txt);
    }

    if (s_batt_status != NULL) {
        if (d.batt_count == 0) {
            lv_label_set_text(s_batt_status, "no HA batteries configured");
        } else if (d.last_http_code == 200) {
            lv_label_set_text_fmt(s_batt_status, "HA ok  (%d batterier)", d.batt_count);
        } else if (!d.wifi_up) {
            lv_label_set_text(s_batt_status, "HA: no WiFi");
        } else {
            lv_label_set_text_fmt(s_batt_status, "HA err %d", d.last_http_code);
        }
    }
}

// Build the UI: a 2-tile tileview (swipe left/right). Tile 0 = controls +
// live readout, tile 1 = scrolling sensor graphs. Caller holds the LVGL
// port lock.
// ---- Tile 5: device info ----------------------------------------------
// A single multi-line label refreshed on a slow timer. Pulls build/version
// from the app descriptor, SSID + IP from the live WiFi/netif state, and
// uptime from esp_timer. All read-only queries, safe on the LVGL task.
static lv_obj_t *s_info_label = NULL;

static void info_timer_cb(lv_timer_t *t) {
    (void)t;
    if (s_info_label == NULL) return;

    const esp_app_desc_t *app = esp_app_get_description();

    // SSID from the current STA config (falls back to the compile-time one).
    char ssid[33] = "?";
    wifi_config_t wc;
    if (esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK && wc.sta.ssid[0]) {
        snprintf(ssid, sizeof(ssid), "%s", (const char *)wc.sta.ssid);
    }

    // IP from the STA netif.
    char ip[16] = "0.0.0.0";
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ipinfo;
    if (netif && esp_netif_get_ip_info(netif, &ipinfo) == ESP_OK) {
        esp_ip4addr_ntoa(&ipinfo.ip, ip, sizeof(ip));
    }

    // Uptime from monotonic timer (seconds since boot).
    int64_t up_s = esp_timer_get_time() / 1000000;
    int d = (int)(up_s / 86400);
    int h = (int)((up_s % 86400) / 3600);
    int m = (int)((up_s % 3600) / 60);
    int s = (int)(up_s % 60);

    // OTA debug: whether an OTA was attempted, how long ago, and its result.
    ota_debug_t ota;
    ota_debug_get(&ota);
    char ota_line[96];
    if (!ota.attempted) {
        snprintf(ota_line, sizeof(ota_line), "ota:    none since boot");
    } else {
        int64_t ago_s = (esp_timer_get_time() - ota.last_start_us) / 1000000;
        const char *state = ota.in_progress ? "RUNNING" : ota.last_result;
        snprintf(ota_line, sizeof(ota_line), "ota:    #%u, %llds ago: %s",
                 ota.attempt_count, (long long)ago_s, state);
    }

    // WiFi band/channel/RSSI for the info page (channel 1-14 = 2.4 GHz,
    // 36+ = 5 GHz). "down" if not associated.
    char wifi_line[48];
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        snprintf(wifi_line, sizeof(wifi_line), "wifi:   %s ch%u  %ddBm",
                 (ap.primary >= 36) ? "5GHz" : "2.4GHz", ap.primary, ap.rssi);
    } else {
        snprintf(wifi_line, sizeof(wifi_line), "wifi:   down");
    }

    // MAC address of the station interface (handy for DHCP/router lookups).
    uint8_t mac[6] = {0};
    char mac_line[28] = "mac:    --";
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(mac_line, sizeof(mac_line), "mac:    %02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    // Free heap (KB) -- the single most useful "is it healthy" number.
    unsigned heap_kb = (unsigned)(esp_get_free_heap_size() / 1024u);

    char up_line[32];
    if (d > 0) snprintf(up_line, sizeof(up_line), "uptime: %dd %02d:%02d:%02d", d, h, m, s);
    else       snprintf(up_line, sizeof(up_line), "uptime: %02d:%02d:%02d", h, m, s);

    // 9 short lines at Montserrat 12 fit the 240x320 panel without clipping.
    char txt[512];
    snprintf(txt, sizeof(txt),
             "build:  %s\nidf:    %s\n%s\nssid:   %s\nip:     %s\n%s\nheap:   %u KB\n%s\n%s",
             app->version, app->idf_ver, mac_line, ssid, ip, wifi_line,
             heap_kb, up_line, ota_line);
    lv_label_set_text(s_info_label, txt);
}

static void build_test_ui(void) {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), LV_PART_MAIN);

    lv_obj_t *tv = lv_tileview_create(scr);
    lv_obj_set_style_bg_color(tv, lv_color_hex(0x101418), LV_PART_MAIN);
    // Page order (left -> right). Device info is FIRST, the home page
    // (controls + song list) LAST, and the HA battery page sits right after
    // the HA temperature page. The tileview shows column 0 first.
    lv_obj_t *t5 = lv_tileview_add_tile(tv, 0, 0, LV_DIR_RIGHT);          // device info
    // Tile 1: the consolidated graph page (dropdown picks which metric).
    lv_obj_t *t1 = lv_tileview_add_tile(tv, 1, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    // Tile 4: Home Assistant temperatures (list of configured sensors).
    lv_obj_t *t4 = lv_tileview_add_tile(tv, 4, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    // Tile 5: Home Assistant sensor batteries (same sensors as tile 4).
    lv_obj_t *tb = lv_tileview_add_tile(tv, 5, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    // Tile 0: home/controls + song list (now the last page).
    lv_obj_t *t0 = lv_tileview_add_tile(tv, 6, 0, LV_DIR_LEFT);
    // Tiles must not scroll internally or they swallow the swipe gesture.
    lv_obj_clear_flag(t0, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(t1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(t4, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(tb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(t5, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Tile 0: controls + readout ----
    lv_obj_t *title = lv_label_create(t0);
    lv_label_set_text(title, "echo");
    lv_obj_set_style_text_color(title, lv_color_hex(0x30d070), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    s_bat_label = lv_label_create(t0);
    lv_obj_set_style_text_color(s_bat_label, lv_color_hex(0x90c0ff), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_bat_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(s_bat_label, LV_SYMBOL_BATTERY_EMPTY " --");
    lv_obj_align(s_bat_label, LV_ALIGN_TOP_RIGHT, -4, 2);

    // Phone-style WiFi signal-strength icon, left of the battery readout.
    // Four bars of increasing height in a fixed 26x14 box, bottom-aligned;
    // wifi_timer_cb recolours them from the live RSSI (fills left-to-right).
    // The box is anchored to the battery label's LEFT edge (same mid-line)
    // each time the battery text changes, so the varying label width can
    // never push the icon under the battery glyph.
    s_wifi_box = lv_obj_create(t0);
    lv_obj_remove_style_all(s_wifi_box);
    lv_obj_set_size(s_wifi_box, 26, 14);
    lv_obj_clear_flag(s_wifi_box, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *bar = lv_obj_create(s_wifi_box);
        lv_obj_remove_style_all(bar);
        int h = 5 + i * 3;                       // 5,8,11,14 px
        lv_obj_set_size(bar, 5, h);
        lv_obj_set_style_radius(bar, 1, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x404040), LV_PART_MAIN);
        lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, i * 7, 0);
        s_wifi_bars[i] = bar;
    }

    lv_timer_create(bat_timer_cb, 2000, NULL);
    bat_timer_cb(NULL);              // draws battery text + anchors the bars
    lv_timer_create(wifi_timer_cb, 3000, NULL);
    wifi_timer_cb(NULL);

    // Reserved status line: other modules report here (via ui_set_status())
    // instead of touching LVGL objects themselves.
    s_status_label = lv_label_create(t0);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x808080), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(s_status_label, "");
    lv_obj_align(s_status_label, LV_ALIGN_TOP_MID, 0, 30);

    s_sensor_label = lv_label_create(t0);
    lv_obj_set_style_text_color(s_sensor_label, lv_color_hex(0xd0d0d0), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_sensor_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(s_sensor_label, "sensors: ...");
    lv_obj_align(s_sensor_label, LV_ALIGN_CENTER, 0, -60);
    lv_obj_set_style_text_align(s_sensor_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    // Scrollable song list -- one button per registered (public-domain)
    // song. Each button carries its song index as user-data; the callback
    // spawns the worker task to play it.
    lv_obj_t *list = lv_list_create(t0);
    lv_obj_set_size(list, 220, 120);
    lv_obj_align(list, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x181c22), LV_PART_MAIN);
    // Let vertical scroll work but still bubble horizontal swipes to the
    // tileview (so page-swipe keeps working from the list).
    lv_obj_add_flag(list, LV_OBJ_FLAG_GESTURE_BUBBLE);
    for (int i = 0; i < audio_song_count(); i++) {
        const audio_song_t *song = audio_get_song(i);
        lv_obj_t *b = lv_list_add_btn(list, LV_SYMBOL_AUDIO, song->name);
        lv_obj_add_event_cb(b, song_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    // Volume controls below the list.
    lv_obj_t *vol_dn = lv_btn_create(t0);
    lv_obj_set_size(vol_dn, 50, 34);
    lv_obj_align(vol_dn, LV_ALIGN_CENTER, -70, 88);
    lv_obj_add_event_cb(vol_dn, vol_down_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *vd = lv_label_create(vol_dn);
    lv_label_set_text(vd, LV_SYMBOL_DOWN);
    lv_obj_center(vd);

    s_vol_label = lv_label_create(t0);
    lv_obj_set_style_text_color(s_vol_label, lv_color_hex(0xc0c0c0), LV_PART_MAIN);
    lv_obj_align(s_vol_label, LV_ALIGN_CENTER, 0, 88);
    update_vol_label();

    lv_obj_t *vol_up = lv_btn_create(t0);
    lv_obj_set_size(vol_up, 50, 34);
    lv_obj_align(vol_up, LV_ALIGN_CENTER, 70, 88);
    lv_obj_add_event_cb(vol_up, vol_up_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *vu = lv_label_create(vol_up);
    lv_label_set_text(vu, LV_SYMBOL_UP);
    lv_obj_center(vu);

    lv_obj_t *hint = lv_label_create(t0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x606060), LV_PART_MAIN);
    lv_label_set_text(hint, LV_SYMBOL_LEFT " swipe right");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);

    // ---- Tile 1: consolidated graph page (one selectable graph) ----
    ui_graphs_create(t1);

    // ---- Tile 4: Home Assistant temperatures ----
    // A scrollable list, one row per configured HA entity. The rows are
    // (re)built by ha_timer_cb from the ha_client snapshot; the network
    // poll runs on its own task, so this page never blocks on HA.
    lv_obj_t *gt4 = lv_label_create(t4);
    lv_obj_set_style_text_color(gt4, lv_color_hex(0x40c0ff), LV_PART_MAIN);
    lv_label_set_text(gt4, LV_SYMBOL_LEFT " HA temps " LV_SYMBOL_RIGHT);
    lv_obj_align(gt4, LV_ALIGN_TOP_MID, 0, 4);

    s_ha_status = lv_label_create(t4);
    lv_obj_set_style_text_color(s_ha_status, lv_color_hex(0x808080), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ha_status, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(s_ha_status, "HA: waiting...");
    lv_obj_align(s_ha_status, LV_ALIGN_TOP_MID, 0, 26);

    s_ha_list = lv_obj_create(t4);
    lv_obj_set_size(s_ha_list, 210, 210);
    lv_obj_align(s_ha_list, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_set_style_bg_color(s_ha_list, lv_color_hex(0x181c22), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ha_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ha_list, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_ha_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_ha_list, 6, LV_PART_MAIN);
    // Let vertical scroll work but still bubble horizontal swipes to the
    // tileview (same fix as the song list -- otherwise the page traps you).
    lv_obj_add_flag(s_ha_list, LV_OBJ_FLAG_GESTURE_BUBBLE);
    // Refresh every 2 s (same cadence as the battery/sensor readout). The
    // underlying ha_client snapshot only changes every ~30 s; this just
    // keeps the "--" -> value transition snappy.
    lv_timer_create(ha_timer_cb, 2000, NULL);
    ha_timer_cb(NULL); // paint the initial (meta-only, "--") rows now

    // ---- Tile 5: HA sensor batteries (the tile-4 sensors' battery entities) ----
    lv_obj_t *gtb = lv_label_create(tb);
    lv_obj_set_style_text_color(gtb, lv_color_hex(0xffd040), LV_PART_MAIN);
    lv_label_set_text(gtb, LV_SYMBOL_LEFT " HA batteri " LV_SYMBOL_RIGHT);
    lv_obj_align(gtb, LV_ALIGN_TOP_MID, 0, 4);

    s_batt_status = lv_label_create(tb);
    lv_obj_set_style_text_color(s_batt_status, lv_color_hex(0x808080), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_batt_status, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(s_batt_status, "HA: waiting...");
    lv_obj_align(s_batt_status, LV_ALIGN_TOP_MID, 0, 26);

    s_batt_list = lv_obj_create(tb);
    lv_obj_set_size(s_batt_list, 210, 210);
    lv_obj_align(s_batt_list, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_set_style_bg_color(s_batt_list, lv_color_hex(0x181c22), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_batt_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_batt_list, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_batt_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_batt_list, 6, LV_PART_MAIN);
    // Bubble horizontal swipes to the tileview (same fix as the other lists).
    lv_obj_add_flag(s_batt_list, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_timer_create(batt_page_timer_cb, 2000, NULL);
    batt_page_timer_cb(NULL); // paint the initial ("--") rows now

    // ---- Tile 5: device info (build/version/SSID/IP/uptime) ----
    lv_obj_t *gt5 = lv_label_create(t5);
    lv_obj_set_style_text_color(gt5, lv_color_hex(0x30d070), LV_PART_MAIN);
    lv_label_set_text(gt5, "device " LV_SYMBOL_RIGHT);
    lv_obj_align(gt5, LV_ALIGN_TOP_MID, 0, 4);

    s_info_label = lv_label_create(t5);
    lv_obj_set_style_text_color(s_info_label, lv_color_hex(0xd0d0d0), LV_PART_MAIN);
    // Montserrat 12 (not the default 14) so the extra rows fit vertically.
    lv_obj_set_style_text_font(s_info_label, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_label_set_text(s_info_label, "build: ...");
    lv_obj_set_width(s_info_label, 232);
    lv_label_set_long_mode(s_info_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_info_label, LV_ALIGN_LEFT_MID, 4, 0);
    // Uptime ticks every second; the rest is effectively static.
    lv_timer_create(info_timer_cb, 1000, NULL);
    info_timer_cb(NULL); // paint immediately
}

// NOTE: ui_init() returns only esp_err_t and keeps all UI state file-static
// in this file. If a second screen / UI instance is ever introduced, change
// this to `ui_init(ui_t **out)` so the caller owns the state rather than
// adding more file-static globals here.
esp_err_t ui_init(void) {
    esp_lcd_panel_handle_t panel = display_get_panel();
    esp_lcd_panel_io_handle_t panel_io = display_get_panel_io();
    esp_lcd_touch_handle_t touch = touch_get_handle();

    if (panel == NULL || panel_io == NULL) {
        ESP_LOGE(TAG, "display not initialized -- call display_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    // 1. Init the LVGL port (creates its LVGL task + tick timer).
    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    esp_err_t ret = lvgl_port_init(&port_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init failed: 0x%x", ret);
        return ret;
    }

    // 2. Attach the existing ST7789 panel as an LVGL display.
    //    buffer_size = a partial-screen buffer (1/10 of the frame) in
    //    internal RAM, DMA-capable. NO swap_bytes: display.c already set
    //    the ST7789 data_endian to LITTLE to match the little-endian
    //    uint16_t pixel stores, so the bytes are already correct -- adding
    //    a swap here would re-break the green/red channel order.
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io,
        .panel_handle = panel,
        // Shrunk from /10 double-buffer (~30 KB DMA RAM) to /20 single-buffer
        // (~7.5 KB) so the DMA-internal draw buffer leaves room for the I2S
        // audio DMA buffers (see .flags note below, 2026-10-10).
        .buffer_size = UI_H_RES * UI_V_RES / 20,
        .double_buffer = false,
        .hres = UI_H_RES,
        .vres = UI_V_RES,
        .monochrome = false,
        .rotation = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
        .flags = {
            // Keep the draw buffer in DMA-capable INTERNAL RAM: the SPI-LCD
            // DMA reads it directly, and putting it in PSRAM stalled the LVGL
            // flush-ready loop (flush never completed -> LVGL task blocked ->
            // TOUCH polling, on the same task, went dead). But the old config
            // (double buffer, 2 x 240*320/10 ~= 30 KB) starved the I2S audio
            // DMA (only ~210 B DMA RAM left -> audio init NO_MEM). Fix: stay
            // DMA-internal but SHRINK — single buffer at 1/20 of the frame
            // (~7.5 KB) — so the display flush works AND ~22 KB of DMA RAM is
            // left for I2S. Both fit the scarce internal DMA pool. (2026-10-10)
            .buff_dma = true,
            .buff_spiram = false,
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (s_disp == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return ESP_FAIL;
    }

    // 3. Attach the CST3530 touch as an LVGL input device (if present --
    //    a missing touch must not block the display coming up).
    if (touch != NULL) {
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = s_disp,
            .handle = touch,
        };
        if (lvgl_port_add_touch(&touch_cfg) == NULL) {
            ESP_LOGW(TAG, "lvgl_port_add_touch failed -- UI will be display-only");
        }
    } else {
        ESP_LOGW(TAG, "no touch handle -- UI will be display-only");
    }

    // Song playback is owned by the audio command-service (audio.c); the UI
    // enqueues via audio_post_song(). No UI-owned player task is created here.

    // 4. Build the initial UI under the LVGL lock.
    if (lvgl_port_lock(0)) {
        build_test_ui();
        lvgl_port_unlock();
    } else {
        ESP_LOGE(TAG, "could not take LVGL lock to build UI");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "LVGL UI init success (%dx%d)", UI_H_RES, UI_V_RES);
    return ESP_OK;
}
