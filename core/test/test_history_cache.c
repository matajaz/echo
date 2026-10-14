#include "../history_cache.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    history_cache_t b={.magic=HISTORY_CACHE_MAGIC,.version=1};
    unsigned index=100%CHART_HISTORY_POINTS;
    b.records[0][index]=(history_record_t){.slot=101,.count=10,.average=12,.min=10,.max=15};
    b.crc=history_crc(&b,offsetof(history_cache_t,crc)); assert(history_cache_valid(&b,100));
    assert(!history_cache_valid(&b,99));
    b.records[0][index].average=100; assert(!history_cache_valid(&b,100));
    b.crc=history_crc(&b,offsetof(history_cache_t,crc)); assert(!history_cache_valid(&b,100));
    b.records[0][index].average=12; b.records[0][index].slot=100;
    b.crc=history_crc(&b,offsetof(history_cache_t,crc)); assert(!history_cache_valid(&b,100));
    b.records[0][index].slot=101; b.version=2;
    b.crc=history_crc(&b,offsetof(history_cache_t,crc)); assert(!history_cache_valid(&b,100));
    puts("history cache: all pass"); return 0;
}
