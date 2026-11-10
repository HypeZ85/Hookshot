"""Real NR menu persistence/transactions extracted from vk_menu.cpp; no game/GPU."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
def block(text, marker):
    start=text.index(marker)
    end=text.index('{',start)+1
    depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}')
        end+=1
    return text[start:end]

def main():
    p=argparse.ArgumentParser()
    p.add_argument('--cxx',default='g++')
    a=p.parse_args()
    menu=(ROOT/'gpu/vk/vk_menu.cpp').read_text(encoding='utf-8')
    options=block((ROOT/'gpu/vk/vk_menu.h').read_text(encoding='utf-8'),'struct Options {')+';'
    controls=block((ROOT/'gpu/vk/vk_dlss.h').read_text(encoding='utf-8'),'struct DlssNrControls {')+';'
    fixture=r"""
#include <algorithm>
#include <atomic>
#include <cassert>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include "gpu/vk/dlss_nr_controls.h"
#include "gpu/vk/dlss_nr_support.h"
using namespace mkwvk;
"""
    # Keep NR helper types/functions in their production namespace.
    fixture=fixture.replace('using namespace mkwvk;','namespace mkwvk {\n'+options+'\n'+controls+r"""
Options opts, saved;
DlssNrControls published;
std::atomic<bool> open_now{true};
bool menu_refresh=false;
int page=0,cur_tab=4;
constexpr int TAB_DLSS5=4,VK_F8=0x77;
struct Host { bool (*take_key_press)(int); } *host=nullptr;
void logf(const char*,...) {}
int saves=0,override_value=-1;
void save();
void dlss_nr_publish(const DlssNrControls& v) { published=v; }
void dlss_nr_compare(int v) { override_value=v; }
dlss_nr::Issue dlss_nr_check(bool=false) { return dlss_nr::Issue::none; }
dlss_nr::Issue dlss_nr_issue() { return dlss_nr::Issue::none; }
""")
    for marker in ['bool nr_slider(','dlss_nr::Settings nr_model(','void nr_look(','void nr_publish(','const char* nr_ini_key(']:
        fixture+=block(menu,marker)+'\n'
    fixture+=r"""
struct Row { int* value; int lo, hi; };
Row rows[] = {{&opts.dlss5_group, 0, 1}, {&opts.dlss5_intensity, 0, 200}};
const char* ini_key(const Row& r) { return nr_ini_key(r.value); }
int output_choice(int value) { return value; }
"""+block(menu, 'bool nr_prepare_row(')+r"""
void load_line(const char* input) {
    char line[128]; snprintf(line, sizeof line, "%s", input);
    char* eq = strchr(line, '='); assert(eq); *eq = 0;
"""+block(menu[menu.index('void menu_init() {'):], 'for (Row& r : rows) {')+'\n}\n'
    start=menu.index('dlss_nr::Comparison nr_comparison;')
    end=menu.index('bool nr_toolbar_preview()',start)
    fixture+=menu[start:end]
    fixture+=block(menu,'void nr_compare_enter() {')+'\n'+block(menu,'void nr_compare_leave(bool commit) {')+r"""
void save() { ++saves; saved=opts; }
} // namespace mkwvk
using namespace mkwvk;
int main() {
    for (int current : {0, 1}) for (int dir : {-1, 1}) {
        opts.dlss5_group=current;
        int next=current+dir;
        assert(nr_prepare_row(rows[0], next, dir));
        assert(next==(dir<0 ? 0 : 1)); // A named group is selected, never toggled away by a second click.
    }
    for (const char* legacy : {"dlss5_group=0", "dlss5_group=2", "dlss5_group=3", "dlss5_group=-9", "dlss5_group=9999999999999"}) {
        opts.dlss5_group=1; load_line(legacy); assert(opts.dlss5_group==0);
    }
    load_line("dlss5_group=1"); assert(opts.dlss5_group==1);
    load_line("dlss5_intensity=999"); assert(opts.dlss5_intensity==200);
    load_line("dlss5_intensity=invalid"); assert(opts.dlss5_intensity==200);
    load_line("dlss5_intensity=35");
    opts.dlss5=1; opts.dlss5_profile=dlss_nr::Balanced;
    opts.dlss5_debug=4; opts.dlss5_compare_mode=1;
    assert(nr_slider(&opts.dlss5_intensity) && !nr_slider(&opts.dlss5_profile));
    assert(std::string(nr_ini_key(&opts.dlss5_intensity))=="dlss5_intensity");
    assert(std::string(nr_ini_key(&opts.dlss5_compare_mode))=="dlss5_compare_mode");
    assert(std::string(nr_ini_key(&opts.dlss5_compare_top))=="dlss5_compare_top");
    assert(nr_ini_key(&opts.rumble)==nullptr);
    nr_compare_enter();
    assert(nr_comparison.active && override_value==1 && opts.dlss5_debug==0);
    opts.dlss5_intensity=80; opts.dlss5_color=0; nr_comparison.toggle();
    assert(nr_persisted_value(&opts.dlss5_intensity)==35);
    assert(nr_persisted_value(&opts.dlss5_color)==100);
    assert(nr_persisted_value(&opts.dlss5)==1);
    nr_compare_leave(false);
    assert(!nr_comparison.active && open_now && cur_tab==TAB_DLSS5 && page==0);
    assert(opts.dlss5_intensity==35 && opts.dlss5_color==100 && opts.dlss5==1 && saves==0);
    assert(opts.dlss5_debug==4 && opts.dlss5_compare_mode==1 && override_value==-1);
    nr_compare_enter();
    opts.dlss5_intensity=45; opts.dlss5_profile=dlss_nr::Custom;
    nr_comparison.toggle();
    nr_compare_leave(true);
    assert(saves==1 && saved.dlss5==0 && saved.dlss5_intensity==45);
    assert(saved.dlss5_profile==dlss_nr::Original && saved.dlss5_debug==4);
    assert(open_now && !nr_comparison.active && published.model.intensity==45 && !published.requested);
    std::puts("NR group migration, keys, temporary save masking, Cancel restore and Save commit: PASS");
}
"""
    with tempfile.TemporaryDirectory(prefix='nr-menu-') as tmp:
        src=Path(tmp)/'menu.cpp'; exe=Path(tmp)/'menu.exe'
        src.write_text(fixture,encoding='utf-8')
        subprocess.run([a.cxx,'-std=c++20','-Wall','-Wextra','-Werror','-I'+str(ROOT),str(src),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
if __name__=='__main__':main()
