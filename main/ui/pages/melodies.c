#include "ui_page.h"
#include "ui_widgets.h"
#include <stdlib.h>
#include <stdio.h>
#define SONG_ROWS 4
typedef struct {
    int selected,base; bool busy,ready;
    lv_obj_t *root,*play,*mute,*volume,*status,*songs[SONG_ROWS];
    bool slider_active;
} melodies_t;
static void render(melodies_t *s) {
    for (int i=0;i<SONG_ROWS;++i) {
        int index=s->base+i;
        const audio_song_t *song=audio_get_song(index);
        if (!song) { lv_obj_add_flag(s->songs[i],LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_clear_flag(s->songs[i],LV_OBJ_FLAG_HIDDEN);
        ui_text(lv_obj_get_child(s->songs[i],0),song->name);
        ui_button_accent(s->songs[i],index==s->selected?ECHO_ACTION:ECHO_BUTTON);
    }
}
static void play(lv_event_t *e) {
    melodies_t *s=lv_event_get_user_data(e);
    bool accepted=s->busy?audio_post_stop():audio_post_song(s->selected);
    if (!accepted) ui_feedback(s->ready?"Audio queue full":"Audio unavailable");
}
static void choose(lv_event_t *e) {
    melodies_t *s=lv_event_get_user_data(e);
    for (int i=0;i<SONG_ROWS;++i) if (lv_event_get_target(e)==s->songs[i]) s->selected=s->base+i;
    render(s); // selection is independent of starting playback
}
static void browse(lv_event_t *e) {
    melodies_t *s=lv_event_get_user_data(e);
    int direction=lv_obj_get_index(lv_event_get_target(e))==0?-1:1;
    s->base+=direction*SONG_ROWS;
    int max=audio_song_count()-SONG_ROWS; if (max<0) max=0;
    if (s->base<0) s->base=0;
    if (s->base>max) s->base=max;
    render(s);
}
static void slider(lv_event_t *e) {
    melodies_t *s=lv_event_get_user_data(e); lv_event_code_t code=lv_event_get_code(e);
    if (code==LV_EVENT_PRESSED) s->slider_active=true;
    if (code==LV_EVENT_RELEASED) s->slider_active=false;
    if (code==LV_EVENT_VALUE_CHANGED && !audio_post_volume_absolute(lv_slider_get_value(s->volume))) ui_feedback("Volume not applied");
}
static void mute(lv_event_t *e) {
    melodies_t *s=lv_event_get_user_data(e); audio_status_t a; audio_get_status(&a);
    if (!audio_post_mute(!a.muted)) ui_feedback("Mute not applied");
    (void)s;
}
void *ui_melodies_create(lv_obj_t *root) {
    melodies_t *s=ui_state_alloc(sizeof(*s)); if (!s) return NULL; s->root=root;
    lv_obj_t *row=ui_row(root,LV_PCT(100),40,8);
    s->play=ui_button(row,LV_SYMBOL_PLAY " Play",108,40,play,s);
    ui_button_accent(s->play,ECHO_ACTION);
    s->mute=ui_button(row,LV_SYMBOL_VOLUME_MAX " Mute",108,40,mute,s);
    s->volume=lv_slider_create(root); lv_obj_set_size(s->volume,210,18); lv_slider_set_range(s->volume,0,100);
    lv_obj_set_style_bg_color(s->volume,lv_color_hex(ECHO_CARD),0);
    lv_obj_set_style_bg_color(s->volume,lv_color_hex(ECHO_COOL),LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s->volume,lv_color_hex(ECHO_TEXT),LV_PART_KNOB);
    lv_obj_add_event_cb(s->volume,slider,LV_EVENT_ALL,s);
    for (int i=0;i<SONG_ROWS;++i) s->songs[i]=ui_button(root,"Loading",224,28,choose,s);
    row=ui_row(root,LV_PCT(100),28,8);
    ui_button(row,"Previous songs",108,28,browse,s); ui_button(row,"More songs",108,28,browse,s);
    s->status=ui_label(root,"Audio starting",ECHO_FONT_SMALL,ECHO_TEXT_DIM);
    lv_obj_set_width(s->status,224); lv_label_set_long_mode(s->status,LV_LABEL_LONG_DOT);
    render(s); return s;
}
void ui_melodies_update(void *state,const ui_model_t *m) {
    melodies_t *s=state; s->busy=audio_is_active(&m->audio); s->ready=m->audio.ready;
    ui_text(lv_obj_get_child(s->play,0),s->busy?LV_SYMBOL_STOP " Stop":LV_SYMBOL_PLAY " Play");
    lv_obj_t *controls[]={s->play,s->mute,s->volume};
    for (int i=0;i<3;++i) { if (s->ready) lv_obj_clear_state(controls[i],LV_STATE_DISABLED); else lv_obj_add_state(controls[i],LV_STATE_DISABLED); }
    if (!s->slider_active) lv_slider_set_value(s->volume,m->audio.volume,LV_ANIM_OFF);
    ui_text(lv_obj_get_child(s->mute,0),m->audio.muted?"Unmute":"Mute");
    const audio_song_t *song=audio_get_song(s->busy?m->audio.playing_index:s->selected);
    char text[96];
    if (!s->ready) snprintf(text,sizeof(text),"Audio unavailable");
    else if (m->audio.state==AUDIO_FAILED) snprintf(text,sizeof(text),"Playback error: %ld",(long)m->audio.last_error);
    else snprintf(text,sizeof(text),"%d%% %s%s",(int)m->audio.volume,
        m->audio.muted?"Muted / ":s->busy?"Playing / ":"Selected / ",song?song->name:"Audio");
    ui_text(s->status,text);
    lv_obj_set_style_text_color(s->status,lv_color_hex(!s->ready?ECHO_TEXT_DIM:
        m->audio.state==AUDIO_FAILED?ECHO_RED:s->busy?ECHO_GREEN:ECHO_COOL),0);
}
