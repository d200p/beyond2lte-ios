#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Run the actual presentation path (queue, worker, dirty-span copy, wait) with
native iOS 26 client-wire fixtures and a deferred mock worker thread."""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'xnu-overlay/iokit/Kernel/arm/B2LFramebuffer.cpp').read_text()
get=s[s.index('static uint32_t\nget32('):s.index('\nbool\nIOMobileFramebufferUserClient::initWithTask')]
# The b2l_fb_probe diagnostic (IOMemoryDescriptor, IOMallocData) is not part of the present path.
get=get[:get.index('static uint64_t\nb2l_probe_hash(')]+get[get.index('/* Wait until no presentation is queued or being copied. */'):]
# The display-controller dump and the scanout flip touch registers; the mock class stubs them out.
get=get[:get.index('void\nAppleCLCD::deconDumpMain(')]+get[get.index('/* Copy the queued layers into the panel, then release them and go idle. */'):]
code=r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdarg>
#include <climits>
static unsigned spans;
static void span_check(uintptr_t row,size_t a,size_t b,size_t n){
 ++spans;assert(a<b&&b<=n&&a%4==0&&b%4==0);
 assert((row+a)%64==0||a==0);assert((row+b)%64==0||b==n);
}
#define B2L_BLIT_SPAN_CHECK(row,a,b,n) span_check(row,a,b,n)
#include "B2LBlit.h"
#define B2L_FB_LAYERS 4
#define B2L_FB_FORMAT_BGRA 0x42475241
#define THREAD_UNINT 0
#ifndef __unused
#define __unused __attribute__((unused))
#endif
#include <cstdlib>
#include <cstdio>
typedef void *thread_call_param_t;
typedef struct call *thread_call_t;
struct Memory {uint64_t length;uint64_t getLength(){return length;}};
struct B2LSurface {
 uint32_t width=4,height=3,bytesPerRow=24,offset=8,pixelFormat=B2L_FB_FORMAT_BGRA;
 uint32_t bytesPerElement=4,elementWidth=1,elementHeight=1;
 uint8_t data[80]{};Memory backing{80};Memory *memory=&backing;int refs=0;
 uintptr_t kernelAddress(){return (uintptr_t)data;}
 void release(){assert(refs>0);--refs;}
};
static B2LSurface surfaces[6];
static B2LSurface *b2l_surface_copy(uint32_t id){
 if(id<1||id>5)return nullptr;++surfaces[id].refs;return &surfaces[id];
}
#define OSSafeReleaseNULL(p) do{if(p){p->release();p=nullptr;}}while(0)
static unsigned locks,shown,sleeps,wakeups,enters;
static uint64_t now;
static uint64_t mach_absolute_time(){return now+=10;}
#define NSEC_PER_SEC 1000000000ull
static void absolutetime_to_nanoseconds(uint64_t t,uint64_t *ns){*ns=t;}
static unsigned traces,begins,ends;
__attribute__((format(printf,1,2))) static void b2l_diag_record(const char *f,...){
 char b[512];va_list a;va_start(a,f);vsnprintf(b,sizeof(b),f,a);va_end(a);++traces;
 if(strstr(b,"B2L FB COPY")&&strstr(b," begin "))++begins;
 if(strstr(b,"B2L FB COPY")&&strstr(b," end ")){++ends;assert(ends==begins&&strstr(b,"nonblack="));}
}
static void IOLockLock(void*){assert(!locks);++locks;}
static void IOLockUnlock(void*){assert(locks==1);--locks;}
static void b2l_early_gui_present(){++shown;}
static void IOLog(const char*,...){}
static thread_call_param_t pending_owner;
static bool thread_call_enter(thread_call_t){assert(!pending_owner);++enters;pending_owner=(void*)1;return false;}
static void run_worker();
/* Sleeping releases the lock; the deferred worker runs meanwhile. */
static void IOLockSleep(void*,void*,int){assert(locks==1);++sleeps;--locks;run_worker();++locks;}
static void IOLockWakeup(void*,void*,bool){++wakeups;}
class AppleCLCD {
 public:
 uintptr_t fPixels;uintptr_t fPanelPixels=0;uint32_t fTopInset=0;uint32_t fWidth=4,fHeight=3,fRowBytes=20;
 B2LSurface *fSurface=nullptr;void *fLock=nullptr;bool fConsolePaused=false;
 uint32_t fPerfSwaps=0,fPerfCopies=0,fPerfFull=0,fPerfReports=0,fPerfWaits=0,fCopyLogs=0;
 uint64_t fPerfPixels=0,fPerfTicks=0,fPerfMaxTicks=0,fPerfWritten=0,fPerfWaitTicks=0;
 struct Layer { B2LSurface *s; int32_t sx, sy, dx, dy, w, h; };
 struct Job { uint32_t count; Layer layer[B2L_FB_LAYERS]; };
 Job fJob{};bool fBusy=false;thread_call_t fCopyCall=nullptr;uint8_t *fShadow=nullptr;bool fShadowValid=false;
 void swap(const uint8_t*);void waitIdle(void);void runCopy(void);
 static void copyWorker(thread_call_param_t owner, thread_call_param_t unused);
 /* Scanout is off in this harness: every frame takes the copy path under test. */
 bool flipTo(B2LSurface*,const Layer*){return false;}
 void pointAtPanel(){}
};
static AppleCLCD *the_fb;
static void run_worker(){if(pending_owner){pending_owner=nullptr;AppleCLCD::copyWorker(the_fb,nullptr);}}
''' + get.replace('__asm__ volatile("dsb sy":::"memory");','/* host: no ARM barrier */') + r'''
static void put(uint8_t *p,unsigned off,uint32_t v){memcpy(p+off,&v,4);}
// Build the userland client object, not a kernel-layout struct. Native
// SwapEnd sends exactly client+0x18 through client+0x59f (0x588 bytes).
static void layer(uint8_t *client,unsigned i,unsigned id,unsigned mask=0,unsigned w=4,unsigned h=3) {
 put(client,0x164,1u<<i);put(client,0x168,mask);put(client,0xb4+i*4,id);
 put(client,0xc4+i*16,0);put(client,0xc8+i*16,0);
 put(client,0xcc+i*16,w);put(client,0xd0+i*16,h);
 put(client,0x124+i*16,0);put(client,0x128+i*16,0);
 put(client,0x12c+i*16,w);put(client,0x130+i*16,h);
}
static uint8_t client[0x5a0],panel[68],shadow[60];
static AppleCLCD fb;
static void fill(unsigned id){
 for(unsigned y=0;y<3;++y)for(unsigned x=0;x<4;++x)
  put(surfaces[id].data,8+y*24+x*4,0xff000000|id*0x10000|y*16|x);
}
static void check(unsigned id){
 for(unsigned y=0;y<3;++y){
  assert(!memcmp(panel+4+y*20,surfaces[id].data+8+y*24,16));
  for(unsigned x=16;x<20;++x)assert(panel[4+y*20+x]==0xa5);
 }
 for(unsigned x=0;x<4;++x)assert(panel[x]==0xa5&&panel[64+x]==0xa5);
 if(fb.fShadow&&fb.fShadowValid)
  for(unsigned y=0;y<3;++y)assert(!memcmp(shadow+y*20,panel+4+y*20,16));
}
static void released(){for(unsigned i=1;i<=5;++i)assert(surfaces[i].refs==0);assert(locks==0&&!fb.fBusy);}
static void present(unsigned i,unsigned id){memset(client,0,sizeof(client));layer(client,i,id);fb.swap(client+0x18);fb.waitIdle();}
static uint64_t written(){uint64_t w=fb.fPerfWritten;fb.fPerfWritten=0;return w;}
int main(){
 the_fb=&fb;fb.fPixels=(uintptr_t)(panel+4);
 for(unsigned id=1;id<=5;++id)fill(id);

 /* Synchronous fallback, no shadow: every layer slot, full writes. */
 for(unsigned i=0;i<4;++i){memset(panel,0xa5,sizeof(panel));present(i,i+1);check(i+1);released();assert(written()==48);}
 assert(shown==1&&fb.fConsolePaused&&fb.fPerfFull==4&&!enters);
 // Removed layers may retain a stale surface ID; disable wins over update.
 memset(panel,0xa5,sizeof(panel));memset(client,0,sizeof(client));layer(client,3,4,8);fb.swap(client+0x18);
 for(auto v:panel)assert(v==0xa5);released();
 // Former wrong mask location must never activate a layer.
 memset(client,0,sizeof(client));layer(client,0,1);put(client,0x164,0);put(client,0x194,1);
 fb.swap(client+0x18);for(auto v:panel)assert(v==0xa5);released();
 // Invalid ID, unsupported format, short real backing and outside rects.
 for(unsigned bad=0;bad<4;++bad){
  memset(client,0,sizeof(client));layer(client,0,bad==0?4096:1);
  if(bad==1)surfaces[1].pixelFormat=0;
  if(bad==2)surfaces[1].backing.length=4;
  if(bad==3)put(client,0xc4,UINT_MAX);
  fb.swap(client+0x18);fb.waitIdle();for(auto v:panel)assert(v==0xa5);released();
  surfaces[1].pixelFormat=B2L_FB_FORMAT_BGRA;surfaces[1].backing.length=80;
 }
 // All four enabled layers are read, copied in order and released.
 memset(client,0,sizeof(client));for(unsigned i=0;i<4;++i)layer(client,i,i+1);
 put(client,0x164,15);fb.swap(client+0x18);check(4);released();

 /* Worker and shadow. */
 fb.fCopyCall=(thread_call_t)1;fb.fShadow=shadow;memset(shadow,0,sizeof(shadow));written();
 // A partial first copy does not validate the shadow: still full writes.
 memset(panel,0xa5,sizeof(panel));memset(client,0,sizeof(client));layer(client,0,1,0,2,2);
 fb.swap(client+0x18);assert(surfaces[1].refs==1&&fb.fBusy&&pending_owner);  // queued, still referenced
 fb.waitIdle();released();assert(!fb.fShadowValid&&written()==16);
 for(unsigned y=0;y<2;++y)assert(!memcmp(panel+4+y*20,surfaces[1].data+8+y*24,8));
 // A full copy validates it.
 present(0,1);check(1);released();assert(fb.fShadowValid&&written()==48);
 // Unchanged frame: nothing written to the panel.
 present(0,1);check(1);released();assert(written()==0);
 // One changed pixel: its 64-byte aligned span, here the whole 16-byte row.
 put(surfaces[1].data,8+1*24+2*4,0x12345678);present(0,1);check(1);released();assert(written()==16&&spans);
 // Changes at both row ends: one span covering them.
 put(surfaces[1].data,8+2*24+0,0x1);put(surfaces[1].data,8+2*24+12,0x2);present(0,1);check(1);released();assert(written()==16);
 // A different full-screen surface rewrites only differing pixels.
 present(0,2);check(2);released();assert(written()==48);
 surfaces[3]=surfaces[2];surfaces[3].refs=0;present(0,3);check(3);released();assert(written()==0);

 // The next SwapEnd waits for the queued copy before taking a new one.
 fill(1);unsigned waits=fb.fPerfWaits;
 memset(client,0,sizeof(client));layer(client,0,1);fb.swap(client+0x18);assert(fb.fBusy&&surfaces[1].refs==1);
 memset(client,0,sizeof(client));layer(client,0,2);fb.swap(client+0x18);
 assert(fb.fPerfWaits==waits+1&&surfaces[1].refs==0&&surfaces[2].refs==1);
 fb.waitIdle();check(2);released();assert(wakeups>=sleeps);
 // An empty swap still waits for the previous copy, queues nothing.
 memset(client,0,sizeof(client));layer(client,0,1);fb.swap(client+0x18);
 memset(client,0,sizeof(client));fb.swap(client+0x18);assert(!fb.fBusy);check(1);released();
 assert(locks==0&&!pending_owner);

 // Random edits: the panel always equals the presented surface afterwards,
 // and each frame writes no more than the bytes of rows that changed.
 srand(7);fill(4);present(0,4);check(4);released();written();
 for(unsigned round=0;round<2000;++round){
  unsigned edits=rand()%4,rows=0;uint8_t before[80];memcpy(before,surfaces[4].data,80);
  for(unsigned e=0;e<edits;++e){unsigned y=rand()%3,x=rand()%4;
   put(surfaces[4].data,8+y*24+x*4,rand()%3?(uint32_t)rand():0);}
  for(unsigned y=0;y<3;++y)rows+=memcmp(before+8+y*24,surfaces[4].data+8+y*24,16)!=0;
  present(0,4);check(4);released();uint64_t w=written();assert(w<=rows*16u&&(rows==0)==(w==0));
 }
 assert(fb.fCopyLogs==64&&begins==64&&ends==64);   /* bounded, always paired */
}
'''
with tempfile.TemporaryDirectory(prefix='b2l-fb-swap-') as tmp:
 tmp=Path(tmp);src=tmp/'check.cpp';src.write_text(code);binary=tmp/'check'
 subprocess.run(['clang++','-std=c++17','-g','-O1','-Wall','-Wextra','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',str(root/'xnu-overlay/iokit/Kernel/arm'),str(src),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
print('PASS: native swap fixture through queue, worker and wait; dirty spans, shadow validation, disable masks, bounds and surface releases (ASan/UBSan)')
