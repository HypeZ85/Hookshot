"""Exercise Compare's real row enumeration and preview edits without NGX or a game."""
RUN_ALL_ARGS = ["--cxx", "{cxx}"]
import argparse
from pathlib import Path
import re
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
    menu = (ROOT / 'gpu/vk/vk_menu.cpp').read_text(encoding='utf-8')
    entries = re.findall(r'\{TAB_DLSS5, "([^"]+)", (nullptr|&opts\.\w+), (-?\d+), (-?\d+), (-?\d+),', menu)
    # Platform-specific descriptions repeat a row; preserve the authored order once.
    entries = list(dict.fromkeys(entries))
    fields = sorted({value.removeprefix('&opts.') for _, value, *_ in entries if value != 'nullptr'})
    source = r'''
#include <algorithm>
#include <cassert>
#include <cstring>
#include <set>
#include <string>
#include "gpu/vk/dlss_nr_controls.h"
using namespace mkwvk;
enum { TAB_DLSS5 };
struct Options { FIELDS } opts;
struct Row { int tab; const char* name; int* value; int lo,hi,step; };
Row rows[] = { ROWS };
struct { bool on=true; } nr_comparison;
int nr_compare_setting_index=0, published=0, changed=0, retried=0;
bool locked=false;
bool row_fixed(const int*) { return locked; }
bool nr_prepare_row(Row&, int&, int) { return true; }
void nr_row_changed(Row&) { ++changed; }
void nr_publish() { ++published; }
void nr_look(dlss_nr::Look) {}
void dlss_nr_retry() { ++retried; }
bool stops_at_ends(const Row& r) { return r.value==&opts.dlss5_intensity || r.value==&opts.dlss5_color; }
HELPERS
bool nr_compare_advanced=true, changed_ui=false;
int nr_compare_selected=3;
int navigation(int key) {
    bool press[8]{}; press[key]=true;
    bool tab_left=false, tab_right=false, changed=false;
NAVIGATION
    changed_ui=changed;
    return adjust;
}
int main() {
    assert(navigation(2)==0 && nr_compare_selected==6 && changed_ui);
    assert(navigation(2)==0 && nr_compare_selected==7);
    assert(navigation(3)==-1 && nr_compare_selected==7);
    assert(navigation(1)==0 && nr_compare_selected==6);
    assert(navigation(4)==1 && nr_compare_selected==6);
    assert(navigation(1)==0 && nr_compare_selected==3);
    nr_compare_advanced=false; nr_compare_selected=0;
    assert(navigation(3)==0 && nr_compare_selected==5);
    int count=0;
    nr_compare_setting(0,&count);
    std::set<std::string> names;
    for (int i=0;i<count;++i) {
        Row* row=nr_compare_setting(i);
        assert(row && names.insert(row->name).second);
    }
    for (const char* name : {"Temporal history", "Auto skin mask", "UI correction", "Debug view",
         "Style", "Model preset", "Local structure", "Local tone", "Skin structure", "Color strength"}) assert(names.count(name)==1);
    assert(!names.count("Compare shortcut") && !names.count("Retry detection") && !names.count("Reset NR settings"));
    assert(!names.count("Group") && !names.count("Controls") && !names.count("Compare"));
    assert(!nr_compare_setting(-1) && !nr_compare_setting(count));
    for (int i=0;i<count;++i) {
        Row& row=*nr_compare_setting(i);
        if (!strcmp(row.name,"Intensity")) {
            opts.dlss5_intensity=200;
            nr_compare_adjust(row,1); assert(opts.dlss5_intensity==200);
            nr_compare_adjust(row,-1); assert(opts.dlss5_intensity==195);
            locked=true; nr_compare_adjust(row,-1); assert(opts.dlss5_intensity==195); locked=false;
        }
        if (!strcmp(row.name,"Temporal history")) {
            opts.dlss5_history=1; nr_compare_adjust(row,1); assert(opts.dlss5_history==0);
        }
        if (!strcmp(row.name,"Color strength")) {
            opts.dlss5_color=100; nr_compare_adjust(row,1); assert(opts.dlss5_color==100);
        }
        if (!strcmp(row.name,"Retry detection")) nr_compare_adjust(row,1);
    }
    assert(changed==4 && retried==0 && published>=4);
}
'''
    source = source.replace('FIELDS', '\n'.join('int '+name+'=0;' for name in fields))
    source = source.replace('ROWS', '\n'.join('{TAB_DLSS5,"'+name+'",'+value+','+','.join(bounds)+'},'
                                            for name, value, *bounds in entries))
    source = source.replace('HELPERS', block(menu, 'Row* nr_compare_setting(')+'\n'+block(menu, 'void nr_compare_adjust('))
    nav_start=menu.index('        int adjust = 0;', menu.index('nr_script_f8_press = false;', menu.index('void menu_tick')))
    source = source.replace('NAVIGATION', menu[nav_start:menu.index('        bool activate =',nav_start)])
    with tempfile.TemporaryDirectory(prefix='nr-compare-') as folder:
        cpp = Path(folder)/'compare.cpp'
        exe = Path(folder)/'compare.exe'
        cpp.write_text(source, encoding='utf-8')
        subprocess.run([args.cxx,'-std=c++20','-Wall','-Wextra','-Werror','-I'+str(ROOT),str(cpp),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
    print('PASS: both advanced groups reachable, bounds, locking, actions and preview delegation')

if __name__ == '__main__':
    main()
