#!/usr/bin/env python

import os

import locale
locale.setlocale(locale.LC_ALL, 'en_US.UTF-8')

header = """
#
#   #####   #######           #     #     #     #     #     #
# ##     #  #     #          # #    #  #  #    # #     #   #
# ##        #     #         #   #   #  #  #   #   #     # #
# ##  ####  #     #        #     #  #  #  #  #     #     #
# ##     #  #     #        #######  #  #  #  #######     #      ###
# ##     #  #     #        #     #  #  #  #  #     #     #      ###
# # #####   #######        #     #   ## ##   #     #     #      ###
#
# Seriously. You shouldn't even be looking at this file unless you're
# debugging generate_mozbuild.py.
#
# DO NOT MODIFY THIS FILE IT IS AUTOGENERATED.
#
"""

footer = """

# can we find a better way of dealing with asm sources?

# left out of UNIFIED_SOURCES for now; that's not C++ anyway, nothing else to unify it with
if not CONFIG['INTEL_ARCHITECTURE'] and CONFIG['CPU_ARCH'] == 'arm' and CONFIG['GNU_CC']:
    SOURCES += [
        'trunk/src/opts/memset.arm.S',
    ]
    if CONFIG['BUILD_ARM_NEON']:
        SOURCES += [
            'trunk/src/opts/memset16_neon.S',
            'trunk/src/opts/memset32_neon.S',
        ]

if CONFIG['INTEL_ARCHITECTURE'] and (CONFIG['GNU_CC'] or CONFIG['CLANG_CL']):
    if CONFIG['CPU_ARCH'] == 'x86_64':
        SOURCES += [
            'trunk/src/opts/SkBlitRow_opts_SSE4_x64_asm.S',
        ]
    else:
        SOURCES += [
            'trunk/src/opts/SkBlitRow_opts_SSE4_asm.S',
        ]

MSVC_ENABLE_PGO = True

FINAL_LIBRARY = 'gkmedias'
LOCAL_INCLUDES += [
    'trunk/include/config',
    'trunk/include/core',
    'trunk/include/effects',
    'trunk/include/gpu',
    'trunk/include/images',
    'trunk/include/pathops',
    'trunk/include/pipe',
    'trunk/include/ports',
    'trunk/include/utils',
    'trunk/include/utils/mac',
    'trunk/include/utils/win',
    'trunk/include/views',
    'trunk/src/core',
    'trunk/src/gpu',
    'trunk/src/gpu/effects',
    'trunk/src/gpu/gl',
    'trunk/src/image',
    'trunk/src/lazy',
    'trunk/src/opts',
    'trunk/src/sfnt',
    'trunk/src/utils',
    'trunk/src/utils/mac',
    'trunk/src/utils/win',
]

if CONFIG['MOZ_WIDGET_TOOLKIT'] in ('android', 'gtk2', 'gtk3', 'qt', 'gonk', 'cocoa'):
    DEFINES['SK_USE_POSIX_THREADS'] = 1

if CONFIG['INTEL_ARCHITECTURE'] and CONFIG['HAVE_TOOLCHAIN_SUPPORT_MSSSE3']:
    DEFINES['SK_BUILD_SSSE3'] = 1

if CONFIG['MOZ_WIDGET_TOOLKIT'] in ('android', 'gonk'):
    DEFINES['SK_FONTHOST_CAIRO_STANDALONE'] = 0

if (CONFIG['MOZ_WIDGET_TOOLKIT'] == 'android') or \
   (CONFIG['MOZ_WIDGET_TOOLKIT'] == 'cocoa') or \
   (CONFIG['MOZ_WIDGET_TOOLKIT'] == 'gonk') or \
   (CONFIG['MOZ_WIDGET_TOOLKIT'] == 'qt') or \
   CONFIG['MOZ_WIDGET_GTK']:
    DEFINES['SK_FONTHOST_DOES_NOT_USE_FONTMGR'] = 1

# We should autogenerate these SSE related flags.

if CONFIG['MOZ_WIDGET_TOOLKIT'] == 'windows':
    DEFINES['SKIA_DLL'] = 1
    DEFINES['GR_DLL'] = 1

    # MSVC doesn't need special compiler flags, but Skia needs to be told that these files should
    # be built with the required SSE level or it will simply compile in stubs and cause runtime crashes
    SOURCES['trunk/src/opts/SkBitmapFilter_opts_SSE2.cpp'].flags += ['-DSK_CPU_SSE_LEVEL=20']
    SOURCES['trunk/src/opts/SkBitmapProcState_opts_SSE2.cpp'].flags += ['-DSK_CPU_SSE_LEVEL=20']
    SOURCES['trunk/src/opts/SkBitmapProcState_opts_SSSE3.cpp'].flags += ['-DSK_CPU_SSE_LEVEL=31']
    SOURCES['trunk/src/opts/SkBlitRect_opts_SSE2.cpp'].flags += ['-DSK_CPU_SSE_LEVEL=20']
    SOURCES['trunk/src/opts/SkBlitRow_opts_SSE2.cpp'].flags += ['-DSK_CPU_SSE_LEVEL=20']
    SOURCES['trunk/src/opts/SkBlurImage_opts_SSE2.cpp'].flags += ['-DSK_CPU_SSE_LEVEL=20']
    SOURCES['trunk/src/opts/SkBlurImage_opts_SSE4.cpp'].flags += ['-DSK_CPU_SSE_LEVEL=41']
    SOURCES['trunk/src/opts/SkMorphology_opts_SSE2.cpp'].flags += ['-DSK_CPU_SSE_LEVEL=20']
    SOURCES['trunk/src/opts/SkUtils_opts_SSE2.cpp'].flags += ['-DSK_CPU_SSE_LEVEL=20']
    SOURCES['trunk/src/opts/SkXfermode_opts_SSE2.cpp'].flags += ['-DSK_CPU_SSE_LEVEL=20']

if CONFIG['INTEL_ARCHITECTURE'] and CONFIG['GNU_CC']:
    SOURCES['trunk/src/opts/SkBitmapFilter_opts_SSE2.cpp'].flags += CONFIG['SSE2_FLAGS']
    SOURCES['trunk/src/opts/SkBitmapProcState_opts_SSE2.cpp'].flags += CONFIG['SSE2_FLAGS']
    SOURCES['trunk/src/opts/SkBitmapProcState_opts_SSSE3.cpp'].flags += ['-mssse3']
    SOURCES['trunk/src/opts/SkBlitRect_opts_SSE2.cpp'].flags += CONFIG['SSE2_FLAGS']
    SOURCES['trunk/src/opts/SkBlitRow_opts_SSE2.cpp'].flags += CONFIG['SSE2_FLAGS']
    SOURCES['trunk/src/opts/SkBlurImage_opts_SSE2.cpp'].flags += CONFIG['SSE2_FLAGS']
    SOURCES['trunk/src/opts/SkBlurImage_opts_SSE4.cpp'].flags += ['-msse4.1']
    SOURCES['trunk/src/opts/SkMorphology_opts_SSE2.cpp'].flags += CONFIG['SSE2_FLAGS']
    SOURCES['trunk/src/opts/SkUtils_opts_SSE2.cpp'].flags += CONFIG['SSE2_FLAGS']
    SOURCES['trunk/src/opts/SkXfermode_opts_SSE2.cpp'].flags += CONFIG['SSE2_FLAGS']
elif CONFIG['CPU_ARCH'] == 'arm' and CONFIG['GNU_CC'] and CONFIG['BUILD_ARM_NEON']:
    DEFINES['__ARM_HAVE_OPTIONAL_NEON_SUPPORT'] = 1
    DEFINES['USE_ANDROID_NDK_CPU_FEATURES'] = 0
elif CONFIG['CLANG_CL']:
    SOURCES['trunk/src/opts/SkBitmapProcState_opts_SSSE3.cpp'].flags += ['-mssse3']

DEFINES['SKIA_IMPLEMENTATION'] = 1
DEFINES['GR_IMPLEMENTATION'] = 1

if CONFIG['GNU_CXX']:
    CXXFLAGS += [
        '-Wno-overloaded-virtual',
        '-Wno-unused-function',
        '-fomit-frame-pointer',
    ]
    if not CONFIG['CLANG_CXX']:
        CXXFLAGS += ['-Wno-logical-op']

if CONFIG['MOZ_WIDGET_TOOLKIT'] in ('gtk2', 'gtk3', 'android', 'gonk', 'qt'):
    CXXFLAGS += CONFIG['MOZ_CAIRO_CFLAGS']
    CXXFLAGS += CONFIG['CAIRO_FT_CFLAGS']

if CONFIG['MOZ_WIDGET_TOOLKIT'] in ('gtk2', 'gtk3', 'qt'):
    CXXFLAGS += CONFIG['MOZ_PANGO_CFLAGS']
"""

import json

platforms = ['linux', 'mac', 'android', 'win']

custom_includes = {
  'trunk/src/ports/SkAtomics_sync.h': True,
  'trunk/src/ports/SkAtomics_win.h': True,
  'trunk/src/ports/SkBarriers_x86.h': True,
  'trunk/src/ports/SkBarriers_arm.h': True,
  'trunk/src/ports/SkBarriers_tsan.h': True,
  'trunk/src/ports/SkMutex_pthread.h': True,
  'trunk/src/ports/SkMutex_win.h': True
}

def generate_includes():
  includes = {}
  for root, dirs, files in os.walk('trunk/include'):
    for name in files:
      if name.endswith('.h'):
        includes[os.path.join(root, name)] = True

  return dict(includes.items() + custom_includes.items())

def generate_opt_sources():
  opt_sources = {'opts': {''}}
  for root, dirs, files in os.walk('trunk/src/opts'):
    for name in files:
      if name.endswith('.cpp'):
        opt_sources['opts'].add(os.path.join(root, name))

  return opt_sources

def generate_platform_sources():
  sources = {}

  for plat in platforms:
    if os.system("cd trunk && GYP_GENERATORS=dump_mozbuild ./gyp_skia -D OS=%s gyp/skia_lib.gyp" % plat) != 0:
      print 'Failed to generate sources for ' + plat
      continue


    f = open('trunk/sources.json');
    sources[plat] = set(json.load(f));
    f.close()

  return dict(sources.items() + generate_opt_sources().items())


def generate_separated_sources(platform_sources):
  blacklist = [
    'ChromeUtils',
    'SkImageDecoder_',
    '_gif',
    'SkFontConfigParser_android',
    'SkJpeg',
    'SkXML',
    'SkCity',
    'GrGLCreateNativeInterface',
    'fontconfig',
    'SkCondVar',
    'SkThreadUtils_pthread_',
    'SkImage_Codec',
    'SkBitmapChecksummer',
    'SkNativeGLContext',
    'SkFontConfig',
    'SkFontHost_win_dw',
    'SkFontMgr_android',
    'SkForceLinking',
    'SkMovie',
    'SkImageDecoder',
    'SkImageEncoder',
    'SkBitmapHasher',
    'SkWGL',
    'SkImages',
    'SkDiscardableMemory_ashmem',
    'SkMemory_malloc',
    'opts_check_x86',
    'third_party',
  ]

  def isblacklisted(value):
    for item in blacklist:
      if value.find(item) >= 0:
        return True

    return False

  separated = {
    'common': {
      #'trunk/src/effects/gradients/SkGradientTileProc.cpp',
      'trunk/src/gpu/gl/GrGLCreateNativeInterface_none.cpp',
      'trunk/src/ports/SkDiscardableMemory_none.cpp',
      'trunk/src/ports/SkImageDecoder_empty.cpp',
      'trunk/src/ports/SkMemory_mozalloc.cpp',
      # 'trunk/src/images/SkImages.cpp',
      # 'trunk/src/images/SkImageRef.cpp',
      # 'trunk/src/images/SkImageRef_GlobalPool.cpp',
      # 'trunk/src/images/SkImageRefPool.cpp',
      # 'trunk/src/images/SkImageDecoder.cpp',
      # 'trunk/src/images/SkImageDecoder_Factory.cpp',
    },
    'android': {
      # 'trunk/src/ports/SkDebug_android.cpp',
      'trunk/src/ports/SkFontHost_android_old.cpp',
      'trunk/src/ports/SkFontHost_cairo.cpp',
      # 'trunk/src/ports/SkFontHost_FreeType.cpp',
      # 'trunk/src/ports/SkFontHost_FreeType_common.cpp',
      # 'trunk/src/ports/SkThread_pthread.cpp',
      # 'trunk/src/ports/SkPurgeableMemoryBlock_android.cpp',
      # 'trunk/src/ports/SkTime_Unix.cpp',
      # 'trunk/src/utils/SkThreadUtils_pthread.cpp',
      # 'trunk/src/images/SkImageRef_ashmem.cpp',
      # 'trunk/src/utils/android/ashmem.cpp',
    },
    'linux': {
      'trunk/src/ports/SkFontHost_cairo.cpp',
    },
    'intel': {
      # There is currently no x86-specific opt for SkTextureCompression
      'trunk/src/opts/opts_check_x86.cpp',
      'trunk/src/opts/SkTextureCompression_opts_none.cpp',
    },
    'arm': {
      'trunk/src/opts/SkUtils_opts_arm.cpp',
      'trunk/src/core/SkUtilsArm.cpp',
    },
    'neon': {
      'trunk/src/opts/SkBitmapProcState_arm_neon.cpp',
    },
    'none': {
      'trunk/src/opts/SkUtils_opts_none.cpp',
    }
  }

  for plat in platform_sources.keys():
    if not separated.has_key(plat):
      separated[plat] = set()

    for value in platform_sources[plat]:
      if isblacklisted(value):
        continue

      if value.find('_SSE') > 0 or value.find('_SSSE') > 0 or value.find('_SSE4') > 0 : #lol
        separated['intel'].add(value)
        continue

      if value.find('_neon') > 0:
        separated['neon'].add(value)
        continue

      if value.find('_arm') > 0:
        separated['arm'].add(value)
        continue

      if value.find('_none') > 0:
        separated['none'].add(value)
        continue

      found = True
      for other in platforms:
        if other == plat or not platform_sources.has_key(other):
          continue

        if not value in platform_sources[other]:
          found = False
          break;

      if found:
        separated['common'].add(value)
      else:
        separated[plat].add(value)

  return separated

def uniq(seq):
  seen = set()
  seen_add = seen.add
  return [ x for x in seq if x not in seen and not seen_add(x)]

def write_cflags(f, values, subsearch, cflag, indent):
  def write_indent(indent):
    for _ in range(indent):
        f.write(' ')

  val_list = uniq(sorted(map(lambda val: val.replace('../', 'trunk/'), values), key=lambda x: x.lower()))

  if len(val_list) == 0:
    return

  for val in val_list:
    if val.find(subsearch) > 0:
      write_indent(indent)
      f.write("SOURCES[\'" + val + "\'].flags += [\'" + cflag + "\']\n")

def write_sources(f, values, indent):

  # Unfortunately for now the gpu and pathops directories are
  # non-unifiable. Keep track of this and fix it.
  unified_blacklist = [
    '_SSE',
    '_SSSE',
    '_neon',
    'FontHost',
    'SkAdvancedTypefaceMetrics',
    'SkBitmapProcState_matrixProcs.cpp',
    'SkBlitter_A8.cpp',
    'SkBlitter_ARGB32.cpp',
    'SkBlitter_RGB16.cpp',
    'SkBlitter_Sprite.cpp',
    'SkScan_Antihair.cpp',
    'SkCondVar.cpp',
    'SkParse.cpp',
    'GrAddPathRenderers_default.cpp',
    'GrDistanceFieldTextContext.cpp',
    'SkSHA1.cpp',
    'SkMD5.cpp',
    'SkPictureData.cpp',
    'SkScaledImageCache.cpp',
    'opts_check_x86.cpp',
  ]

  def isblacklisted(value):
    for item in unified_blacklist:
      if value.find(item) >= 0:
        return True

    return False

  sources = {}
  sources['nonunified'] = set()
  sources['unified'] = set()

  for item in values:
    if isblacklisted(item):
      sources['nonunified'].add(item)
    else:
      sources['unified'].add(item)

  write_list(f, "UNIFIED_SOURCES", sources['unified'], indent)
  write_list(f, "SOURCES", sources['nonunified'], indent)
  
def write_list(f, name, values, indent):
  def write_indent(indent):
    for _ in range(indent):
        f.write(' ')

  val_list = uniq(sorted(map(lambda val: val.replace('../', 'trunk/'), values), key=lambda x: x.lower()))

  if len(val_list) == 0:
    return

  write_indent(indent)
  f.write(name + ' += [\n')
  for val in val_list:
    write_indent(indent + 4)
    f.write('\'' + val + '\',\n')

  write_indent(indent)
  f.write(']\n')

def write_mozbuild(includes, sources):
  filename = 'moz.build'
  f = open(filename, 'w')

  f.write(header)

  write_list(f, 'EXPORTS.skia', includes, 0)

  write_sources(f, sources['common'], 0)

  f.write("if CONFIG['MOZ_WIDGET_TOOLKIT'] in ('android', 'gonk'):\n")
  write_sources(f, sources['android'], 4)

  f.write("if CONFIG['MOZ_WIDGET_TOOLKIT'] == 'cocoa':\n")
  write_sources(f, sources['mac'], 4)

  f.write("if CONFIG['MOZ_WIDGET_GTK']:\n")
  write_sources(f, sources['linux'], 4)

  f.write("if CONFIG['MOZ_WIDGET_TOOLKIT'] == 'qt':\n")
  write_sources(f, sources['linux'], 4)

  f.write("if CONFIG['MOZ_WIDGET_TOOLKIT'] == 'windows':\n")
  # Windows-specific files don't get unification because of nasty headers.
  # Luckily there are not many files in this.
  write_list(f, "SOURCES", sources['win'], 4)

  f.write("if CONFIG['INTEL_ARCHITECTURE']:\n")
  write_sources(f, sources['intel'], 4)

  f.write("elif CONFIG['CPU_ARCH'] == 'arm' and CONFIG['GNU_CC']:\n")
  write_sources(f, sources['arm'], 4)

  f.write("    if CONFIG['BUILD_ARM_NEON']:\n")
  write_list(f, 'SOURCES', sources['neon'], 8)
  write_cflags(f, sources['neon'], 'neon', '-mfpu=neon', 8)

  f.write("else:\n")
  write_sources(f, sources['none'], 4)

  f.write(footer)

  f.close()

  print 'Wrote ' + filename

def main():
  includes = generate_includes()
  platform_sources = generate_platform_sources()
  separated_sources = generate_separated_sources(platform_sources)
  write_mozbuild(includes, separated_sources)


if __name__ == '__main__':
  main()
