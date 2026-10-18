// Embedded UI/console fonts: the raw .ttf files are pulled in by the assembler (.incbin), no C arrays.
// Symbols keep the names declared in embedded_fonts.h (md_font::<Name>, md_font::<Name>_size), hence the hand-mangled labels.
// MD_FONT_DIR: absolute path of engine/src/vendor/fonts (set in CMake).
#include "embedded_fonts.h"

#define MD_EMBED_FONT(MANGLED, MANGLED_SIZE, FILE) \
    __asm__(".section .rodata\n" \
            ".balign 16\n" \
            ".global " MANGLED "\n" \
            MANGLED ":\n" \
            ".incbin \"" MD_FONT_DIR "/" FILE ".ttf\"\n" \
            MANGLED "_end:\n" \
            ".balign 4\n" \
            ".global " MANGLED_SIZE "\n" \
            MANGLED_SIZE ":\n" \
            ".long " MANGLED "_end - " MANGLED "\n" \
            ".previous\n")

MD_EMBED_FONT("_ZN7md_font13Arimo_RegularE", "_ZN7md_font18Arimo_Regular_sizeE", "Arimo-Regular");
MD_EMBED_FONT("_ZN7md_font10Arimo_BoldE", "_ZN7md_font15Arimo_Bold_sizeE", "Arimo-Bold");
MD_EMBED_FONT("_ZN7md_font12Arimo_ItalicE", "_ZN7md_font17Arimo_Italic_sizeE", "Arimo-Italic");
MD_EMBED_FONT("_ZN7md_font16Arimo_BoldItalicE", "_ZN7md_font21Arimo_BoldItalic_sizeE", "Arimo-BoldItalic");
MD_EMBED_FONT("_ZN7md_font18UbuntuMono_RegularE", "_ZN7md_font23UbuntuMono_Regular_sizeE", "UbuntuMono-R");
MD_EMBED_FONT("_ZN7md_font15UbuntuMono_BoldE", "_ZN7md_font20UbuntuMono_Bold_sizeE", "UbuntuMono-B");
MD_EMBED_FONT("_ZN7md_font17UbuntuMono_ItalicE", "_ZN7md_font22UbuntuMono_Italic_sizeE", "UbuntuMono-RI");
MD_EMBED_FONT("_ZN7md_font21UbuntuMono_BoldItalicE", "_ZN7md_font26UbuntuMono_BoldItalic_sizeE", "UbuntuMono-BI");
