#!/usr/bin/env python3
"""Exercise actual framebuffer timing counters, including concurrent copies."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[1]
source = (repo / 'xnu-overlay/iokit/Kernel/arm/B2LFramebuffer.cpp').read_text()
start = source.index('static uint64_t b2l_fb_swap_count')
body = source[start:source.index('static uint64_t\nb2l_probe_hash', start)]
# The fork's runCopy times the whole queued job once; the S10 merge feeds that
# elapsed time to the same B2L FRAMES copy counters.
assert 'if (copies) b2l_fb_copy_note(ticks);' in source
pre = r'''
#include <cassert>
#include <cstdint>
#include <thread>
#include <cstdio>
constexpr uint64_t NSEC_PER_SEC = 1000000000;
static uint64_t now;
static uint64_t mach_absolute_time() { return now; }
static void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *ticks) { *ticks = ns; }
'''
post = r'''
int main() {
    uint64_t out[7];
    now = 10; b2l_fb_swap_note(); b2l_fb_copy_note(40);
    now += 3000000000; b2l_fb_swap_note();
    b2l_fb_swap_snapshot(out);
    assert(out[0] == 2 && out[1] == 3000000000 && out[2] == 1 && out[3] == now);
    assert(out[4] == 1 && out[5] == 40 && out[6] == 40);
    b2l_fb_swap_snapshot(out);
    assert(!out[1] && !out[6] && out[0] == 2 && out[4] == 1 && out[5] == 40);
    std::thread workers[4];
    for (unsigned i = 0; i < 4; i++) workers[i] = std::thread([] {
        for (unsigned n = 0; n < 1000; n++) b2l_fb_copy_note(17);
    });
    for (auto &worker : workers) worker.join();
    b2l_fb_swap_snapshot(out);
    assert(out[0] == 2 && out[4] == 4001 && out[5] == 68040 && out[6] == 17);
    puts("Framebuffer timing: frame gaps and copy elapsed time stay separate, interval maxima reset, concurrent totals retained");
}
'''
with tempfile.TemporaryDirectory(prefix='b2l-fb-timing-') as temporary:
    root = Path(temporary)
    c, exe = root / 'test.cpp', root / 'test'
    c.write_text(pre + body + post)
    subprocess.run(shlex.split(os.environ.get('CXX', 'clang++')) +
                   ['-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-pthread', '-fsanitize=address,undefined', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
