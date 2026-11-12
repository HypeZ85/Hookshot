"""Run the production translation lookup for NR labels and composed values in every locale."""
RUN_ALL_ARGS = ["--cxx", "{cxx}"]
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def block(text, marker):
    start = text.index(marker)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cxx', default='g++')
    args = parser.parse_args()
    source = (ROOT/'gpu/vk/vk_menu_i18n.cpp').read_text(encoding='utf-8')
    fixture = r'''
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "gpu/vk/menu_i18n.h"
namespace mkwvk::i18n {
int selected=0;
int lang_now() { return selected; }
std::string tr_text(const std::string&);
'''
    for marker in ('size_t conversion(', 'bool match_format(', 'std::string fill_format(',
                   'struct Table {', 'const Table& table(', 'std::string tr_text(', 'const char* tr('):
        fixture += block(source, marker)+(';' if marker.startswith('struct ') else '')+'\n'
    fixture += r'''
}
int main() {
    using namespace mkwvk;
    using namespace mkwvk::i18n;
    for (int lang=0;lang<menu_i18n::kLangCount;++lang) {
        selected=lang;
        const auto& locale=menu_i18n::kLangs[lang];
        for (int i=0;i<locale.count;++i) {
            const auto& entry=locale.entries[i];
            assert(tr_text(entry.en)==entry.text);
        }
        char expected[256];
        snprintf(expected,sizeof expected,tr("Intensity %d%%"),35);
        assert(tr_text("Intensity 35%")==expected);
        assert(tr_text("121%")=="121%");
        assert(tr_text("unknown NR diagnostic")=="unknown NR diagnostic");
        for (const char* text : {"NR applying...", "Temporal history", "Auto skin mask", "Local structure"}) {
            assert(!tr_text(text).empty());
            if (lang) assert(tr_text(text)!=text);
        }
        const std::string composed=tr_text("Local structure")+"  3/14";
        assert(composed.find("3/14")!=std::string::npos);
        assert(!tr_text("Base Options").empty() && !tr_text("Advanced Options").empty());
        assert(tr_text(" (unavailable)").front()==' ');
    }
}
'''
    with tempfile.TemporaryDirectory(prefix='nr-i18n-') as folder:
        cpp=Path(folder)/'lookup.cpp'
        exe=Path(folder)/'lookup.exe'
        cpp.write_text(fixture,encoding='utf-8')
        subprocess.run([args.cxx,'-std=c++20','-Wall','-Wextra','-Werror','-I'+str(ROOT),str(cpp),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=15)
    print('PASS: every locale, real lookup, formatted percentages, composed labels and fallback')

if __name__ == '__main__':
    main()
