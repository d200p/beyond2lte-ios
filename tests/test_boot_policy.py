#!/usr/bin/env python3
"""Exercise the actual RTC wait policy with a host IOKit boundary stub."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


def main():
    source = (Path(sys.argv[1]) / "iokit/Kernel/IOStartIOKit.cpp").read_text()
    start = source.index("void\nIOKitInitializeTime(")
    end = source.index("\nvoid\niokit_post_constructor_init", start)
    body = source[start:end]
    pre = r'''
#include <cassert>
#include <cstring>
#include <cstddef>
struct mach_timespec_t { unsigned tv_sec, tv_nsec; };
static int requested, supplied;
static unsigned waited, calls, calendar_calls;
static int PE_parse_boot_argn(const char *name,void *out,size_t n){
 assert(!strcmp(name,"b2l_rtc_wait") && n==sizeof(int));
 if(!supplied)return 0;*(int*)out=requested;return 1;
}
static void b2l_early_console_policy(){}
static void b2l_boot_stage(const char*){}
struct IOService {
 static int resourceMatching(const char *name){assert(!strcmp(name,"IORTC"));return 1;}
 static void waitForService(int match,mach_timespec_t *t){
  assert(match==1 && !t->tv_nsec);calls++;waited=t->tv_sec;
 }
};
static void clock_initialize_calendar(){calendar_calls++;}
'''
    post = r'''
int main(){
 for(int value: {-1,0,1,30,31,2147483647}) {
  for(int present=0;present<2;present++){
   requested=value;supplied=present;calls=calendar_calls=0;
   IOKitInitializeTime();
   unsigned expected=30;
#if defined(B2L)
   if(present && value>=0 && value<=30)expected=(unsigned)value;
#endif
   assert(waited==expected && calls==1 && calendar_calls==1);
  }
 }
}
'''
    # Disable x86-only NVRAM probing: this harness models the ARM64 board.
    body = body.replace("#if defined(__i386__) || defined(__x86_64__)", "#if 0")
    with tempfile.TemporaryDirectory(prefix="b2l-rtc-policy-") as temporary:
        root = Path(temporary)
        src = root / "rtc.cpp"
        src.write_text("#include <initializer_list>\n" + pre + body + post)
        compiler = shlex.split(os.environ.get("CXX", "clang++"))
        for board in ([], ["-DB2L=1"]):
            binary = root / ("b2l" if board else "other")
            subprocess.run(compiler + ["-std=c++17", "-Wno-unused-function",
                           "-fsanitize=address,undefined"] + board +
                           [str(src), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
    print("RTC policy: native fallback, opt-in bounds, other boards and calendar initialization passed")


if __name__ == "__main__":
    main()
