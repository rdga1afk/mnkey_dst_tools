// The font of the first-run window (it has to exist before the Kenshi folder is known): Arimo Regular, Apache-2.0, pulled in by the assembler.
// MD_FONT_DIR: absolute path of engine/src/vendor/fonts (set in game/CMakeLists.txt).
#include "first_run_dialog.h"

__asm__(".section .rodata\n"
        ".balign 16\n"
        ".global md_firstrun_font_data\n"
        "md_firstrun_font_data:\n"
        ".incbin \"" MD_FONT_DIR "/Arimo-Regular.ttf\"\n"
        "md_firstrun_font_data_end:\n"
        ".balign 4\n"
        ".global md_firstrun_font_size_v\n"
        "md_firstrun_font_size_v:\n"
        ".long md_firstrun_font_data_end - md_firstrun_font_data\n"
        ".previous\n");
