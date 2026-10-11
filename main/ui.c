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
#include <time.h>

static const char *TAG = "ui";

// Panel geometry -- must match display.c (ST7789, 240x320 portrait).
#define UI_H_RES 240
#define UI_V_RES 320

static lv_disp_t *s_disp = NULL;
static lv_obj_t *s_vol_label = NULL;
// Paginated song list (fixed-row: object count = viewport, NOT song count — so
// the LVGL pool no longer caps the number of songs; v110 review memory fix #2).
#define SONG_ROWS 5
static lv_obj_t *s_song_rows[SONG_ROWS] = {0};   // fixed row buttons
static lv_obj_t *s_song_row_lbls[SONG_ROWS] = {0};
static lv_obj_t *s_song_page_lbl = NULL;
static int s_song_page = 0;
static lv_obj_t *s_bat_label = NULL;
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

// K01 Overview + A11 idle return + I01 cleaning lock state.
static lv_obj_t *s_tileview = NULL;      // the tileview (for programmatic nav)
static lv_obj_t *s_ov_tile = NULL;       // the Overview tile (landing target)
static lv_obj_t *s_ov_climate = NULL;    // overview: local climate line
static lv_obj_t *s_ov_batt = NULL;       // overview: battery line
static lv_obj_t *s_ov_net = NULL;        // overview: connectivity line (wifi band + HA)
static lv_obj_t *s_ov_outdoor = NULL;    // overview: outdoor temp (Ute-Aquara, A17)
static lv_obj_t *s_ov_elec_hdr = NULL;   // overview: "Electricity" heading (A17)
static lv_obj_t *s_ov_wifi_box = NULL;   // overview: top-right WiFi strength icon (A17)
static lv_obj_t *s_ov_wifi_bars[4] = {0};
static lv_obj_t *s_ov_power = NULL;      // overview: solar/house power line (A16)
static lv_obj_t *s_ov_time = NULL;       // overview: wall clock (HH:MM, SNTP)
#define OV_LANDING_COL 0                 // Overview is tileview column 0
// A11: idle-return. last-activity monotonic ms; return to landing after idle.
static uint32_t s_last_activity_ms = 0;
static bool s_pin_here = false;          // "Keep here": opt out of idle return
#define IDLE_RETURN_MS 120000u           // 2 min inactivity -> back to Overview
// I01: cleaning lock. While active, touch is ignored (input disabled) and an
// overlay shows a countdown; auto-unlocks at the deadline.
static lv_obj_t *s_clean_overlay = NULL; // full-screen lock overlay
static lv_obj_t *s_clean_label = NULL;   // countdown text
static int64_t s_clean_until_us = 0;     // 0 = not locked
#define CLEAN_LOCK_MS 30000              // 30 s wipe window

// --- Milestone C: comfort rules (A05) + offline timer (I02) + dim/wake (K05) ---
#include "comfort_rules.h"
// A05: two local rules evaluated on the sensor path, visual-only advisory.
static const comfort_rule_t s_rules[] = {
    { "High humidity", COMFORT_ABOVE, 70.0f, 65.0f, 10u * 60u * 1000u }, // >=70% RH 10 min
    { "Low temp",      COMFORT_BELOW, 15.0f, 17.0f, 10u * 60u * 1000u }, // <=15 C 10 min
};
#define N_RULES ((int)(sizeof(s_rules) / sizeof(s_rules[0])))
static comfort_rt_t s_rule_rt[N_RULES];
static lv_obj_t *s_ov_advisory = NULL;   // overview advisory line

// I02: offline monotonic timer. deadline_us is a monotonic esp_timer value;
// WiFi/HA loss cannot affect it. 0 = no timer running.
static int64_t s_timer_deadline_us = 0;
static bool s_timer_fired = false;
static lv_obj_t *s_timer_label = NULL;
#define TIMER_PRESET_MS 300000           // 5 min preset

// K05: dim the backlight after inactivity; first touch while dimmed is CONSUMED
// (restores brightness, no underlying action). Civil-time schedule deferred.
static bool s_dimmed = false;
#define DIM_AFTER_MS 60000u              // dim after 60 s idle
#define DIM_LEVEL 10                     // dimmed backlight %
#define BRIGHT_LEVEL 100                 // normal backlight %

// A single long-lived player task consumes play-requests. A song-list
// press sets s_requested_index and (if something is playing) asks the
// current song to stop early -- so pressing a new song interrupts the
// current one and starts the new one. s_req_gen is the handoff counter:
// the player consumes a request by remembering the generation it last saw.
//
// Song playback is now owned by the audio service (audio.c): the UI only
// enqueues via audio_post_song() in song_btn_cb below. The former UI-owned
// player_task + request-handoff machinery was removed (V03: single owner).
// Paginated song list: a row's user-data is its ROW index (0..SONG_ROWS-1);
// the stable song index is s_song_page*SONG_ROWS + row. Validated at tap time
// so a shrunk list / stale page can't launch the wrong song.
static void song_render_page(void) {
    int total = audio_song_count();
    int base = s_song_page * SONG_ROWS;
    for (int r = 0; r < SONG_ROWS; r++) {
        int idx = base + r;
        if (!s_song_rows[r]) continue;
        if (idx < total) {
            const audio_song_t *song = audio_get_song(idx);
            lv_label_set_text(s_song_row_lbls[r],
                              song && song->name ? song->name : "?");
            lv_obj_clear_flag(s_song_rows[r], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_song_rows[r], LV_OBJ_FLAG_HIDDEN);  // empty slot
        }
    }
    if (s_song_page_lbl) {
        int pages = (total + SONG_ROWS - 1) / SONG_ROWS;
        if (pages < 1) pages = 1;
        lv_label_set_text_fmt(s_song_page_lbl, "%d/%d", s_song_page + 1, pages);
    }
}

static void song_btn_cb(lv_event_t *e) {
    int row = (int)(intptr_t)lv_event_get_user_data(e);
    int idx = s_song_page * SONG_ROWS + row;        // stable index at tap time
    if (idx < 0 || idx >= audio_song_count()) return;  // guard shrunk/stale page
    const audio_song_t *song = audio_get_song(idx);
    ESP_LOGI(TAG, "song pressed: [%d] %s", idx, song ? song->name : "?");
    if (!audio_post_song(idx)) {
        ESP_LOGW(TAG, "audio service busy/unavailable -- song request dropped");
    }
}

static void song_prev_cb(lv_event_t *e) {
    (void)e;
    if (s_song_page > 0) { s_song_page--; song_render_page(); }
}
static void song_next_cb(lv_event_t *e) {
    (void)e;
    int pages = (audio_song_count() + SONG_ROWS - 1) / SONG_ROWS;
    if (s_song_page < pages - 1) { s_song_page++; song_render_page(); }
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

// ---- A11 idle-return + I01 cleaning lock + K05 dim/wake helpers (LVGL task) ----

// K05: apply the backlight level for the current dim state (on the UI task, not
// in a touch callback — I2C via board_backlight_set is fine here).
static void ui_set_dim(bool dim) {
    if (dim == s_dimmed) return;
    s_dimmed = dim;
    display_set_backlight(dim ? DIM_LEVEL : BRIGHT_LEVEL);
}

// Record user activity (resets the idle-return + dim timers).
static void ui_note_activity(void) {
    s_last_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

static bool ui_clean_locked(void) { return s_clean_until_us != 0; }

// Global screen input observer.
//   K05 wake-consume: if DIMMED, the first touch wakes the screen and is
//     SWALLOWED (no underlying widget action), including the whole press→
//     release→click stroke that started while dimmed (held finger can't act).
//   A11 activity: any (non-waking) input resets the idle timer.
static bool s_wake_swallow = false;   // true from a dimmed-press until its release
static void ui_screen_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    bool is_input = (code == LV_EVENT_PRESSED || code == LV_EVENT_RELEASED ||
                     code == LV_EVENT_CLICKED || code == LV_EVENT_GESTURE ||
                     code == LV_EVENT_PRESSING);
    if (!is_input) return;

    if (code == LV_EVENT_PRESSED && s_dimmed) {
        ui_set_dim(false);          // wake (restore brightness)
        ui_note_activity();
        s_wake_swallow = true;      // swallow the rest of THIS stroke
        lv_event_stop_processing(e);
        return;
    }
    if (s_wake_swallow) {
        ui_note_activity();
        if (code == LV_EVENT_RELEASED || code == LV_EVENT_CLICKED) {
            s_wake_swallow = false; // stroke ended; next press acts normally
        }
        lv_event_stop_processing(e);
        return;
    }

    ui_note_activity();     // A11: any (non-waking) input resets the idle timer
}

// I01: start the cleaning lock — a full-screen overlay that eats touches and
// shows a countdown; auto-unlocks after CLEAN_LOCK_MS.
static void ui_clean_lock_start(void) {
    if (s_clean_overlay == NULL) return;
    s_clean_until_us = esp_timer_get_time() + (int64_t)CLEAN_LOCK_MS * 1000;
    lv_obj_clear_flag(s_clean_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_clean_overlay);
    ESP_LOGI(TAG, "cleaning lock: touch disabled for %d s", CLEAN_LOCK_MS / 1000);
}

static void ui_clean_lock_end(void) {
    s_clean_until_us = 0;
    if (s_clean_overlay) lv_obj_add_flag(s_clean_overlay, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "cleaning lock: released");
}

static void clean_btn_cb(lv_event_t *e) {
    (void)e;
    ui_clean_lock_start();
}

// I02: start a preset timer if none running; cancel if one is. Monotonic
// deadline (esp_timer) — unaffected by WiFi/HA/clock changes.
static void timer_btn_cb(lv_event_t *e) {
    (void)e;
    if (s_timer_deadline_us != 0) {               // running -> cancel
        s_timer_deadline_us = 0;
        s_timer_fired = false;
        if (s_timer_label) lv_label_set_text(s_timer_label, LV_SYMBOL_BELL " Timer");
        ESP_LOGI(TAG, "I02: timer cancelled");
    } else {                                      // idle/fired -> (re)start
        s_timer_fired = false;
        s_timer_deadline_us = esp_timer_get_time() + (int64_t)TIMER_PRESET_MS * 1000;
        ESP_LOGI(TAG, "I02: timer started (%d min)", TIMER_PRESET_MS / 60000);
    }
}

// K01 overview refresh + I01 countdown + A11 idle-return (1 s cadence).
static void overview_timer_cb(lv_timer_t *t) {
    (void)t;
    int64_t now_us = esp_timer_get_time();

    // --- I01: cleaning-lock countdown ---
    if (ui_clean_locked()) {
        int64_t left_ms = (s_clean_until_us - now_us) / 1000;
        if (left_ms <= 0) {
            ui_clean_lock_end();
        } else if (s_clean_label) {
            lv_label_set_text_fmt(s_clean_label,
                "Cleaning\nTouch locked\n\n%lld s", (long long)((left_ms + 999) / 1000));
        }
        return;   // while locked, don't refresh/return underneath the overlay
    }

    // Wall clock (SNTP). Only show real time once synced (year >= 2021); never
    // fabricate time from uptime (review rule).
    if (s_ov_time) {
        time_t t = time(NULL);
        struct tm tmv;
        localtime_r(&t, &tmv);
        if (tmv.tm_year + 1900 >= 2021) {
            lv_label_set_text_fmt(s_ov_time, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
        } else {
            lv_label_set_text(s_ov_time, "--:--");
        }
    }

    // --- K01: refresh overview from existing snapshots (no new polling) ---
    // NOTE: these snapshot structs are LARGE (sensors_data_t, and ha_data_t is
    // ~1.7KB). They MUST be static, not stack locals — the LVGL task stack is
    // only ~4KB and three ha_data_t copies here overflowed it (stack-protection
    // panic after UI init). Single-task (LVGL timer), so static is safe.
    uint64_t rnow = (uint64_t)now_us;
    static sensors_data_t s;
    static ha_data_t hd;
    bool have_ha = ha_get(&hd);
    if (sensors_get(&s)) {
        bool th_fresh = sensors_group_fresh_at(s.th_last_us, rnow);
        if (s_ov_climate) {
            if (th_fresh) {
                // LVGL's set_text_fmt has NO %f support -> format floats with
                // libc snprintf, then pass the finished string via %s.
                char cl[48];
                snprintf(cl, sizeof(cl), "Climate\n%.1f C   %.0f%% RH\nfresh",
                         s.temp_c, s.humidity_rh);
                lv_label_set_text(s_ov_climate, cl);
            } else {
                lv_label_set_text(s_ov_climate, "Climate\n--\nunavailable");
            }
        }
        if (s_ov_batt) {
            bool bat_fresh = s.bat_ok && sensors_group_fresh_at(s.bat_last_us, rnow);
            if (bat_fresh) {
                // SOC is uncalibrated; the "(est)" suffix was dropped per
                // UX request -- show the value only (no calibration claim).
                lv_label_set_text_fmt(s_ov_batt, LV_SYMBOL_BATTERY_2 " %d%%", s.bat_soc_pct);
            } else {
                lv_label_set_text(s_ov_batt, LV_SYMBOL_BATTERY_EMPTY " --");
            }
        }

        // A05: feed the comfort rules. 'valid' gates on th freshness so stale
        // data can never trigger. Rule 0=humidity, rule 1=temperature.
        uint32_t rnow_ms = (uint32_t)(now_us / 1000);
        bool th_valid = sensors_group_fresh_at(s.th_last_us, rnow);
        comfort_rule_update(&s_rules[0], &s_rule_rt[0], th_valid, s.humidity_rh, rnow_ms);
        comfort_rule_update(&s_rules[1], &s_rule_rt[1], th_valid, s.temp_c, rnow_ms);
    }

    // A05: show the first ACTIVE advisory (or clear the line). Visual-only.
    if (s_ov_advisory) {
        const char *msg = NULL;
        for (int i = 0; i < N_RULES; i++) {
            if (s_rule_rt[i].state == COMFORT_ACTIVE) { msg = s_rules[i].label; break; }
        }
        if (msg) {
            char a[40];
            snprintf(a, sizeof(a), LV_SYMBOL_WARNING " %s", msg);
            lv_label_set_text(s_ov_advisory, a);
            lv_obj_clear_flag(s_ov_advisory, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_ov_advisory, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Connectivity: WiFi band (signal) + a DISTINCT HA-reachable state.
    if (s_ov_net) {
        wifi_ap_record_t ap;
        bool linked = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);
        const char *band = !linked ? "offline" : (ap.primary >= 36 ? "5GHz" : "2.4GHz");
        bool ha_ok = have_ha && hd.last_http_code == 200;
        lv_label_set_text_fmt(s_ov_net, "WiFi %s  |  HA %s",
                              band, ha_ok ? "ok" : "--");
    }

    // Outdoor temperature (A17): the first catalogue temperature entry is
    // Ute-Aquara (catalogue/entities.yaml) -- the outdoor sensor. Shown as
    // "Outdoor" rather than the raw entity name; replaces the old generic
    // "first device temperature" summary line.
    if (s_ov_outdoor) {
        if (have_ha && hd.count > 0 && hd.sensors[0].ok) {
            char hl[32];
            snprintf(hl, sizeof(hl), "Outdoor %.1f C", hd.sensors[0].temp_c);
            lv_label_set_text(s_ov_outdoor, hl);
        } else {
            lv_label_set_text(s_ov_outdoor, "Outdoor --");
        }
    }

    // A16: solar + house power for the Overview, built from the ha_get()
    // snapshot. Each entry carries its own unit (W live, kWh energy); W is
    // shown integral, kWh with 2 decimals. Missing/stale values are skipped
    // (never a fake 0) so a dead entity just drops out of the line.
    if (s_ov_power) {
        if (have_ha && hd.power_count > 0) {
            char pl[80];
            int off = 0;
            for (int i = 0; i < hd.power_count && off < (int)sizeof(pl) - 16; i++) {
                const ha_sensor_t *p = &hd.power[i];
                if (!p->ok) continue;
                const char *u = p->unit[0] ? p->unit : "";
                if (u[0] == 'k') {
                    off += snprintf(pl + off, sizeof(pl) - (size_t)off,
                                    "%s%.2f%s ", p->name, p->temp_c, u);
                } else {
                    off += snprintf(pl + off, sizeof(pl) - (size_t)off,
                                    "%s%u%s ", p->name, (unsigned)(p->temp_c + 0.5f), u);
                }
            }
            if (off == 0) lv_label_set_text(s_ov_power, "--");
            else          lv_label_set_text(s_ov_power, pl);
        } else {
            lv_label_set_text(s_ov_power, "--");
        }
    }

    // --- I02: offline monotonic countdown timer ---
    if (s_timer_label) {
        if (s_timer_deadline_us != 0) {
            int64_t left_us = s_timer_deadline_us - now_us;
            if (left_us <= 0 && !s_timer_fired) {
                s_timer_fired = true;
                s_timer_deadline_us = 0;
                lv_label_set_text(s_timer_label, LV_SYMBOL_BELL " Timer done!");
                audio_post_tone(880, 400);   // optional chime via the audio owner
                ESP_LOGI(TAG, "I02: timer fired");
            } else if (left_us > 0) {
                int secs = (int)((left_us + 999999) / 1000000);
                lv_label_set_text_fmt(s_timer_label, LV_SYMBOL_BELL " %d:%02d",
                                      secs / 60, secs % 60);
            }
        } else if (!s_timer_fired) {
            lv_label_set_text(s_timer_label, LV_SYMBOL_BELL " Timer");
        }
    }

    // --- K05: dim the backlight after inactivity (wake handled in the input cb) ---
    {
        uint32_t now_ms = (uint32_t)(now_us / 1000);
        if (!s_dimmed && (uint32_t)(now_ms - s_last_activity_ms) >= DIM_AFTER_MS) {
            ui_set_dim(true);
        }
    }

    // --- A11: idle return to the landing tile ---
    if (!s_pin_here && s_tileview && s_ov_tile) {
        uint32_t now_ms = (uint32_t)(now_us / 1000);
        if ((uint32_t)(now_ms - s_last_activity_ms) >= IDLE_RETURN_MS) {
            // Only move if we're not already on the landing tile. Protected
            // states (cleaning lock) already returned above; a pending/modal
            // check could be added here if those gain explicit flags.
            lv_obj_t *cur = lv_tileview_get_tile_act(s_tileview);
            if (cur != s_ov_tile) {
                lv_obj_set_tile(s_tileview, s_ov_tile, LV_ANIM_ON);
                ESP_LOGI(TAG, "A11: idle %us -> returned to Overview",
                         IDLE_RETURN_MS / 1000);
            }
            s_last_activity_ms = now_ms;   // avoid repeated moves every tick
        }
    }
}

// Volume up/down -- enqueued on the audio service (non-blocking).
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

// A17: recolour the Overview WiFi icon from the live RSSI (LVGL task).
static void ov_wifi_timer_cb(lv_timer_t *t) {
    (void)t;
    if (s_ov_wifi_bars[0] == NULL) return;
    wifi_ap_record_t ap;
    int rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
    int level = wifi_level_from_rssi(rssi);
    for (int i = 0; i < 4; i++) {
        bool on = (i < level);
        lv_obj_set_style_bg_color(s_ov_wifi_bars[i],
            lv_color_hex(on ? 0x90c0ff : 0x404040), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_ov_wifi_bars[i],
            on ? LV_OPA_COVER : LV_OPA_50, LV_PART_MAIN);
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
    uint64_t bat_now = (uint64_t)esp_timer_get_time();
    bool bat_fresh = have && d.bat_ok && sensors_group_fresh_at(d.bat_last_us, bat_now);
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
    // Sensor freshness + chart feed. The textual sensor readout moved to the
    // Overview (K01); the melody page no longer shows it. We still compute
    // freshness here to feed the page-2 charts on the fixed 2 s grid.
    {
        sensors_data_t s;
        if (sensors_get(&s)) {
            // R10/V05a: freshness against the READER's current clock (not the
            // producer's frozen publish-time), 64-bit µs (N10).
            uint64_t reader_now = (uint64_t)esp_timer_get_time();
            bool th_fresh  = sensors_group_fresh_at(s.th_last_us, reader_now);
            bool imu_fresh = sensors_group_fresh_at(s.imu_last_us, reader_now);
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

    // Last reset reason (user request): why did it last reboot?
    const char *rr;
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   rr = "poweron";  break;
        case ESP_RST_SW:        rr = "sw/ota";   break;
        case ESP_RST_PANIC:     rr = "panic";    break;
        case ESP_RST_TASK_WDT:  rr = "task_wdt"; break;
        case ESP_RST_INT_WDT:   rr = "int_wdt";  break;
        case ESP_RST_BROWNOUT:  rr = "brownout"; break;
        default:                rr = "other";    break;
    }
    char rst_line[24];
    snprintf(rst_line, sizeof(rst_line), "reset:  %s", rr);

    char txt[512];
    snprintf(txt, sizeof(txt),
             "build:  %s\nidf:    %s\n%s\nssid:   %s\nip:     %s\n%s\nheap:   %u KB\n%s\n%s\n%s",
             app->version, app->idf_ver, mac_line, ssid, ip, wifi_line,
             heap_kb, up_line, rst_line, ota_line);
    lv_label_set_text(s_info_label, txt);
}

static void build_test_ui(void) {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), LV_PART_MAIN);

    lv_obj_t *tv = lv_tileview_create(scr);
    lv_obj_set_style_bg_color(tv, lv_color_hex(0x101418), LV_PART_MAIN);
    // Page order (left -> right). K01: the OVERVIEW is now column 0 (the default
    // landing the tileview shows first) — a glanceable home, not device info.
    // Device info moves to the far right (support/health detail).
    lv_obj_t *tov = lv_tileview_add_tile(tv, 0, 0, LV_DIR_RIGHT);         // K01 overview (landing)
    // Tile 1: the consolidated graph page (dropdown picks which metric).
    lv_obj_t *t1 = lv_tileview_add_tile(tv, 1, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    // Tile 4: Home Assistant temperatures (list of configured sensors).
    lv_obj_t *t4 = lv_tileview_add_tile(tv, 4, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    // Tile 5: Home Assistant sensor batteries (same sensors as tile 4).
    lv_obj_t *tb = lv_tileview_add_tile(tv, 5, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    // Tile 6: home/controls + song list.
    lv_obj_t *t0 = lv_tileview_add_tile(tv, 6, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    // Tile 7: device info (support/health detail), now the last page.
    lv_obj_t *t5 = lv_tileview_add_tile(tv, 7, 0, LV_DIR_LEFT);
    // Tiles must not scroll internally or they swallow the swipe gesture.
    lv_obj_clear_flag(tov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(t0, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(t1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(t4, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(tb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(t5, LV_OBJ_FLAG_SCROLLABLE);
    s_tileview = tv;
    s_ov_tile = tov;

    // ---- K01 Overview tile (default landing) ----
    {
        lv_obj_t *ot = lv_label_create(tov);
        lv_label_set_text(ot, "Overview");
        lv_obj_set_style_text_color(ot, lv_color_hex(0x30d070), LV_PART_MAIN);
        lv_obj_set_style_text_font(ot, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(ot, LV_ALIGN_TOP_MID, 0, 8);

        // Wall clock (SNTP). "--:--" until the first sync — never invent time.
        s_ov_time = lv_label_create(tov);
        lv_obj_set_style_text_color(s_ov_time, lv_color_hex(0xe0e0e0), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_ov_time, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_label_set_text(s_ov_time, "--:--");
        lv_obj_align(s_ov_time, LV_ALIGN_TOP_LEFT, 6, 8);

        // A17: WiFi strength icon in the top-right corner (mirrors the page-1
        // status-bar icon). ov_wifi_timer_cb recolours the bars from RSSI.
        s_ov_wifi_box = lv_obj_create(tov);
        lv_obj_remove_style_all(s_ov_wifi_box);
        lv_obj_set_size(s_ov_wifi_box, 26, 14);
        lv_obj_clear_flag(s_ov_wifi_box, LV_OBJ_FLAG_SCROLLABLE);
        for (int i = 0; i < 4; i++) {
            lv_obj_t *bar = lv_obj_create(s_ov_wifi_box);
            lv_obj_remove_style_all(bar);
            int h = 5 + i * 3;
            lv_obj_set_size(bar, 5, h);
            lv_obj_set_style_radius(bar, 1, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_bg_color(bar, lv_color_hex(0x404040), LV_PART_MAIN);
            lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
            lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, i * 7, 0);
            s_ov_wifi_bars[i] = bar;
        }
        lv_obj_align(s_ov_wifi_box, LV_ALIGN_TOP_RIGHT, -6, 8);
        lv_timer_create(ov_wifi_timer_cb, 3000, NULL);
        ov_wifi_timer_cb(NULL);

        // Connectivity line: WiFi band + a DISTINCT HA-reachable indicator
        // (signal strength != HA usable — review's explicit point).
        s_ov_net = lv_label_create(tov);
        lv_obj_set_style_text_color(s_ov_net, lv_color_hex(0x90c0ff), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_ov_net, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_label_set_text(s_ov_net, "net --");
        lv_obj_align(s_ov_net, LV_ALIGN_TOP_MID, 0, 34);

        // Local climate (SHTC3) with explicit freshness/quality.
        s_ov_climate = lv_label_create(tov);
        lv_obj_set_style_text_color(s_ov_climate, lv_color_hex(0xe0e0e0), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_ov_climate, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_label_set_text(s_ov_climate, "Climate\n--");
        lv_obj_set_style_text_align(s_ov_climate, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(s_ov_climate, LV_ALIGN_TOP_MID, 0, 50);

        // Outdoor temperature (A17): Ute-Aquara (first catalogue temperature
        // entry). Replaces the old generic "first device temperature" line.
        s_ov_outdoor = lv_label_create(tov);
        lv_obj_set_style_text_color(s_ov_outdoor, lv_color_hex(0xffd060), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_ov_outdoor, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_label_set_text(s_ov_outdoor, "Outdoor --");
        lv_obj_set_style_text_align(s_ov_outdoor, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(s_ov_outdoor, LV_ALIGN_TOP_MID, 0, 106);

        // Electricity section heading (A17); the power line sits below it.
        s_ov_elec_hdr = lv_label_create(tov);
        lv_obj_set_style_text_color(s_ov_elec_hdr, lv_color_hex(0x808080), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_ov_elec_hdr, &lv_font_montserrat_12, LV_PART_MAIN);
        lv_label_set_text(s_ov_elec_hdr, "Electricity");
        lv_obj_set_style_text_align(s_ov_elec_hdr, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(s_ov_elec_hdr, LV_ALIGN_TOP_MID, 0, 134);

        // Battery at the bottom (A17).
        s_ov_batt = lv_label_create(tov);
        lv_obj_set_style_text_color(s_ov_batt, lv_color_hex(0x90c0ff), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_ov_batt, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_label_set_text(s_ov_batt, LV_SYMBOL_BATTERY_EMPTY " --");
        lv_obj_align(s_ov_batt, LV_ALIGN_BOTTOM_MID, 0, -66);

        // A16: solar/house power line, tucked between the climate and HA
        // lines. Montserrat 12 so "Sol 0W Hus 443W Idag 0.49kWh" fits.
        s_ov_power = lv_label_create(tov);
        lv_obj_set_style_text_color(s_ov_power, lv_color_hex(0xffd060), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_ov_power, &lv_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_style_text_align(s_ov_power, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_label_set_text(s_ov_power, "--");
        lv_obj_align(s_ov_power, LV_ALIGN_TOP_MID, 0, 154);

        // A05: sustained-comfort advisory (hidden until a rule is ACTIVE).
        s_ov_advisory = lv_label_create(tov);
        lv_obj_set_style_text_color(s_ov_advisory, lv_color_hex(0xffc040), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_ov_advisory, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_label_set_text(s_ov_advisory, "");
        lv_obj_add_flag(s_ov_advisory, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(s_ov_advisory, LV_ALIGN_BOTTOM_MID, 0, -92);

        // I02: offline-timer status line (above the buttons).
        s_timer_label = lv_label_create(tov);
        lv_obj_set_style_text_color(s_timer_label, lv_color_hex(0xc0d0e0), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_timer_label, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_label_set_text(s_timer_label, LV_SYMBOL_BELL " Timer");
        lv_obj_align(s_timer_label, LV_ALIGN_BOTTOM_MID, 0, -42);

        // Bottom row: Clean screen (I01) + Timer start/cancel (I02), side by side.
        lv_obj_t *clean = lv_btn_create(tov);
        lv_obj_set_size(clean, 112, 34);
        lv_obj_align(clean, LV_ALIGN_BOTTOM_LEFT, 4, -2);
        lv_obj_t *cl = lv_label_create(clean);
        lv_label_set_text(cl, LV_SYMBOL_SETTINGS " Clean");
        lv_obj_center(cl);
        lv_obj_add_event_cb(clean, clean_btn_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *tmr = lv_btn_create(tov);
        lv_obj_set_size(tmr, 112, 34);
        lv_obj_align(tmr, LV_ALIGN_BOTTOM_RIGHT, -4, -2);
        lv_obj_t *tl = lv_label_create(tmr);
        lv_label_set_text(tl, LV_SYMBOL_BELL " 5 min");
        lv_obj_center(tl);
        lv_obj_add_event_cb(tmr, timer_btn_cb, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_t *title = lv_label_create(t0);
    lv_label_set_text(title, "Melodies");
    lv_obj_set_style_text_color(title, lv_color_hex(0x30d070), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // Item 4: battery + WiFi status icons now live on the Device-info page
    // (t5), not the melody page. (A later step may add them to every page.)
    s_bat_label = lv_label_create(t5);
    lv_obj_set_style_text_color(s_bat_label, lv_color_hex(0x90c0ff), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_bat_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(s_bat_label, LV_SYMBOL_BATTERY_EMPTY " --");
    lv_obj_align(s_bat_label, LV_ALIGN_TOP_RIGHT, -4, 2);

    // Phone-style WiFi signal-strength icon, left of the battery readout.
    // wifi_timer_cb recolours the bars from the live RSSI; bat_timer_cb anchors
    // the box to the battery label's left edge each refresh.
    s_wifi_box = lv_obj_create(t5);
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

    // (The sensor readout that used to live here moved to the Overview page.)

    // Scrollable song list -- one button per registered (public-domain)
    // song. Each button carries its song index as user-data; the callback
    // spawns the worker task to play it.
    lv_obj_t *list = lv_obj_create(t0);
    lv_obj_set_size(list, 220, 150);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x181c22), LV_PART_MAIN);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 4, LV_PART_MAIN);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(list, LV_OBJ_FLAG_GESTURE_BUBBLE);
    // Fixed rows (viewport-bounded). Each row's user-data = ROW index; the
    // stable song index is page*SONG_ROWS + row (see song_btn_cb).
    for (int r = 0; r < SONG_ROWS; r++) {
        lv_obj_t *b = lv_btn_create(list);
        lv_obj_set_width(b, LV_PCT(100));
        lv_obj_set_height(b, 26);
        lv_obj_add_flag(b, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(b, song_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)r);
        lv_obj_t *l = lv_label_create(b);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_label_set_text(l, "--");
        lv_obj_center(l);
        s_song_rows[r] = b;
        s_song_row_lbls[r] = l;
    }
    // Prev / page / Next row.
    lv_obj_t *prev = lv_btn_create(t0);
    lv_obj_set_size(prev, 50, 30);
    lv_obj_align(prev, LV_ALIGN_TOP_MID, -55, 196);
    lv_obj_add_event_cb(prev, song_prev_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pl = lv_label_create(prev); lv_label_set_text(pl, LV_SYMBOL_LEFT); lv_obj_center(pl);
    s_song_page_lbl = lv_label_create(t0);
    lv_obj_set_style_text_color(s_song_page_lbl, lv_color_hex(0xc0c0c0), LV_PART_MAIN);
    lv_obj_align(s_song_page_lbl, LV_ALIGN_TOP_MID, 0, 203);
    lv_obj_t *next = lv_btn_create(t0);
    lv_obj_set_size(next, 50, 30);
    lv_obj_align(next, LV_ALIGN_TOP_MID, 55, 196);
    lv_obj_add_event_cb(next, song_next_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nl = lv_label_create(next); lv_label_set_text(nl, LV_SYMBOL_RIGHT); lv_obj_center(nl);
    song_render_page();   // fill the first page

    // Volume controls below the list.
    lv_obj_t *vol_dn = lv_btn_create(t0);
    lv_obj_set_size(vol_dn, 50, 34);
    lv_obj_align(vol_dn, LV_ALIGN_TOP_MID, -70, 234);
    lv_obj_add_event_cb(vol_dn, vol_down_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *vd = lv_label_create(vol_dn);
    lv_label_set_text(vd, LV_SYMBOL_DOWN);
    lv_obj_center(vd);

    s_vol_label = lv_label_create(t0);
    lv_obj_set_style_text_color(s_vol_label, lv_color_hex(0xc0c0c0), LV_PART_MAIN);
    lv_obj_align(s_vol_label, LV_ALIGN_TOP_MID, 0, 242);
    update_vol_label();

    lv_obj_t *vol_up = lv_btn_create(t0);
    lv_obj_set_size(vol_up, 50, 34);
    lv_obj_align(vol_up, LV_ALIGN_TOP_MID, 70, 234);
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

    // ---- I01 cleaning-lock overlay (hidden until invoked) ----
    s_clean_overlay = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_clean_overlay);
    lv_obj_set_size(s_clean_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_clean_overlay, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_clean_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(s_clean_overlay, LV_OBJ_FLAG_HIDDEN);
    s_clean_label = lv_label_create(s_clean_overlay);
    lv_obj_set_style_text_color(s_clean_label, lv_color_hex(0x30d070), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_clean_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_clean_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(s_clean_label, "Cleaning\nTouch locked");
    lv_obj_center(s_clean_label);

    // A11: observe ALL input on the screen to reset the idle timer.
    lv_obj_add_event_cb(lv_scr_act(), ui_screen_event_cb, LV_EVENT_ALL, NULL);
    s_last_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);

    // One timer drives the Overview refresh, the cleaning-lock countdown, and
    // the idle-return check (1 s cadence — cheap, no new polling).
    lv_timer_create(overview_timer_cb, 1000, NULL);
    overview_timer_cb(NULL);
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
