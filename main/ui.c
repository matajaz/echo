// Hardware attachment only. Pages/navigation live under main/ui/.
#include "ui.h"
#include "ui/ui_shell.h"
#include "display.h"
#include "touch.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
static bool s_initialized;
esp_err_t ui_init(void) {
    if (s_initialized) return ESP_ERR_INVALID_STATE;
    esp_lcd_panel_handle_t panel=display_get_panel();
    esp_lcd_panel_io_handle_t io=display_get_panel_io();
    if (!panel || !io) return ESP_ERR_INVALID_STATE;
    lvgl_port_cfg_t config=ESP_LVGL_PORT_INIT_CONFIG(); config.task_stack=8192;
    esp_err_t err=lvgl_port_init(&config); if (err!=ESP_OK) return err;
    lvgl_port_display_cfg_t display={.io_handle=io,.panel_handle=panel,.buffer_size=240*320/20,
        .double_buffer=false,.hres=240,.vres=320,.monochrome=false,
        .rotation={.swap_xy=false,.mirror_x=false,.mirror_y=false},
        .flags={.buff_dma=true,.buff_spiram=false}};
    lv_disp_t *disp=lvgl_port_add_disp(&display);
    if (!disp) { lvgl_port_deinit(); return ESP_FAIL; }
    lv_indev_t *input=NULL;
    if (touch_get_handle()) {
        lvgl_port_touch_cfg_t cfg={.disp=disp,.handle=touch_get_handle()}; input=lvgl_port_add_touch(&cfg);
    }
    if (!lvgl_port_lock(2000)) { if (input) lvgl_port_remove_touch(input); lvgl_port_remove_disp(disp); lvgl_port_deinit(); return ESP_ERR_TIMEOUT; }
    bool ready=ui_shell_init(input); lvgl_port_unlock();
    if (!ready) {
        if (input) lvgl_port_remove_touch(input);
        lvgl_port_remove_disp(disp); lvgl_port_deinit(); return ESP_ERR_NO_MEM;
    }
    s_initialized=true;
    ESP_LOGI("ui","UI ready: 9 pages, touch=%d",input!=NULL); return ESP_OK;
}
void ui_set_status(const char *text) {
    if (s_initialized && lvgl_port_lock(100)) { ui_shell_status(text); lvgl_port_unlock(); }
}
