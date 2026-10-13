"""生成当前界面所需字模，不依赖固件运行时字体库。"""
from pathlib import Path
import math
import re
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware"
FONTS = Path("C:/Windows/Fonts")
def generate_fonts():
    sources = "\n".join(path.read_text(encoding="utf-8-sig") for path in FIRMWARE.glob("*.h")
                        if path.name not in {"font_assets.h", "replay_assets.h"})
    strings = "".join(re.findall(r'"([^"\n]*)"', sources))
    characters = sorted(set(chr(i) for i in range(32, 127)) | {c for c in strings if ord(c) >= 128})
    output = ["#pragma once", "#include <stdint.h>",
              "// 本文件由 tools/generate_ui_assets.py 生成，仅包含当前界面使用的字符。",
              "namespace FontAssets {",
              "struct Glyph { uint32_t codepoint; uint32_t offset; uint8_t width; uint8_t advance; };",
              "struct Font { const Glyph* glyphs; const uint8_t* alpha; unsigned count; unsigned height; };"]
    for name, size, path, alphabet in [
        ("small", 11, FONTS / "msyh.ttc", characters),
        ("body", 14, FONTS / "msyh.ttc", characters),
        ("number", 43, FONTS / "segoeuil.ttf", list(" +-./0123456789")),
    ]:
        font = ImageFont.truetype(str(path), size)
        ascent, descent = font.getmetrics()
        height = ascent + descent + 1
        origin_y = 0
        if name == "number":
            origin_y = 2 - font.getbbox("0123456789")[1]
            height = font.getbbox("0123456789")[3] - font.getbbox("0123456789")[1] + 4
        glyphs, alpha = [], []
        for character in sorted(alphabet):
            width = max(1, math.ceil(font.getlength(character)), font.getbbox(character)[2])
            bitmap = Image.new("L", (width, height), 0)
            ImageDraw.Draw(bitmap).text((0, origin_y), character, font=font, fill=255)
            glyphs.append((ord(character), len(alpha), width, math.ceil(font.getlength(character))))
            alpha.extend(bitmap.tobytes())
        output.append(f"inline const uint8_t {name}Alpha[] = {{")
        for start in range(0, len(alpha), 40):
            output.append(",".join(str(value) for value in alpha[start:start + 40]) + ",")
        output.append("};")
        output.append(f"inline const Glyph {name}Glyphs[] = {{")
        output.extend("{%d,%d,%d,%d}," % glyph for glyph in glyphs)
        output.append("};")
        output.append(f"inline const Font {name} = {{{name}Glyphs, {name}Alpha, {len(glyphs)}, {height}}};")
    output.append("}")
    (FIRMWARE / "font_assets.h").write_text("\n".join(output) + "\n", encoding="utf-8")
    print(f"Generated fonts: {len(characters)} characters; header {len(''.join(output))} characters")



if __name__ == "__main__":
    generate_fonts()
