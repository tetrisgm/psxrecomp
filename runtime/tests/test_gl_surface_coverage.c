#include "../src/gl_surface_coverage.h"
#include <assert.h>
#include <limits.h>

int main(void) {
    GlCoverage map={0};unsigned expected[32*16]={0};
    assert(gl_coverage_init(&map,32,16));
    uint32_t seed=0x7331;
    for(unsigned step=0;step<2000;++step) {
        seed=seed*1664525u+1013904223u;int x=(int)(seed%40)-4;
        seed=seed*1664525u+1013904223u;int y=(int)(seed%24)-4;
        seed=seed*1664525u+1013904223u;int w=1+seed%32;
        seed=seed*1664525u+1013904223u;int h=1+seed%16;
        unsigned owner=step%9;
        int native_only=(step%3)==0;
        assert(native_only?gl_coverage_claim_native(&map,x,y,w,h,owner):
            gl_coverage_replace(&map,x,y,w,h,owner));
        for(int py=0;py<16;++py)for(int px=0;px<32;++px)
            if(px>=x && py>=y && px<x+w && py<y+h &&
               (!native_only || !expected[py*32+px]))expected[py*32+px]=owner;
        unsigned visited[32*16]={0};
        for(size_t i=0;i<map.count;++i) {
            GlCoverageRect r=map.rects[i];
            assert(r.x>=0 && r.y>=0 && r.w>0 && r.h>0 && r.x+r.w<=32 && r.y+r.h<=16);
            for(int py=r.y;py<r.y+r.h;++py)for(int px=r.x;px<r.x+r.w;++px) {
                assert(!visited[py*32+px]++);
                assert(expected[py*32+px]==r.owner);
            }
        }
        for(unsigned i=0;i<32*16;++i)assert(visited[i]==1);
    }
    assert(gl_coverage_replace(&map,0,0,INT_MAX,INT_MAX,77));
    assert(map.count==1 && map.rects[0].owner==77);
    GlCoverageRect *same=map.rects;
    assert(gl_coverage_replace(&map,3,3,7,8,77) && map.rects==same);
    gl_coverage_destroy(&map);
    assert(!map.rects && !map.count);
    return 0;
}
