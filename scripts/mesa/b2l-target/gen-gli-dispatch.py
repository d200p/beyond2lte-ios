#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""S10 port: the GLI dispatch table of the EAGL layer, generated at build time.

Core Animation's OpenGL ES renderer (CA::OGL::GLESContext in the 23G90
QuartzCore) takes its GL functions from [EAGLContext getMacroContextPrivate]:
a GLIContext followed by a GLIFunctionDispatch, the same table as the macOS
OpenGL framework's gliDispatch.h (975 entries in the order of the SDK header;
QuartzCore's own offsets were checked against it: finish 89, gen_textures 98,
get_integerv 104, get_string 117, tex_parameteri 304, bind_framebuffer_EXT
672, framebuffer_texture2D_EXT 677). Every entry takes the GLIContext first.

The header is Apple's and stays out of the repository: this script reads it
from the build machine's macOS SDK and writes b2l_gli_table.h with, per entry,
a thunk of the same C prototype that makes our Mesa context current, calls the
Mesa function of the same GL name through the current dispatch, and logs first
uses, GL errors in the first calls and entries Mesa does not have.

usage: gen-gli-dispatch.py GLIDISPATCH_H OUTPUT_H
"""
import re
import sys

# Sanity checks: indices QuartzCore uses, from its 23G90 code.
EXPECTED = {
    'bind_texture': 5, 'finish': 89, 'gen_textures': 98, 'get_integerv': 104, 'get_string': 117,
    'tex_parameteri': 304, 'bind_framebuffer_EXT': 672, 'gen_framebuffers_EXT': 674,
    'framebuffer_texture2D_EXT': 677, 'fence_sync': 779, 'client_wait_sync': 782,
}
COUNT = 975

# GL types by size; anything else stops the build.
INT32 = {'GLenum', 'GLint', 'GLuint', 'GLsizei', 'GLbitfield', 'GLfixed', 'GLclampx'}
INT16 = {'GLshort', 'GLushort', 'GLhalf', 'GLhalfARB', 'GLhalfNV'}
INT8 = {'GLbyte', 'GLubyte', 'GLboolean', 'GLchar', 'GLcharARB'}
INT64 = {'GLint64', 'GLuint64', 'GLint64EXT', 'GLuint64EXT', 'GLintptr', 'GLsizeiptr', 'GLintptrARB',
         'GLsizeiptrARB'}
FLOAT = {'GLfloat', 'GLclampf'}
DOUBLE = {'GLdouble', 'GLclampd'}
POINTERS = {'GLhandleARB', 'GLsync', 'GLDEBUGPROC', 'GLDEBUGPROCARB', 'GLDEBUGPROCAMD', 'GLvdpauSurfaceNV'}
C_TYPES = {}
for names, c in ((INT32, 'uint32_t'), (INT16, 'uint16_t'), (INT8, 'uint8_t'), (INT64, 'uint64_t'),
                 (FLOAT, 'float'), (DOUBLE, 'double'), (POINTERS, 'void *')):
    for n in names:
        C_TYPES[n] = c
C_TYPES['GLvoid'] = 'void'
C_TYPES['void'] = 'void'
# Signedness matters only for the value logged; widths are what the ABI needs.
SIGNED = {'GLint': 'int32_t', 'GLsizei': 'int32_t', 'GLfixed': 'int32_t', 'GLshort': 'int16_t',
          'GLbyte': 'int8_t', 'GLint64': 'int64_t', 'GLint64EXT': 'int64_t', 'GLintptr': 'int64_t',
          'GLsizeiptr': 'int64_t', 'GLintptrARB': 'int64_t', 'GLsizeiptrARB': 'int64_t'}

SUFFIXES = ('ARB', 'EXT', 'APPLE', 'NV', 'ATI', 'OES', 'KHR', 'SGIS', 'IBM', 'AMD', 'IMG')

# GLI names whose Mesa function has another name (beyond the suffix rule).
RENAMES = {
    'blend_equation_separate_ATI': 'glBlendEquationSeparate',
}


def c_type(text):
    """A GL type text ('const GLint *', 'GLenum') as a C type of the same size."""
    t = text.replace('const', ' ').strip()
    stars = t.count('*')
    base = t.replace('*', ' ').split()
    if len(base) != 1:
        raise SystemExit('unexpected type: %r' % text)
    if stars:
        return 'const void *' if 'const' in text else 'void *'
    if base[0] not in C_TYPES:
        raise SystemExit('unknown GL type: %r' % text)
    if base[0] in SIGNED:
        return SIGNED[base[0]]
    return C_TYPES[base[0]]


def camel(name):
    """bind_framebuffer_EXT -> BindFramebufferEXT, tex_image2D -> TexImage2D."""
    out = []
    for part in name.split('_'):
        if part in SUFFIXES or (part[:1].isdigit()):
            out.append(part)
        else:
            out.append(part[:1].upper() + part[1:])
    return ''.join(out)


def gl_names(name):
    """Mesa names to try, core first (Mesa aliases ARB/EXT names to the core
    slot when they are the same function; ES contexts fill the core slot)."""
    if name in RENAMES:
        return [RENAMES[name]]
    c = camel(name)
    names = []
    for s in SUFFIXES:
        if c.endswith(s) and len(c) > len(s):
            names.append('gl' + c[:-len(s)])
    names.append('gl' + c)
    return names


def parse(path):
    text = open(path, encoding='utf-8', errors='replace').read()
    m = re.search(r'typedef struct __GLIFunctionDispatchRec\s*\{(.*?)\}\s*GLIFunctionDispatch;', text, re.S)
    if not m:
        raise SystemExit('no GLIFunctionDispatch in ' + path)
    body = re.sub(r'/\*.*?\*/', ' ', m.group(1), flags=re.S)
    body = re.sub(r'//[^\n]*', ' ', body)
    entries = []
    for decl in body.split(';'):
        decl = ' '.join(decl.split())
        if not decl:
            continue
        if decl.startswith('#'):
            raise SystemExit('preprocessor line inside the table: ' + decl)
        fm = re.match(r'^(.*?)\(\s*\*\s*(\w+)\s*\)\s*\((.*)\)$', decl)
        if not fm:
            raise SystemExit('cannot parse: ' + decl)
        ret, name, params = fm.group(1).strip(), fm.group(2), fm.group(3).strip()
        plist = [p.strip() for p in params.split(',')]
        if not plist or not plist[0].startswith('GLIContext'):
            raise SystemExit('first parameter is not GLIContext: ' + decl)
        args = []
        for p in plist[1:]:
            pm = re.match(r'^(.*?)(\w+)$', p)
            if not pm or not pm.group(1).strip():
                raise SystemExit('cannot parse parameter %r in %s' % (p, name))
            ptype = pm.group(1).strip()
            # 'const GLchar* const *string' and similar
            args.append(c_type(ptype))
        entries.append((name, c_type(ret), args))
    return entries


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    entries = parse(sys.argv[1])
    if len(entries) != COUNT:
        raise SystemExit('%d entries, expected %d: layout differs from 23G90' % (len(entries), COUNT))
    index = {n: i for i, (n, _, _) in enumerate(entries)}
    for n, i in EXPECTED.items():
        if index.get(n) != i:
            raise SystemExit('%s at %s, expected %d: layout differs from 23G90' % (n, index.get(n), i))
    out = ['/* Generated by scripts/mesa/b2l-target/gen-gli-dispatch.py from the build',
           ' * machine\'s gliDispatch.h; not repository content. */',
           '#define B2L_GLI_COUNT %d' % COUNT, '']
    out.append('static const char *const b2l_gli_name[B2L_GLI_COUNT] = {')
    out += ['\t"%s",' % n for n, _, _ in entries]
    out.append('};')
    out.append('/* Mesa names to try per entry, separated by spaces. */')
    out.append('static const char *const b2l_gli_gl[B2L_GLI_COUNT] = {')
    out += ['\t"%s",' % ' '.join(gl_names(n)) for n, _, _ in entries]
    out.append('};')
    out.append('')
    for i, (name, ret, args) in enumerate(entries):
        params = ''.join(', %s a%d' % (t, k) for k, t in enumerate(args))
        call_types = ', '.join(args) if args else 'void'
        call_args = ', '.join('a%d' % k for k in range(len(args)))
        first = '(uint64_t)(uintptr_t)a0' if args and args[0].endswith('*') else \
            ('(uint64_t)a0' if args and args[0] not in ('float', 'double') else '0')
        out.append('static %s\nb2l_gli_%d(void *c%s)\n{' % (ret, i, params))
        out.append('\t_glapi_proc f = b2l_gli_enter(c, %d, %s);' % (i, first))
        if ret == 'void':
            out.append('\tif (f == NULL) {\n\t\treturn;\n\t}')
            out.append('\t((void (*)(%s))f)(%s);' % (call_types, call_args))
            out.append('\tb2l_gli_leave(%d);' % i)
        else:
            zero = '(%s)0' % ret
            out.append('\tif (f == NULL) {\n\t\treturn %s;\n\t}' % zero)
            out.append('\t%s r = ((%s (*)(%s))f)(%s);' % (ret, ret, call_types, call_args))
            out.append('\tb2l_gli_leave(%d);' % i)
            out.append('\treturn r;')
        out.append('}')
    out.append('')
    out.append('static void *const b2l_gli_thunk[B2L_GLI_COUNT] = {')
    out += ['\t(void *)b2l_gli_%d,' % i for i in range(COUNT)]
    out.append('};')
    open(sys.argv[2], 'w').write('\n'.join(out) + '\n')
    print('gli: %d entries, %s' % (COUNT, sys.argv[2]))


if __name__ == '__main__':
    main()
