#include "hid_map.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    struct hid_map_info info;
    uint8_t packet[64], map[BRIDGE_MAP_MAX];
    /* Conventional unnumbered boot-format keyboard, with LED outputs. */
    const uint8_t keyboard[]={
        0x75,1,0x95,8,0x81,2, 0x75,8,0x95,1,0x81,1,
        0x75,8,0x95,6,0x81,0, 0x75,1,0x95,5,0x91,2,
        0x75,3,0x95,1,0x91,1,
    };
    assert(!hid_map_parse(keyboard,sizeof(keyboard),&info));
    assert(!info.uses_ids && info.input_len[0]==8 && info.output_len[0]==1);
    uint8_t keys[8]={0,0,4};
    assert(hid_packet(&info,0,keys,8,packet)==8 && !memcmp(keys,packet,8));
    assert(hid_packet(&info,1,keys,8,packet)<0);
    assert(hid_packet(&info,0,keys,7,packet)<0);
    /* Numbered NKRO + media + mouse; constants count toward report lengths. */
    const uint8_t composite[]={
        0x85,1,0x75,1,0x95,8,0x81,2,0x95,232,0x81,2,
        0x85,2,0x75,16,0x95,1,0x81,0,
        0x85,3,0x75,8,0x95,5,0x81,6,
    };
    assert(!hid_map_parse(composite,sizeof(composite),&info));
    assert(info.uses_ids && info.input_len[1]==30 && info.input_len[2]==2 && info.input_len[3]==5);
    assert(hid_packet(&info,3,keys,5,packet)==6 && packet[0]==3);
    /* Push/pop restores report size, count and ID. */
    const uint8_t pushpop[]={0x85,1,0x75,8,0x95,1,0xa4,0x85,2,0x95,2,0x81,2,0xb4,0x81,2};
    assert(!hid_map_parse(pushpop,sizeof(pushpop),&info));
    assert(info.input_len[1]==1 && info.input_len[2]==2);
    const uint8_t truncated[]={0x77,1};
    const uint8_t mixed[]={0x75,8,0x95,1,0x81,2,0x85,1,0x81,2};
    const uint8_t too_big[]={0x85,1,0x75,8,0x95,64,0x81,2};
    const uint8_t underflow[]={0xb4};
    const uint8_t zero_id[]={0x85,0};
    assert(hid_map_parse(truncated,sizeof(truncated),&info)<0);
    assert(hid_map_parse(mixed,sizeof(mixed),&info)<0);
    assert(hid_map_parse(too_big,sizeof(too_big),&info)<0);
    assert(hid_map_parse(underflow,sizeof(underflow),&info)<0);
    assert(hid_map_parse(zero_id,sizeof(zero_id),&info)<0);
    assert(hid_map_parse(keyboard,0,&info)<0);
    for (int n=1;n<argc;n++) {
        FILE *f=fopen(argv[n],"rb"); assert(f);
        size_t len=fread(map,1,sizeof(map),f); fclose(f);
        assert(!hid_map_parse(map,len,&info));
        assert(info.uses_ids && info.input_len[1]==8 &&
               info.input_len[2]==12 && info.input_len[3]==9);
        unsigned count=0;
        printf("%s:",argv[n]);
        for(unsigned id=0;id<256;id++) if(info.input_len[id]) {
            printf(" ID %u: %u bytes",id,info.input_len[id]); count++;
            memset(keys,0,sizeof(keys));
            uint8_t data[64]={0};
            assert(hid_packet(&info,id,data,info.input_len[id],packet)==info.input_len[id]+info.uses_ids);
        }
        assert(count>=3); puts("");
    }
    /* Deterministic malformed-descriptor smoke fuzzing, run under ASan/UBSan. */
    unsigned seed=0x12345;
    for(unsigned i=0;i<20000;i++) {
        unsigned len=i%sizeof(map)+1;
        for(unsigned j=0;j<len;j++) { seed=seed*1664525u+1013904223u; map[j]=seed>>24; }
        hid_map_parse(map,len,&info);
    }
    puts("HID descriptor and packet tests passed");
    return 0;
}
