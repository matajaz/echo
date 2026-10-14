#include "dashboard_model.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
const char *const dashboard_names[DASH_COUNT]={"Right now","Weather","Weather today","Sun & Moon","Network","World / Markets","Air quality","Network clients","News","Police events","SL Transit","Lights","Humidity","Temperatures","Batteries","Electricity","WAN health"};
int dashboard_moon_percent(int64_t epoch,bool *waxing) {
 if(epoch<1700000000 || !waxing)return -1;
 const double month=29.530588853*86400.0;
 double age=fmod((double)epoch-947182440.0,month);if(age<0)age+=month;
 double fraction=age/month;*waxing=fraction<0.5;
 return (int)((1.0-cos(2.0*3.14159265*fraction))*50.0+0.5);
}
static const cJSON *field(const cJSON *o,const char *key) { return cJSON_GetObjectItemCaseSensitive(o,key); }
static const char *str(const cJSON *o,const char *key) { const cJSON *v=field(o,key); return cJSON_IsString(v)?v->valuestring:""; }
static void line(dashboard_section_t *s,const char *fmt,...) {
 size_t n=strlen(s->text); if(n>=sizeof(s->text)-1)return;
 va_list a;va_start(a,fmt);vsnprintf(s->text+n,sizeof(s->text)-n,fmt,a);va_end(a);
}
static bool numeric(const cJSON *v,double *out) {
 if(cJSON_IsNumber(v))*out=v->valuedouble;
 else if(cJSON_IsString(v)){char *end;*out=strtod(v->valuestring,&end);if(end==v->valuestring || *end)return false;}
 else return false;
 return isfinite(*out);
}
static void number(dashboard_section_t *s,const cJSON *o,const char *key,const char *label,const char *unit) {
 double v;if(numeric(field(o,key),&v))line(s,"%s: %.2f %s\n",label,v,unit);
}
static uint32_t epoch(const cJSON *o,const char *key) {
 double n;return numeric(field(o,key),&n)&&n>0&&n<=UINT32_MAX&&floor(n)==n?(uint32_t)n:0;
}
static dashboard_section_t *section(dashboard_section_t *out,int id,const cJSON *o,uint32_t fallback,uint64_t now) {
 dashboard_section_t *s=&out[id];memset(s,0,sizeof(*s));s->received_us=now;
 s->observed_at=epoch(o,"observed_at");if(!s->observed_at)s->observed_at=epoch(o,"fetched_at");if(!s->observed_at)s->observed_at=fallback;
 const char *status=str(o,"status");s->valid=cJSON_IsObject(o)&&(!*status||!strcmp(status,"ok")||!strcmp(status,"empty"));
 if(cJSON_IsTrue(field(o,"_stale")))s->valid=false;
 if(!s->valid)line(s,"Data unavailable (%s)\n",*status?status:"missing section");
 return s;
}
static void items(dashboard_section_t *s,const cJSON *array,const char *a,const char *b,const char *c) {
 int n=cJSON_GetArraySize(array);if(!cJSON_IsArray(array)){s->valid=false;line(s,"Data unavailable (missing list)\n");return;}
 for(int i=0;i<n&&i<12;i++){const cJSON *o=cJSON_GetArrayItem(array,i);line(s,"%s%s%s%s%s\n",str(o,a),b?"  ":"",b?str(o,b):"",c?"  ":"",c?str(o,c):"");}
 if(n==0)line(s,"None reported\n");
 if(n>12)line(s,"+ %d more\n",n-12);
}
static void sensors(dashboard_section_t *s,const cJSON *a,const char *const *names,int count,const char *unit) {
 for(int i=0;i<count;i++){double v;const cJSON *item=cJSON_GetArrayItem(a,i);if(numeric(item,&v))line(s,"%s: %.1f %s\n",names[i],v,unit);else line(s,"%s: unavailable\n",names[i]);}
}
bool dashboard_parse(const char *json,int source,uint64_t now,dashboard_section_t out[DASH_COUNT]) {
 cJSON *root=cJSON_Parse(json);if(!cJSON_IsObject(root)){cJSON_Delete(root);return false;}
 cJSON *owned=NULL;const cJSON *v=root;
 if(source){v=field(field(root,"attributes"),"values");if(cJSON_IsString(v)){owned=cJSON_Parse(v->valuestring);v=owned;}double ver;if(!cJSON_IsObject(v)||!numeric(field(v,"version"),&ver)||ver!=1){cJSON_Delete(owned);cJSON_Delete(root);return false;}}
 if(source==0){
 uint32_t generated=epoch(root,"generated_at");if(!generated){cJSON_Delete(root);return false;}
 const cJSON *wx=field(v,"wx"),*o;dashboard_section_t *s;
 s=section(out,DASH_WEATHER,wx,generated,now);if(s->valid){line(s,"%s\n%s\n",str(wx,"place"),str(field(wx,"now"),"cond"));number(s,field(wx,"now"),"temp","Temperature","C");number(s,field(wx,"now"),"feels","Feels like","C");number(s,wx,"hi","High","C");number(s,wx,"lo","Low","C");}
 s=section(out,DASH_WEATHER_DAY,wx,generated,now);if(s->valid){line(s,"%s  %s\n",str(wx,"place"),str(wx,"date"));const cJSON *a=field(wx,"periods");for(int i=0;i<cJSON_GetArraySize(a)&&i<4;i++){o=cJSON_GetArrayItem(a,i);line(s,"\n%s: %s\n",str(o,"label"),str(o,"cond"));number(s,o,"hi","High","C");number(s,o,"lo","Low","C");number(s,o,"wind","Wind","m/s");number(s,o,"gust","Gust","m/s");number(s,o,"mm","Rain","mm");}}
 s=section(out,DASH_SUN,wx,generated,now);if(s->valid){o=field(wx,"sun");line(s,"Today's sunrise: %s\nToday's sunset: %s\n",str(o,"up"),str(o,"down"));bool waxing=false;int moon=dashboard_moon_percent(generated,&waxing);if(moon>=0)line(s,"Moon: about %d %% (%s)\n",moon,waxing?"waxing":"waning");}
 o=field(v,"net");s=section(out,DASH_NETWORK,o,generated,now);if(s->valid)line(s,"ISP: %s\nExternal IP: %s\nLocation: %s\n",str(o,"isp"),str(o,"ip"),str(o,"city"));
 o=field(v,"fx");s=section(out,DASH_MARKETS,o,generated,now);if(s->valid){number(s,o,"eursek","EUR/SEK","");number(s,o,"usdsek","USD/SEK","");number(s,o,"omx","OMX","");number(s,o,"omx_chg","OMX change","%");o=field(v,"stocks");line(s,"Best: %s\nWorst: %s\n",str(o,"best_name"),str(o,"worst_name"));number(s,o,"best_chg","Best change","%");number(s,o,"worst_chg","Worst change","%");}
 o=field(v,"aq");s=section(out,DASH_AIR,o,generated,now);if(s->valid){number(s,o,"aqi","AQI","");number(s,o,"pm25","PM2.5","ug/m3");number(s,o,"pm10","PM10","ug/m3");number(s,o,"no2","NO2","ug/m3");number(s,o,"birch","Birch pollen","");number(s,o,"grass","Grass pollen","");number(s,o,"alder","Alder pollen","");}
 o=field(v,"kuma");s=section(out,DASH_SERVICES,o,generated,now);if(s->valid){number(s,o,"up","Up","");number(s,o,"total","Total","");line(s,"\nDown services:\n");items(s,field(o,"down"),"name",NULL,NULL);line(s,"\nImportant services:\n");const cJSON *a=field(o,"imp");for(int i=0;i<cJSON_GetArraySize(a)&&i<12;i++){const cJSON *q=cJSON_GetArrayItem(a,i);const cJSON *up=field(q,"up");line(s,"%s: %s\n",str(q,"name"),cJSON_IsBool(up)?cJSON_IsTrue(up)?"up":"down":"unknown");}}
 o=field(v,"news2");s=section(out,DASH_NEWS,o,generated,now);if(s->valid)items(s,field(o,"items"),"t",NULL,NULL);
 o=field(v,"police");s=section(out,DASH_POLICE,o,generated,now);if(s->valid)items(s,field(o,"items"),"name","summary",NULL);
 s=&out[DASH_TRANSIT];memset(s,0,sizeof(*s));s->received_us=now;s->observed_at=epoch(v,"sl_observed_at");if(!s->observed_at)s->observed_at=generated;s->valid=!strcmp(str(v,"sl_status"),"ok")&&cJSON_IsArray(field(v,"sl"))&&!cJSON_IsTrue(field(v,"sl_stale"));if(s->valid)items(s,field(v,"sl"),"line","disp","dest");else line(s,"Departures unavailable\n");
 o=field(v,"warn");s=section(out,DASH_STATUS,o,generated,now);if(s->valid){line(s,"Weather warnings:\n");items(s,field(o,"warnings"),"level","event",NULL);}o=field(v,"holiday");line(s,"\nNext holiday: %s %s\n",str(o,"name"),str(o,"date"));
 }else if(source==1){
 static const char *const temps[]={"Ute-Grund","Framsida","Forrad","Oliver","Kok","Anli","Oliver-TVOC","Kallare","Vind"};
 static const char *const hums[]={"Ute-Grund","Framsida","Oliver","Kok","Anli","Oliver-TVOC","Kallare","Vind"};
 static const char *const batts[]={"Home battery","Anli","Kok","Oliver-TVOC","Oliver","VVB","Badrum","Kallare","Ute-Aquara","Dusch","Vind","Forrad","Ute-Grund"};
 const int ids[]={DASH_TEMPERATURES,DASH_HUMIDITY,DASH_BATTERIES};const char *keys[]={"temps","hums","batts"};const char *const *names[]={temps,hums,batts};const int counts[]={9,8,13};
 for(int i=0;i<3;i++){dashboard_section_t *s=section(out,ids[i],v,0,now);s->valid=cJSON_IsArray(field(v,keys[i]));if(s->valid)sensors(s,field(v,keys[i]),names[i],counts[i],i==0?"C":"%");}
 dashboard_section_t *s=section(out,DASH_LIGHTS,v,0,now);s->valid=cJSON_IsArray(field(v,"lamps"));static const char *const lamps[]={"Kok","Sovrum","Taklampa","Ute1","Hue white","Baksida","Framsida","Vind","Kallare"};for(int i=0;i<9;i++){const cJSON *q=cJSON_GetArrayItem(field(v,"lamps"),i);line(s,"%s: %s\n",lamps[i],cJSON_IsString(q)?q->valuestring:"unavailable");if(cJSON_IsString(q)&&(!strcmp(q->valuestring,"on")||!strcmp(q->valuestring,"off")))s->available_lights|=(uint16_t)(1U<<i);}s->valid=s->valid&&s->available_lights!=0;
 }else if(source==2){
 const cJSON *net=field(v,"net");dashboard_section_t *wan=section(out,DASH_WAN,net,0,now);
 if(wan->valid){number(wan,net,"latency_ms","WAN latency","ms");number(wan,net,"up","WAN up (1=yes)","");line(wan,"Uptime: %s\n",str(net,"uptime"));}

 const cJSON *o=field(v,"el");dashboard_section_t *s=section(out,DASH_ELECTRICITY,o,0,now);if(s->valid){line(s,"%s\n",str(o,"date"));number(s,o,"now","Now","ore/kWh");number(s,o,"min","Min","ore/kWh");number(s,o,"max","Max","ore/kWh");number(s,o,"avg","Average","ore/kWh");const cJSON *a=field(o,"hourly");for(int i=0;i<24;i++){double n;if(numeric(cJSON_GetArrayItem(a,i),&n))line(s,"%02d: %.0f ore/kWh\n",i,n);}}
 // Collector sun times represent today's date. Keep them if available; ambient
 // next rising/setting are next events and must not be labelled as today's.
 if(!out[DASH_SUN].valid){o=field(v,"sun");s=section(out,DASH_SUN,o,0,now);if(s->valid){line(s,"Next sunrise: %s\nNext sunset: %s\n",str(o,"rise"),str(o,"set"));double n;if(numeric(field(o,"moon_pct"),&n)&&n>=0)line(s,"Moon illumination: %.0f %%\n",n);else line(s,"Moon: unavailable\n");}}
 }
 for(int i=0;i<DASH_COUNT;i++)if(out[i].received_us==now&&!out[i].text[0])out[i].valid=false;
 cJSON_Delete(owned);cJSON_Delete(root);return true;
}
bool dashboard_fresh(const dashboard_section_t *s,uint64_t now,int64_t wall) {
 if(!s||!s->valid||!s->received_us||now<s->received_us||now-s->received_us>900000000ULL)return false;
 if(s->observed_at && wall>1700000000 && (wall<(int64_t)s->observed_at-60 || wall-(int64_t)s->observed_at>1800))return false;
 return true;
}
