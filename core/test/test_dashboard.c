#include "dashboard_model.h"
#include "cost_model.h"
#include "ha_parse.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static dashboard_section_t sections[DASH_COUNT];
static void dashboards(void) {
 bool waxing=false;assert(dashboard_moon_percent(0,&waxing)==-1);int moon=dashboard_moon_percent(1790259661,&waxing);assert(moon>=0&&moon<=100);
 FILE *f=fopen("fixtures/dashboard_extra.json","rb");assert(f);assert(!fseek(f,0,SEEK_END));long n=ftell(f);rewind(f);char *body=calloc(n+1,1);assert(body);assert(fread(body,1,n,f)==(size_t)n);fclose(f);
 assert(dashboard_parse(body,0,1000000,sections));free(body);
 assert(sections[DASH_WEATHER].valid&&strstr(sections[DASH_WEATHER].text,"16.00"));
 assert(sections[DASH_AIR].valid&&strstr(sections[DASH_AIR].text,"PM2.5: 2.00"));
 assert(!sections[DASH_NETWORK].valid); // explicit collector _stale
 assert(sections[DASH_NEWS].valid&&strlen(sections[DASH_NEWS].text)<DASH_TEXT_CAP);
 assert(dashboard_fresh(&sections[DASH_AIR],2000000,1790258500));
 assert(!dashboard_fresh(&sections[DASH_AIR],2000000,1790261000)); // source time, not refetch time
 assert(!dashboard_fresh(&sections[DASH_AIR],1000000000,1790258500));
 assert(!dashboard_parse("{}",0,3000000,sections));assert(sections[DASH_AIR].received_us==1000000);
 assert(!dashboard_parse("{",0,3000000,sections));
 const char *panel="{\"attributes\":{\"values\":{\"version\":1,\"hums\":[\"0\",\"unknown\",\"70\"],\"lamps\":[\"on\",\"off\",\"unavailable\"]}}}";
 assert(dashboard_parse(panel,1,3000000,sections));assert(strstr(sections[DASH_HUMIDITY].text,"0.0 %"));assert(strstr(sections[DASH_HUMIDITY].text,"Framsida: unavailable"));assert(strstr(sections[DASH_LIGHTS].text,"Kok: on"));assert(sections[DASH_LIGHTS].available_lights==3);
 assert(dashboard_parse("{\"attributes\":{\"values\":\"{\\\"version\\\":1,\\\"el\\\":{\\\"now\\\":-10}}\"}}",2,4000000,sections));assert(strstr(sections[DASH_ELECTRICITY].text,"-10.00"));
 assert(!dashboard_parse("{\"attributes\":{\"values\":{\"version\":2}}}",1,5000000,sections));
}
static void costs(void) {
 price_day_t day={.valid=true,.count=4,.points={{0,900,-2},{900,1800,0},{1800,2700,2},{2700,3600,4}}};float price;
 assert(cost_interval_price(&day,0,3600,&price)&&fabsf(price-1)<.0001);
 energy_hour_t hour={.start=0,.imported=2,.import_ok=true};assert(cost_hour(&hour,&day,&price)==COST_ESTIMATED&&price==2);
 hour.import_cost_ok=true;hour.import_cost=-3;assert(cost_hour(&hour,&day,&price)==COST_RECORDED&&price==-3);
 hour.import_cost=0;assert(cost_hour(&hour,&day,&price)==COST_RECORDED&&price==0);
 hour.import_cost_ok=false;day.points[2].start++;assert(cost_hour(&hour,&day,&price)==COST_MISSING);
 day.points[2].start--;day.points[1].value=NAN;assert(!cost_interval_price(&day,0,3600,&price));
 energy_history_t history={.day_start=1700000000,.day_end=1700007200,.count=2};
 assert(ha_parse_cost_statistics("{\"success\":true,\"result\":{\"cost\":[{\"start\":1700000000000,\"change\":-1},{\"start\":1700003600000,\"change\":0}],\"income\":[{\"start\":1700000000000,\"change\":2}]}}","cost","income",&history));
 assert(history.hours[0].import_cost_ok&&history.hours[0].import_cost==-1);assert(history.hours[1].import_cost_ok&&history.hours[1].import_cost==0);assert(history.hours[0].export_income_ok&&!history.hours[1].export_income_ok);
}
int main(void){dashboards();costs();puts("Dashboard freshness, bounded content, bundle formats, signed monetary statistics and quarter-hour costs passed");}
