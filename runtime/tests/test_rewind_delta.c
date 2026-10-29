#include "../src/rewind_delta.h"
#include <stdio.h>
#define CHECK(x) do { if(!(x)) { puts(#x); return 1; } } while(0)
int main(void) {
    RwHistory r={0};
    /* Odd tail, unchanged frames, page boundaries, wrap/eviction, then fork. */
    uint8_t expected[24][RW_PAGE*3+17];
    memset(expected,0,sizeof(expected));
    for(unsigned i=0;i<24;i++) {
        if(i) memcpy(expected[i],expected[i-1],sizeof(expected[i]));
        if(i%3) {
            expected[i][i*613%sizeof(expected[i])]=(uint8_t)i;
            expected[i][sizeof(expected[i])-1]=(uint8_t)(255-i);
        }
        CHECK(rw_push(&r,expected[i],sizeof(expected[i]),i,7));
        CHECK(r.count<=7);
    }
    for(unsigned i=23;i>16;i--) {
        CHECK(rw_back(&r)); CHECK(r.tick==i-1);
        CHECK(!memcmp(r.image,expected[i-1],r.size));
    }
    CHECK(!rw_back(&r)); CHECK(r.bytes==0);
    CHECK(rw_push(&r,expected[3],sizeof(expected[3]),100,7));
    CHECK(rw_back(&r)); CHECK(r.tick==16);
    CHECK(!memcmp(r.image,expected[16],r.size));
    /* Variable-length device payloads preserve history across shrink/grow. */
    CHECK(rw_push(&r,expected[1],27,101,7));
    CHECK(r.count==1 && r.size==27);
    CHECK(rw_push(&r,expected[2],sizeof(expected[2]),102,7));
    CHECK(rw_back(&r) && r.size==27);
    CHECK(!memcmp(r.image,expected[1],27));
    CHECK(rw_back(&r) && r.size==sizeof(expected[16]));
    CHECK(!memcmp(r.image,expected[16],r.size));
    rw_clear(&r);
    /* Sequential fragments crossing pages match whole-image capture; an
     * aborted serialization restores every byte even after shrinking. */
    CHECK(rw_push(&r,expected[5],sizeof(expected[5]),5,7));
    CHECK(rw_begin(&r,7));
    CHECK(rw_write(&r,expected[12],17));
    CHECK(rw_write(&r,expected[12]+17,6000));
    CHECK(rw_write(&r,expected[12]+6017,sizeof(expected[12])-6017));
    CHECK(rw_finish(&r,12));
    CHECK(!memcmp(r.image,expected[12],r.size));
    CHECK(rw_back(&r)); CHECK(!memcmp(r.image,expected[5],r.size));
    CHECK(rw_begin(&r,7)); CHECK(rw_write(&r,expected[23],6000));
    rw_abort(&r); CHECK(!memcmp(r.image,expected[5],r.size));
    rw_clear(&r); rw_clear(&r);
    puts("rewind delta: tail, unchanged, eviction, backwards, fork, layout PASS");
    return 0;
}
