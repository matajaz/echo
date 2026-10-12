#include "../wav_stream.h"
#include "../chart_history.h"
#include "../input_gate.h"
#include "../ha_parse.h"
#include "../time_utils.h"
#include "cJSON.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static int checks;
#define CHECK(x) do { assert(x); ++checks; } while(0)
static char *file(const char *name) {
    FILE *f=fopen(name,"rb"); assert(f); fseek(f,0,SEEK_END); long n=ftell(f); rewind(f);
    char *s=malloc(n+1); assert(s); assert(fread(s,1,n,f)==(size_t)n); s[n]=0; fclose(f); return s;
}
static void u32(uint8_t *p,uint32_t n) { for(int i=0;i<4;++i) p[i]=(uint8_t)(n>>(i*8)); }
static void wav(void) {
    uint8_t h[64]={0}; memcpy(h,"RIFF",4); u32(h+4,40); memcpy(h+8,"WAVEfmt ",8);
    u32(h+16,16); h[20]=1; h[22]=1; u32(h+24,16000); u32(h+28,32000); h[32]=2; h[34]=16;
    memcpy(h+36,"data",4); u32(h+40,4);
    wav_info_t info;
    for(size_t n=0;n<44;++n) CHECK(wav_header(h,n,&info)==WAV_MORE);
    CHECK(wav_header(h,44,&info)==WAV_READY && info.rate==16000 && info.bytes==4 && info.offset==44);
    h[34]=24; CHECK(wav_header(h,44,&info)==WAV_INVALID); h[34]=16;
    h[22]=2; CHECK(wav_header(h,44,&info)==WAV_INVALID); h[22]=1;
    u32(h+16,15); CHECK(wav_header(h,44,&info)==WAV_INVALID); u32(h+16,16);
    memcpy(h+12,"JUNK",4); u32(h+16,0xffffffe8);
    CHECK(wav_header(h,sizeof(h),&info)==WAV_INVALID); // former ASan underflow
    wav_fragment_t fragment={0}; uint8_t pcm[]={1,2,3,4,5,6}; int16_t out[3]={0};
    CHECK(wav_samples(&fragment,pcm,1,out)==0 && fragment.have_byte);
    CHECK(wav_samples(&fragment,pcm+1,3,out)==2 && (uint16_t)out[0]==0x0201 && (uint16_t)out[1]==0x0403);
    CHECK(wav_samples(&fragment,pcm+4,1,out)==0 && fragment.have_byte);
    CHECK(wav_samples(&fragment,pcm+5,1,out)==1 && (uint16_t)out[0]==0x0605 && !fragment.have_byte);
}
static void charts_and_input(void) {
    chart_history_t h={0}; chart_history_add(&h,0,40);
    for(unsigned i=1;i<=60;++i) chart_history_add(&h,i,20);
    chart_stats_t s=chart_history_stats(&h,60);
    CHECK(s.valid && s.max==20 && s.min==20 && s.average==20 && s.count==60);
    s=chart_history_stats(&h,120); CHECK(!s.valid); // aged out, not forever-current
    chart_history_add(&h,121,12); chart_history_add(&h,121,18);
    s=chart_history_stats(&h,121); CHECK(s.min==12 && s.max==18 && s.average==15);
    input_gate_t gate={0};
    CHECK(input_gate_filter(&gate,true,true)); // dimmed initial press
    CHECK(input_gate_filter(&gate,true,false)); // wake, still held
    CHECK(input_gate_filter(&gate,false,false)); // release remains swallowed
    CHECK(!input_gate_filter(&gate,true,false)); // next press operates normally
    CHECK(input_gate_filter(&gate,true,true)); // cleaning begins mid-stroke
    CHECK(input_gate_filter(&gate,true,false)); // cleaning ends, held finger
    CHECK(input_gate_filter(&gate,false,false));
}
static void ha(void) {
    float v=0;
    CHECK(ha_parse_state("{\"attributes\":{\"state\":\"999\",\"unit_of_measurement\":\"°F\"},\"state\":\"68\"}","C",&v,NULL)==HA_VALID && fabsf(v-20)<.001f);
    CHECK(ha_parse_state("{\"state\":\"unknown\",\"attributes\":{\"unit_of_measurement\":\"W\"}}","W",&v,NULL)==HA_INVALID);
    CHECK(ha_parse_state("{\"state\":\"1\",\"attributes\":{\"unit_of_measurement\":\"kW\"}}","W",&v,NULL)==HA_VALID && v==1000);
    CHECK(ha_parse_state("{\"state\":\"1000\",\"attributes\":{\"unit_of_measurement\":\"Wh\"}}","kWh",&v,NULL)==HA_VALID && v==1);
    CHECK(ha_parse_state("{\"state\":\"1\",\"attributes\":{\"unit_of_measurement\":\"EUR/kWh\"}}","SEK/kWh",&v,NULL)==HA_UNIT_ERROR);
    CHECK(ha_parse_state("{\"state\":\"NaN\"}","C",&v,NULL)==HA_INVALID);
    char *json=file("fixtures/nordpool.json"); CHECK(strlen(json)>4096);
    energy_prices_t prices;
    CHECK(ha_parse_state(json,"SEK/kWh",&v,&prices)==HA_VALID && isfinite(v));
    CHECK(prices.today.valid && prices.today.count>24 && prices.today.count<=100);
    free(json);
    ha_sensor_t sample={.ok=true,.received_us=1000000,.stale_ms=5000};
    CHECK(ha_fresh(&sample,6000000)); CHECK(!ha_fresh(&sample,6000001)); CHECK(!ha_fresh(&sample,999999));
    json=file("fixtures/energy_statistics.json"); cJSON *root=cJSON_Parse(json); assert(root);
    energy_history_t history={.day_start=(int64_t)cJSON_GetObjectItem(root,"day_start")->valuedouble,
        .day_end=(int64_t)cJSON_GetObjectItem(root,"day_end")->valuedouble};
    history.count=(history.day_end-history.day_start)/3600; cJSON_Delete(root);
    CHECK(ha_parse_statistics(json,"sensor.import","sensor.export",&history));
    for(int i=0;i<history.count;++i) CHECK(history.hours[i].import_ok && history.hours[i].export_ok && history.hours[i].imported>=0);
    free(json);
    history=(energy_history_t){.day_start=1700000000,.day_end=1700007200,.count=2};
    CHECK(!ha_parse_statistics("{\"success\":false}","sensor.import","sensor.export",&history));
    CHECK(ha_parse_statistics("{\"success\":true,\"result\":{\"sensor.import\":[{\"start\":1700000000000,\"change\":1.2},{\"start\":1700003600000,\"change\":null}]}}","sensor.import","sensor.export",&history));
    CHECK(history.hours[0].import_ok && !history.hours[1].import_ok); // gaps remain gaps
}
static void price_and_time(void) {
    int64_t a,b; CHECK(iso_epoch("2026-10-25T02:00:00+02:00",&a)); CHECK(iso_epoch("2026-10-25T02:00:00+01:00",&b));
    CHECK(b-a==3600); CHECK(!iso_epoch("2026-02-30T00:00:00Z",&a)); CHECK(!iso_epoch("2026-10-25T02:00:00",&a));
    price_day_t day={.count=3,.valid=true,.points={{0,1800,10},{1800,5400,-2},{5400,9000,20}}};
    float avg; int64_t start;
    CHECK(price_cheapest(&day,3600,&start,&avg) && start==1800 && avg==-2);
    CHECK(price_cheapest(&day,5400,&start,&avg) && start==0 && fabsf(avg-2)<.001);
    day.points[1].start=1801; CHECK(!price_cheapest(&day,7200,&start,&avg));
    day=(price_day_t){.count=3,.valid=true,.points={{0,6000,5},{6000,9000,1},{9000,12000,10}}};
    CHECK(price_cheapest(&day,4000,&start,&avg) && start==5000 && fabsf(avg-2)<.001);
    CHECK(price_cheapest_after(&day,4000,5500,&start,&avg) && start>=5500);
    CHECK(!price_cheapest_after(&day,4000,8500,&start,&avg));
}
int main(void) { wav(); charts_and_input(); ha(); price_and_time(); printf("Review regressions: %d checks passed\n",checks); }
