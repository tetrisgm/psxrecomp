/* Exact main-engine differential proof, including dormant projection bytes,
 * aliased destinations, scalar precision, stale generations and partial words. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../src/pgxp.cpp"
#include <cassert>
#include <cstdio>
extern "C" int gte_geometry_correction_lookup(uint32_t, int32_t*, int32_t*) { return 0; }
extern "C" int gte_geometry_correction_lookup_probe(uint32_t, int32_t*, int32_t*) { return 0; }
extern "C" int psx_mod_gpu_dma_memory_contains(uint32_t, uint32_t) { return 0; }
static uint32_t seed = 0x60420917;
static uint32_t next() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
static void value(PGXPValue &v) {
    auto *bytes = reinterpret_cast<unsigned char*>(&v);
    for (size_t i=0;i<sizeof(v);++i) bytes[i] = (unsigned char)next();
    v.value = next(); v.gen = next()%3 ? g_pgxp_cpu.gen : 0;
    v.flags = (uint16_t)(next() & 0x7f);
    v.x16 = (int32_t)((v.value & 0xffffu) << 16) | (next() & 0xffffu);
    v.y16 = (int32_t)(v.value & 0xffff0000u) | (next() & 0xffffu);
}
int main() {
    // Independent libm oracle across normal/exceptional IEEE encodings and
    // all guest shift counts. Compare payloads, signed zero and every bit.
    const uint64_t edges[] = {0, UINT64_C(0x8000000000000000), 1,
        UINT64_C(0x000fffffffffffff), UINT64_C(0x0010000000000000),
        UINT64_C(0x7fefffffffffffff), UINT64_C(0x7ff0000000000000),
        UINT64_C(0xfff0000000000000), UINT64_C(0x7ff8000012345678),
        UINT64_C(0x7ff0000012345678)};
    for (unsigned n=0;n<1000000;++n) {
        uint64_t bits=n<630 ? edges[(n/63)%10] : (uint64_t(next())<<32)|next();
        double a;std::memcpy(&a,&bits,sizeof a);
        int sh=int(n%63)-31;
        double expected=std::ldexp(a,sh), actual=pgxp_scale_pow2(a,sh);
        assert(!std::memcmp(&expected,&actual,sizeof actual));
    }
    std::puts("PASS: 1000000 power-of-two scales match libm bit for bit");
    pgxp_set_enabled(1); pgxp_set_cpu_mode(1);
    const uint32_t ops[] = {0,2,3,4,6,7,0x10,0x11,0x12,0x13,0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x2a,0x2b};
    const uint32_t imms[] = {8,9,0xa,0xb,0xc,0xd,0xe,0xf,0x10};
    const uint32_t loads[] = {0x20,0x21,0x22,0x23,0x24,0x25,0x26};
    const uint32_t stores[] = {0x28,0x29,0x2a,0x2b,0x2e};
    for (unsigned n=0;n<100000;++n) {
        pgxp_set_cpu_mode((n % 3) != 0);
        pgxp_set_preserve_projection((n & 4) != 0);
        for (unsigned r=0;r<34;++r) {
            value(g_pgxp_cpu.gpr[r]);
            auto &s=g_pgxp_cpu.scalar[r];s.value=g_pgxp_cpu.gpr[r].value;
            s.gen=next()%4 ? 0 : g_pgxp_cpu.gen;s.v=(double)(int32_t)s.value+0.25;
        }
        uint32_t rs=next()%32,rt=next()%32,rd=next()%32,sh=next()%32;
        uint32_t s1=rs ? g_pgxp_cpu.gpr[rs].value : 0;
        uint32_t s2=rt ? g_pgxp_cpu.gpr[rt].value : 0;
        uint32_t instr, result;
        if(n&1) {
            uint32_t fn=ops[next()%(sizeof(ops)/sizeof(*ops))];
            instr=(rs<<21)|(rt<<16)|(rd<<11)|(sh<<6)|fn;
            result=s1+s2;
            if(fn==0x22||fn==0x23) result=s1-s2;
            if(fn==0x24) result=s1&s2;
            if(fn==0x25) result=s1|s2;
            if(fn==0x26) result=s1^s2;
            if(fn==0x27) result=~(s1|s2);
            if(fn==0||fn==2||fn==3||fn==4||fn==6||fn==7) {
                s1=rt ? g_pgxp_cpu.gpr[rt].value : 0;
                s2=fn<4?sh:(rs?g_pgxp_cpu.gpr[rs].value:0);
                result=(fn&3)==0?s1<<(s2&31):(fn&3)==2?s1>>(s2&31):(uint32_t)((int32_t)s1>>(s2&31));
            }
        } else {
            uint32_t op=imms[next()%(sizeof(imms)/sizeof(*imms))], imm=next()&0xffffu;
            if(!(n%8)) imm=0;
            instr=(op<<26)|(rs<<21)|(rt<<16)|imm;
            s2=imm;result=s1+(int16_t)imm;
            if(op==0xc) result=s1&imm;
            if(op==0xd) result=s1|imm;
            if(op==0xe) result=s1^imm;
            if(op==0xf) result=imm<<16;
        }
        PGXPCompiledState before=g_pgxp_cpu;
        psx_pgxp_alu(nullptr,instr,result,s1,s2);
        PGXPCompiledState after=g_pgxp_cpu;
        g_pgxp_cpu=before;
        psx_pgxp_alu_inline(nullptr,instr,result,s1,s2);
        if(std::memcmp(&after,&g_pgxp_cpu,sizeof(after))) {
            std::printf("ALU mismatch case=%u instr=%08x rs=%u rt=%u rd=%u\n",n,instr,rs,rt,rd);return 1;
        }
        const uint32_t addresses[]={0x80100000u,0xa0300000u,0x1f800100u,0x1fc00000u,0x1f801810u,PSX_MOD_GPU_DMA_APERTURE_BASE,PSX_MOD_MEMORY_BASE};
        uint32_t addr=addresses[next()%7]+(next()&3u);
        PGXPValue *slot=pgxp_inline_ptr(addr),saved{},expected{};
        if(slot) { value(*slot);saved=*slot; }
        uint32_t val=slot?slot->value:next();if(n%7==0) val^=0xffff;
        uint32_t op=loads[next()%7]; instr=(op<<26)|(rt<<16);
        before=g_pgxp_cpu;psx_pgxp_load(nullptr,instr,addr,val);after=g_pgxp_cpu;if(slot)expected=*slot;
        g_pgxp_cpu=before;if(slot)*slot=saved;
        psx_pgxp_load_inline(nullptr,instr,addr,val);
        if(std::memcmp(&after,&g_pgxp_cpu,sizeof(after))||(slot&&std::memcmp(slot,&expected,sizeof(expected)))) {
            std::printf("LOAD mismatch case=%u instr=%08x\n",n,instr);return 2;
        }
        op=stores[next()%5];instr=(op<<26)|(rt<<16);val=rt?g_pgxp_cpu.gpr[rt].value:0;
        if(slot)saved=*slot;
        before=g_pgxp_cpu;psx_pgxp_store(nullptr,instr,addr,val);after=g_pgxp_cpu;if(slot)expected=*slot;
        g_pgxp_cpu=before;if(slot)*slot=saved;
        psx_pgxp_store_inline(nullptr,instr,addr,val);
        if(std::memcmp(&after,&g_pgxp_cpu,sizeof(after))||(slot&&std::memcmp(slot,&expected,sizeof(expected)))) {
            std::printf("STORE mismatch case=%u instr=%08x\n",n,instr);return 3;
        }
    }
    /* Exact integer addresses, carry boundaries and near misses exercise the
     * ADDI shortcut. Compare every byte, including dormant destination data. */
    for (unsigned n=0;n<50000;++n) {
        pgxp_set_cpu_mode((n % 13) != 0);
        pgxp_set_preserve_projection((n & 1) != 0);
        uint32_t rs=1+next()%31, rt=(n&1)?rs:next()%32;
        value(g_pgxp_cpu.gpr[rs]); value(g_pgxp_cpu.gpr[rt]);
        uint32_t s1=next();
        if(n%7==0) s1=(uint16_t)s1;
        if(n%7==1) s1=(uint32_t)(int32_t)(int16_t)s1;
        auto &a=g_pgxp_cpu.gpr[rs];
        a.gen=g_pgxp_cpu.gen; a.value=s1; a.flags=PGXP_F_VXY;
        a.x16=(int32_t)(s1<<16); a.y16=(int32_t)(s1&0xffff0000u);
        auto &sc=g_pgxp_cpu.scalar[rs];sc.gen=0;sc.value=s1;sc.v=(int32_t)s1+0.25;
        switch(n%11) {
        case 0:sc.gen=g_pgxp_cpu.gen;break;
        case 1:a.x16|=1;break;
        case 2:a.flags|=PGXP_F_VZ;break;
        case 3:a.gen=0;break;
        case 4:a.flags|=PGXP_F_DX;break;
        case 5:a.value^=1;break;
        }
        uint32_t imm=next()&0xffffu;
        uint32_t instr=((8u+(n&1))<<26)|(rs<<21)|(rt<<16)|imm;
        uint32_t result=s1+(uint32_t)(int32_t)(int16_t)imm;
        if(n%19==0)result^=1;
        PGXPCompiledState before=g_pgxp_cpu;
        psx_pgxp_alu(nullptr,instr,result,s1,imm);
        PGXPCompiledState after=g_pgxp_cpu;
        g_pgxp_cpu=before;psx_pgxp_alu_inline(nullptr,instr,result,s1,imm);
        if(std::memcmp(&after,&g_pgxp_cpu,sizeof(after))) {
            std::printf("ADDI mismatch case=%u instr=%08x source=%08x\n",n,instr,s1);return 4;
        }
    }
    /* Journal restoration and explicit diagnostics must remain on the reference path. */
    uint32_t sw=(0x2bu<<26)|(1u<<16),lw=(0x23u<<26)|(2u<<16);
    PGXPValue prior=*pgxp_inline_ptr(0x80100000u);
    pgxp_checkpoint_begin();
    psx_pgxp_store_inline(nullptr,sw,0x80100000u,7);
    psx_pgxp_load_inline(nullptr,lw,0x80100000u,7);
    pgxp_checkpoint_rollback();
    assert(!std::memcmp(&prior,pgxp_inline_ptr(0x80100000u),sizeof(prior)));
    const PGXPStoreRecord *ring;uint32_t cap;
    assert(pgxp_store_ring(&ring,&cap)==0 && cap==0);
    pgxp_store_trace_enable(1);psx_pgxp_store_inline(nullptr,sw,0x80100000u,9);
    assert(pgxp_store_ring(&ring,&cap)==1 && cap);
    pgxp_store_trace_enable(0);psx_pgxp_store_inline(nullptr,sw,0x80100000u,10);
    assert(pgxp_store_ring(&ring,&cap)==1);
    std::puts("PASS: 350000 PGXP operations match all state bytes; checkpoint/trace semantics pass");
}
