#!/usr/bin/env python3
"""Make Mesa's OSMesa target create a Panfrost screen first (softpipe when there is none) and accept
OpenGL ES contexts, for the EAGL layer CoreAnimation's GLES renderer will run on.

  patch-osmesa.py MESA_DIR        (after patch-mesa.py; configure with -Dosmesa=true, softpipe+panfrost)

Every edit asserts that the text it replaces is found exactly once, so a Mesa change shows up as a
clear failure here rather than a silent no-op. Edits follow the S7 port's gpu/mesa/patch-mesa.py,
re-checked against Mesa 24.3.4. Also copies sweet_hw.c into the OSMesa target.
"""
import shutil
import sys
from pathlib import Path

mesa = Path(sys.argv[1])
here = Path(__file__).resolve().parent


def edit(rel, old, new):
    path = mesa / rel
    text = path.read_text()
    assert text.count(old) == 1, f'{rel}: expected text found {text.count(old)} times:\n{old}'
    path.write_text(text.replace(old, new))
    print('patched', rel)


# OSMesa: the GPU screen first, softpipe when it is unavailable.
edit('src/gallium/targets/osmesa/osmesa_target.c',
     "struct pipe_screen *\nosmesa_create_screen(void)\n{\n",
     "struct pipe_screen *sweet_hw_create_screen(void) __attribute__((weak));\n\n"
     "struct pipe_screen *\nosmesa_create_screen(void)\n{\n"
     "   if (sweet_hw_create_screen) {\n"
     "      struct pipe_screen *hw = sweet_hw_create_screen();\n"
     "      if (hw)\n"
     "         return debug_screen_wrap(hw);\n"
     "   }\n\n")
edit('src/gallium/targets/osmesa/meson.build',
     "libosmesa = shared_library(\n  osmesa_lib_name,\n  'osmesa_target.c',\n",
     "osmesa_sweet_files = []\n"
     "osmesa_sweet_deps = []\n"
     "if host_machine.system() == 'darwin' and with_gallium_panfrost\n"
     "  osmesa_sweet_files += 'sweet_hw.c'\n"
     "  osmesa_sweet_deps += [driver_panfrost]\n"
     "  foreach d : ['dep_libdrm', 'idep_xmlconfig']\n"
     "    if is_variable(d)\n"
     "      osmesa_sweet_deps += get_variable(d)\n"
     "    endif\n"
     "  endforeach\n"
     "endif\n\n"
     "libosmesa = shared_library(\n  osmesa_lib_name,\n  'osmesa_target.c', osmesa_sweet_files,\n")
edit('src/gallium/targets/osmesa/meson.build',
     "    dep_ws2_32, dep_thread, dep_clock, dep_unwind, driver_swrast, idep_mesautil,\n",
     "    dep_ws2_32, dep_thread, dep_clock, dep_unwind, driver_swrast, idep_mesautil,\n"
     "    osmesa_sweet_deps,\n")

# OpenGL ES contexts from OSMesa (profile 0x36): major 1 = ES1, else ES2/ES3. From Bergamota's
# gpu/eagl/mesa patch (MIT), as the S7 port uses it.
edit('src/gallium/frontends/osmesa/osmesa.c',
     "             profile != OSMESA_COMPAT_PROFILE)\n            return NULL;",
     "             profile != OSMESA_COMPAT_PROFILE &&\n"
     "             profile != 0x36 /* OpenGL ES profile */)\n            return NULL;")
edit('src/gallium/frontends/osmesa/osmesa.c',
     "      ? API_OPENGL_CORE : API_OPENGL_COMPAT;\n",
     "      ? API_OPENGL_CORE : API_OPENGL_COMPAT;\n"
     "   if (profile == 0x36) /* GLES context: major 1 = ES1, else ES2/ES3 */\n"
     "      attribs.profile = (version_major == 1) ? API_OPENGLES : API_OPENGLES2;\n")

shutil.copy(here / 'sweet_hw.c', mesa / 'src/gallium/targets/osmesa/sweet_hw.c')
print('copied sweet_hw.c')
