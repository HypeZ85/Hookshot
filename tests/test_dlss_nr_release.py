"""Exercise SR/NR release and retry locking without NGX, Vulkan or a game."""
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
    source = (ROOT/'gpu/vk/vk_dlss.cpp').read_text(encoding='utf-8')
    evaluate = block(source, 'bool evaluate(')
    assert 'release_locked();' in evaluate and '\n            release();' not in evaluate
    fixture = r'''
#include <cassert>
#include <functional>
#include <mutex>
#include <vector>
using NVSDK_NGX_Handle=int;
struct State {
    std::mutex mu;
    NVSDK_NGX_Handle* feature=nullptr;
    unsigned feature_rw=10,feature_rh=10,feature_ow=20,feature_oh=20,min_rw=1,min_rh=1;
    unsigned refused=99;
} state;
State& st() { return state; }
int drops=0,released=0;
std::vector<std::function<void()>> deferred;
void drop_nr() { ++drops; }
void temporal_later(std::function<void()> fn) { deferred.push_back(fn); }
void NVSDK_NGX_VULKAN_ReleaseFeature(NVSDK_NGX_Handle* p) { assert(p); ++released; }
'''
    for marker in ('void release_locked() {', 'void release() {', 'void dlss_retry() {'):
        fixture += block(source, marker)+'\n'
    fixture += r'''
int main() {
    int handle=1;
    state.feature=&handle;
    { // The evaluation failure path already owns the non-recursive mutex.
        std::lock_guard lock(state.mu);
        release_locked();
    }
    assert(drops==1 && !state.feature && state.refused==0 && released==0 && deferred.size()==1);
    assert(!state.feature_rw && !state.feature_rh && !state.feature_ow && !state.feature_oh && !state.min_rw && !state.min_rh);
    deferred.front()(); assert(released==1);
    state.refused=123;
    release(); // Idle/engine-switch path acquires the mutex itself, even without an SR feature.
    assert(drops==2 && state.refused==0 && deferred.size()==1);
    state.refused=456;
    dlss_retry(); assert(state.refused==0);
    assert(state.mu.try_lock()); state.mu.unlock();
}
'''
    with tempfile.TemporaryDirectory(prefix='nr-release-') as folder:
        cpp=Path(folder)/'release.cpp'
        exe=Path(folder)/'release.exe'
        cpp.write_text(fixture,encoding='utf-8')
        subprocess.run([args.cxx,'-std=c++20','-Wall','-Wextra','-Werror','-I'+str(ROOT),str(cpp),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=10)
    print('PASS: locked failure recovery, external release, deferred destruction and retry')

if __name__ == '__main__':
    main()
