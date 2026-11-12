/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef B2L_BLIT_H
#define B2L_BLIT_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
/* The source and destination extents are real mapped allocations. Never derive
 * memory bounds solely from userspace width/height. No resize or YUV fallback. */
struct b2l_blit_rect {
    uint32_t w, h;          /* clipped size in pixels */
    uint64_t src, dst;      /* byte offsets of the first copied pixel */
};

/* Validate and clip one copy; 1 with *r filled, 0 when nothing may be copied. */
static inline int b2l_blit_clip(uint64_t dbytes,uint32_t dw,uint32_t dh,uint32_t ds,
    uint64_t sbytes,uint32_t sw,uint32_t sh,uint32_t ss,uint32_t soff,
    int32_t sx,int32_t sy,int32_t dx,int32_t dy,int32_t width,int32_t height,
    struct b2l_blit_rect *r)
{
    if(!dw||!dh||!sw||!sh||dw>16384||dh>16384||sw>16384||sh>16384||
       ds<dw*4||ss<sw*4||ds>65536||ss>65536||sx<0||sy<0||dx<0||dy<0||
       (uint32_t)sx>=sw||(uint32_t)sy>=sh||(uint32_t)dx>=dw||(uint32_t)dy>=dh||
       width<=0||height<=0)return 0;
    uint32_t w=(uint32_t)width,h=(uint32_t)height;
    if(w>sw-(uint32_t)sx)w=sw-(uint32_t)sx;
    if(w>dw-(uint32_t)dx)w=dw-(uint32_t)dx;
    if(h>sh-(uint32_t)sy)h=sh-(uint32_t)sy;
    if(h>dh-(uint32_t)dy)h=dh-(uint32_t)dy;
    uint64_t end_s=(uint64_t)soff+((uint64_t)sy+h-1)*ss+((uint64_t)sx+w)*4;
    uint64_t end_d=((uint64_t)dy+h-1)*ds+((uint64_t)dx+w)*4;
    if(end_s>sbytes||end_d>dbytes)return 0;
    r->w=w;r->h=h;
    r->src=(uint64_t)soff+(uint64_t)sy*ss+(uint64_t)sx*4;
    r->dst=(uint64_t)dy*ds+(uint64_t)dx*4;
    return 1;
}

static inline int b2l_blit(void *dst,uint64_t dbytes,uint32_t dw,uint32_t dh,uint32_t ds,
    const void *src,uint64_t sbytes,uint32_t sw,uint32_t sh,uint32_t ss,uint32_t soff,
    int32_t sx,int32_t sy,int32_t dx,int32_t dy,int32_t width,int32_t height)
{
    struct b2l_blit_rect r;
    if(!dst||!src||!b2l_blit_clip(dbytes,dw,dh,ds,sbytes,sw,sh,ss,soff,sx,sy,dx,dy,width,height,&r))return 0;
    /* memmove tolerates the default scanout surface being re-presented. */
    for(uint32_t y=0;y<r.h;++y)
        memmove((uint8_t*)dst+r.dst+(uint64_t)y*ds,(const uint8_t*)src+r.src+(uint64_t)y*ss,(size_t)r.w*4);
    return 1;
}

static inline uint64_t b2l_blit_load64(const uint8_t *p){uint64_t v;memcpy(&v,p,8);return v;}

#ifndef B2L_BLIT_SPAN_CHECK
#define B2L_BLIT_SPAN_CHECK(row,a,b,n) ((void)0)
#endif

/* Copy only what changed. shadow has the destination's layout and holds what
 * the destination currently shows; it lives in cached memory, while dst may be
 * uncached panel memory. Each row writes one span, from the first to the last
 * differing pixel, to both. *written receives the bytes written to dst.
 *
 * Spans are widened to 64-byte boundaries of the destination address (or the
 * row edges). On the S10+, short writes at arbitrary 4-byte offsets into the
 * write-combined panel memory silently stopped the whole machine at the first
 * changed frame (builds 0109-0118); 64-byte aligned spans (0119) do not.
 * Returns 1 when the rectangle was valid (even if nothing changed). */
static inline int b2l_blit_dirty(void *dst,void *shadow,uint64_t dbytes,uint32_t dw,uint32_t dh,uint32_t ds,
    const void *src,uint64_t sbytes,uint32_t sw,uint32_t sh,uint32_t ss,uint32_t soff,
    int32_t sx,int32_t sy,int32_t dx,int32_t dy,int32_t width,int32_t height,uint64_t *written)
{
    struct b2l_blit_rect r;
    *written=0;
    if(!dst||!shadow||!src||!b2l_blit_clip(dbytes,dw,dh,ds,sbytes,sw,sh,ss,soff,sx,sy,dx,dy,width,height,&r))return 0;
    size_t n=(size_t)r.w*4;
    for(uint32_t y=0;y<r.h;++y){
        const uint8_t *s=(const uint8_t*)src+r.src+(uint64_t)y*ss;
        uint8_t *m=(uint8_t*)shadow+r.dst+(uint64_t)y*ds;
        size_t a=0,b=n;
        while(a+8<=n&&b2l_blit_load64(s+a)==b2l_blit_load64(m+a))a+=8;
        while(a<n&&s[a]==m[a])a++;
        if(a==n)continue;
        while(b>=a+8&&b2l_blit_load64(s+b-8)==b2l_blit_load64(m+b-8))b-=8;
        while(b>a&&s[b-1]==m[b-1])b--;
        a&=~(size_t)3;b=(b+3)&~(size_t)3;          /* whole pixels */
        {
            uintptr_t row=(uintptr_t)dst+r.dst+(uint64_t)y*ds;
            size_t lo=(size_t)((row+a)&63),hi=(size_t)((64-((row+b)&63))&63);
            a=lo>a?0:a-lo;                         /* row is 4-byte aligned */
            b=b+hi>n?n:b+hi;
            B2L_BLIT_SPAN_CHECK(row,a,b,n);
        }
        memcpy(m+a,s+a,b-a);
        memmove((uint8_t*)dst+r.dst+(uint64_t)y*ds+a,s+a,b-a);
        *written+=b-a;
    }
    return 1;
}
#endif
