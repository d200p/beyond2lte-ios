#!/usr/bin/env python3
"""The platform UUID: stable, RFC 4122 shaped, and published where gethostuuid looks."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
ARM = ROOT / 'xnu-overlay/iokit/Kernel/arm'

SOURCE = r"""
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "b2l_platform_uuid.h"

int main(void) {
 static const uint8_t one[6] = { 2, 0, 0, 0, 0, 1 }, two[6] = { 2, 0, 0, 0, 0, 2 };
 uint8_t a[16], b[16], c[16], d[16];
 b2l_platform_uuid(one, 6, a);
 b2l_platform_uuid(one, 6, b);
 b2l_platform_uuid(two, 6, c);
 b2l_platform_uuid(NULL, 0, d);
 assert(!memcmp(a, b, 16));
 assert(memcmp(a, c, 16) && memcmp(a, d, 16));
 for (int i = 0; i < 3; i++) {
  const uint8_t *u = i == 0 ? a : i == 1 ? c : d;
  assert((u[6] >> 4) == 5 && (u[8] >> 6) == 2);
 }
 assert(!memmem(a, 16, one, 6));
 puts("PASS");
 return 0;
}
"""


def main():
    with tempfile.TemporaryDirectory() as work:
        work = pathlib.Path(work)
        (work / 'uuid_test.c').write_text(SOURCE)
        built = subprocess.run(['cc', '-Wall', '-I', str(ARM), '-o', str(work / 'uuid_test'), str(work / 'uuid_test.c')],
                               capture_output=True, text=True)
        assert built.returncode == 0, built.stderr
        run = subprocess.run([str(work / 'uuid_test')], capture_output=True, text=True)
        assert run.returncode == 0 and run.stdout.strip() == 'PASS', run.stdout + run.stderr[-800:]

    source = (ARM / 'B2LPlatformExpert.cpp').read_text()
    # IOBSDGetPlatformUUID waits on the resource and then reads the provider's property.
    assert 'provider->setProperty(kIOPlatformUUIDKey' in source
    assert 'publishResource(kIOPlatformUUIDKey' in source
    assert source.index('IODTPlatformExpert::start(provider)') < source.index('publishResource(kIOPlatformUUIDKey')
    # corecrypto is not registered this early: 0137 never reached storage with SHA1 here.
    header = (ARM / 'b2l_platform_uuid.h').read_text().split('#ifndef')[1]
    for text in (header, source):
        assert 'SHA1' not in text and 'crypto' not in text
    # The id must stay out of the log.
    assert 'text)' not in source.split('IOPlatformUUID published')[1].split('\n')[0]
    # configd's InterfaceNamer matches this class by name.
    assert 'class IONetworkStack : public IOService' in source
    # configd stops unless IOServiceOpen succeeds: 0141's stack had no user client.
    assert 'class IONetworkStackUserClient : public IOUserClient' in source
    assert source.index('kIOUserClientClassKey') < source.index('stack->registerService()')
    assert source.index('stack->registerService()') > source.index('publishResource(kIOPlatformUUIDKey')
    print('platform uuid tests passed')


if __name__ == '__main__':
    main()
