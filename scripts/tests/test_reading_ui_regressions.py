"""Run the production header and TXT cache code with small seams."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def method(source, name):
    start = source.index(name)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def run_cpp(program):
    with tempfile.TemporaryDirectory(prefix='reading-ui-') as directory:
        cpp = Path(directory) / 'check.cpp'
        exe = Path(directory) / 'check'
        cpp.write_text(program)
        subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


class ReadingUiRegressionTest(unittest.TestCase):
    def test_header_subtitle_is_inside_clip(self):
        source = (ROOT / 'src/components/themes/inx/InxTheme.cpp').read_text()
        code = method(source, 'void InxTheme::drawHeader(')
        run_cpp(r'''
#include <algorithm>
#include <cassert>
#include <cstring>
#include <initializer_list>
struct Rect { int x, y, width, height; };
constexpr int SMALL_FONT_ID = 1, NOTOSERIF_12_FONT_ID = 2, kIconGap = 8, kRowPadding = 20;
namespace EpdFontFamily { enum Style { REGULAR, BOLD }; }
struct CrossPointSettings { enum class HIDE_BATTERY_PERCENTAGE { HIDE_ALWAYS }; };
struct { CrossPointSettings::HIDE_BATTERY_PERCENTAGE hideBatteryPercentage{}; } SETTINGS;
namespace InxMetrics { struct { int batteryWidth=20, batteryHeight=10, batteryBarHeight=24, contentSidePadding=20; } values; }
struct GfxRenderer {
  mutable Rect clip{};
  mutable bool clipped=false;
  mutable int subtitles=0;
  struct ClipScope {
    const GfxRenderer& r;
    ClipScope(const GfxRenderer& r, int x, int y, int w, int h):r(r) { r.clip={x,y,w,h}; r.clipped=true; }
    ~ClipScope() { r.clipped=false; }
  };
  int getLineHeight(int font) const { return font == SMALL_FONT_ID ? 18 : 36; }
  int getTextWidth(int, const char* text) const { return static_cast<int>(strlen(text))*6; }
  void fillRect(int,int,int,int,bool) const {}
  void drawLine(int,int,int,int,bool) const {}
  void drawText(int font, int x, int y, const char* text, bool=true, EpdFontFamily::Style=EpdFontFamily::REGULAR) const {
    if (font != SMALL_FONT_ID) return;
    ++subtitles;
    assert(clipped && x >= clip.x && y >= clip.y);
    assert(x + getTextWidth(font,text) <= clip.x + clip.width);
    assert(y + getLineHeight(font) <= clip.y + clip.height);
  }
};
struct InxTheme {
  void drawBatteryRight(const GfxRenderer&, Rect, bool) const {}
  void drawHeader(const GfxRenderer&, Rect, const char*, const char*) const;
};
''' + code + r'''
int main() {
  GfxRenderer r;
  for (int width : {480, 800}) for (int y : {0, 12}) {
    for (const char* subtitle : {"更多详情", "More Details", "2026-09-16"})
      InxTheme{}.drawHeader(r, {0,y,width,66}, "Stats", subtitle);
  }
  assert(r.subtitles == 12);
  InxTheme{}.drawHeader(r, {0,0,480,66}, "Stats", nullptr);
  assert(r.subtitles == 12);
}
''')

    def test_txt_spacing_keeps_cache_fields_byte_aligned(self):
        import re
        source = (ROOT / 'src/activities/reader/TxtReaderActivity.cpp').read_text()
        read = method(source, 'bool readPodChecked(')
        write = method(source, 'bool writePodChecked(')
        expression = re.search(
            r'!writePodChecked\(f, (.*SETTINGS\.extraParagraphSpacing.*?)\)\s*\|\|\s*!writePodChecked\(f, complete\)',
            source).group(1)
        run_cpp(r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
struct HalFile {
  std::vector<uint8_t> bytes;
  size_t position=0;
  size_t write(const uint8_t* data,size_t size) {bytes.insert(bytes.end(),data,data+size);return size;}
  int read(uint8_t* data,size_t size) {
    if(position+size>bytes.size()) return 0;
    std::memcpy(data,bytes.data()+position,size);position+=size;return static_cast<int>(size);
  }
};
struct {uint8_t extraParagraphSpacing=0;} SETTINGS;
''' + 'template<typename T>\n' + read + '\ntemplate<typename T>\n' + write + r'''
int main() {
  for(uint8_t level=0;level<=5;++level) {
    SETTINGS.extraParagraphSpacing=level;
    HalFile f;
    assert(writePodChecked(f, ''' + expression + r'''));
    const uint8_t complete=1, encoding=1;
    const uint32_t pageCount=7;
    assert(writePodChecked(f,complete));assert(writePodChecked(f,encoding));assert(writePodChecked(f,pageCount));
    assert(f.bytes.size()==7);
    HalFile reopened=f;
    uint8_t spacing=255,loadedComplete=0,loadedEncoding=0;uint32_t loadedPages=0;
    assert(readPodChecked(reopened,spacing));assert(readPodChecked(reopened,loadedComplete));
    assert(readPodChecked(reopened,loadedEncoding));assert(readPodChecked(reopened,loadedPages));
    assert(spacing==(level!=0) && loadedComplete==1 && loadedEncoding==1 && loadedPages==7);
    assert(reopened.position==reopened.bytes.size());
  }
}
''')


if __name__ == '__main__':
    unittest.main()
