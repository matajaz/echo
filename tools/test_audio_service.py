#!/usr/bin/env python3
"""Execute actual admission/pump functions with a preempting queue mock."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'main/audio.c').read_text()
def extract(signature):
    start = source.index(signature)
    pos = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[start:pos]
code = r'''
#include <assert.h>
#include <stdio.h>
#include "audio_cmd.h"
static audio_status_t s_status;
static unsigned s_stop_generation,s_cmd_next_id=1,consumer_generation;
static bool s_service_ready=true,suspended,full,consume_on_resume;
static void *s_cmd_q=(void *)1;
static audio_cmd_t sent,s_pending,queue[6];
static bool s_pending_valid;
static int queue_count,s_volume=70;
#define pdTRUE 1
#define AUDIO_CMD_QUEUE_LEN 6
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
static void run_consumer(void) {
    audio_on_start(&s_status,true,sent.arg); audio_on_playing(&s_status);
    consumer_generation=s_stop_generation;
}
static void vTaskSuspendAll(void) { assert(!suspended); suspended=true; }
static void xTaskResumeAll(void) { suspended=false; if(consume_on_resume) run_consumer(); }
static int xQueueSend(void *q,const audio_cmd_t *c,int timeout) {
    (void)q; (void)timeout;
    if(full) return 0; sent=*c;
    if(!suspended && consume_on_resume) run_consumer();
    return pdTRUE;
}
static int xQueueReceive(void *q,audio_cmd_t *c,int timeout) {
    (void)q; (void)timeout; if(!queue_count) return 0;
    *c=queue[0]; for(int i=1;i<queue_count;++i) queue[i-1]=queue[i]; --queue_count; return pdTRUE;
}
static int audio_adjust_volume(int delta) { s_volume+=delta; return s_volume; }
static void audio_set_muted(bool mute) { (void)mute; }
'''
code += '\n' + extract('void audio_request_stop(void)')
code += '\n' + extract('static void audio_apply_control(')
code += '\n' + extract('static void audio_pump_controls(')
code += '\n' + extract('static bool audio_post_command(')
code += r'''
int main(void) {
    audio_status_init(&s_status,70); consume_on_resume=true;
    assert(audio_post_command((audio_cmd_t){.type=AUDIO_CMD_SONG,.arg=0}));
    assert(consumer_generation==s_stop_generation); // idle -> Play cannot cancel itself
    unsigned generation=s_stop_generation;
    assert(audio_post_command((audio_cmd_t){.type=AUDIO_CMD_SONG,.arg=1}));
    assert(s_stop_generation==generation+1 && consumer_generation==s_stop_generation);
    generation=s_stop_generation; unsigned last=s_status.last_cmd_id;
    full=true; assert(!audio_post_command((audio_cmd_t){.type=AUDIO_CMD_SONG,.arg=2}));
    assert(s_stop_generation==generation && s_status.last_cmd_id==last);
    queue_count=2; queue[0]=(audio_cmd_t){.type=AUDIO_CMD_SONG,.arg=3};
    queue[1]=(audio_cmd_t){.type=AUDIO_CMD_WAV,.filename="second.wav"};
    audio_pump_controls(); assert(s_pending_valid && s_pending.type==AUDIO_CMD_WAV);
    assert(!strcmp(s_pending.filename,"second.wav")); // filename owned by retained command
    queue_count=1; queue[0]=(audio_cmd_t){.type=AUDIO_CMD_STOP}; audio_pump_controls();
    assert(!s_pending_valid); // later Stop removes a pending replacement
    queue_count=2; queue[0]=(audio_cmd_t){.type=AUDIO_CMD_VOLUME_ABSOLUTE,.arg=40};
    queue[1]=(audio_cmd_t){.type=AUDIO_CMD_VOLUME_ABSOLUTE,.arg=60}; audio_pump_controls();
    assert(s_volume==60);
    puts("Audio scheduler/admission/pending/volume regressions passed");
}
'''
code = '#include <string.h>\n' + code
with tempfile.TemporaryDirectory(prefix='echo-audio-test-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(ROOT/'core'),str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
