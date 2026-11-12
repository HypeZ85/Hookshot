"""Exercise the real independent NR adapter with fake Vulkan/NGX calls, without a game or GPU."""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    source = (ROOT / "gpu/vk/vk_dlss.cpp").read_text(encoding="utf-8")
    start = source.index("bool dlss_nr_chain(")
    adapter = source[start:source.index("\nbool dlss_built()", start)]
    fixture = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include "gpu/vk/dlss_nr_support.h"
using namespace mkwvk;
using VkCommandBuffer=int;
using VkDescriptorType=int;
enum { VK_NULL_HANDLE, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
       VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
       VK_SUCCESS, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_LAYOUT_GENERAL,
       VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, VK_PIPELINE_BIND_POINT_COMPUTE, VK_SHADER_STAGE_COMPUTE_BIT };
struct TemporalImage { int image=1,view=1,format=1; uint32_t width=1280,height=720; };
struct TemporalFrame {
    TemporalImage color,depth,motion,output{2,2,1,1920,1080};
    uint32_t render_w=1280,render_h=720;
    int nr_stage=0;
    bool reset=false,depth_reversed=false;
    float jitter[2]{.25f,-.25f};
};
struct Controls { int stage=0,debug=0,color=100; bool requested=true,native_hdr=false; } nr_controls;
struct State {
    std::mutex mu;
    Controls nr_controls;
    bool nr_stage_after=false,nr_failed=false,nr_running=true;
    int nr_feature=1,nr_inputs=0,nr_inputs_set=0,nr_inputs_layout=0,point=0,nr_last_debug=0;
    TemporalImage color,depth,motion,nr_depth,nr_motion,nr_output{3,3,1,1920,1080},nr_composed{4,4,1,1920,1080};
    float nr_jitter[2]{};
    std::chrono::steady_clock::time_point nr_last_check{};
} state;
State& st() { return state; }
std::mutex nr_controls_mutex;
std::atomic<int> g_nr_compare{-1};
std::atomic<bool> nr_resources{false};
dlss_nr::Issue issue=dlss_nr::Issue::none;
auto dlss_nr_issue() { return issue; }
void nr_issue_set(dlss_nr::Issue v) { issue=v; }
int checks=0,drops=0,allocations=0,dispatches=0,evaluations=0,composites=0,barriers=0;
bool image_ok=true,pipeline_ok=true,evaluate_ok=true;
uint32_t eval_w=0,eval_h=0;
int pushed[5]{};
auto nr_check_locked(bool) { ++checks; return issue; }
void drop_nr() { ++drops; state.nr_feature=0; state.nr_inputs=state.point=0; }
void logf(const char*,...) {}
const int DLSS_NR_INPUTS_COMP_SPV[]={0};
bool make_pipeline(const int*,size_t,const int*,int,int,int&,int&,int& p) { ++allocations; p=pipeline_ok; return pipeline_ok; }
struct VkSamplerCreateInfo { int type,magFilter=0,minFilter=0,addressModeU=0,addressModeV=0,addressModeW=0; };
struct { int device=0; } dev;
int vkCreateSampler(int,const VkSamplerCreateInfo*,void*,int* p) { ++allocations; *p=1; return VK_SUCCESS; }
bool make_image(int,TemporalImage& i,int,uint32_t w,uint32_t h) { ++allocations; i.width=w;i.height=h; return image_ok; }
struct VkDescriptorImageInfo { int sampler,view,layout; };
struct VkWriteDescriptorSet { int type=0,dstBinding=0,descriptorCount=0,descriptorType=0; const VkDescriptorImageInfo* pImageInfo=nullptr; };
void full_barrier(int) { ++barriers; }
void vkCmdBindPipeline(int,int,int) {}
void vkCmdPushDescriptorSetKHR(int,int,int,int,int,VkWriteDescriptorSet*) {}
void vkCmdPushConstants(int,int,int,int,size_t n,const void* p) { assert(n==sizeof pushed); std::memcpy(pushed,p,n); }
void vkCmdDispatch(int,uint32_t,uint32_t,int) { ++dispatches; }
bool neural_render(int,uint32_t w,uint32_t h,bool& reset) {
    ++evaluations; eval_w=w;eval_h=h;
    reset=true;
    if (!evaluate_ok) { state.nr_failed=true; nr_issue_set(dlss_nr::Issue::render_failed); drop_nr(); return false; }
    return g_nr_compare>=0 ? g_nr_compare!=0 : state.nr_controls.requested;
}
bool nr_composite(int,uint32_t,uint32_t,bool,const TemporalImage&) { ++composites; return true; }
''' + adapter + r'''
int frame_stage=0;
void fresh(int stage=0) {
    frame_stage=stage;
    nr_controls={}; nr_controls.stage=stage;
    state.nr_stage_after=stage!=0; state.nr_failed=false; state.nr_feature=1;state.nr_inputs=state.point=0;
    state.nr_last_check={}; state.nr_last_debug=0; state.nr_running=true;
    issue=dlss_nr::Issue::none; g_nr_compare=-1; nr_resources=true;
    checks=drops=allocations=dispatches=evaluations=composites=barriers=0;
    image_ok=pipeline_ok=evaluate_ok=true;
}
bool run(TemporalFrame& f,bool after,TemporalImage& out,bool& debug) { f.nr_stage=frame_stage; return dlss_nr_chain(1,f,after,false,out,debug); }
int main() {
    TemporalFrame f;TemporalImage out; bool debug=false;
    fresh(); assert(!run(f,true,out,debug)); assert(!allocations&&!evaluations);
    assert(run(f,false,out,debug)); assert(eval_w==1280&&eval_h==720&&f.reset);
    assert(state.color.view==f.color.view&&state.nr_jitter[0]==.25f);
    assert(pushed[0]==1280&&pushed[2]==1280&&pushed[4]==0);
    assert(barriers>=2&&dispatches==1&&out.view==state.nr_output.view);
    nr_controls.stage=1; // A UI change between Before and After cannot add a second evaluation.
    assert(!run(f,true,out,debug)); assert(evaluations==1);
    fresh(1);f.reset=false;f.depth_reversed=true;
    assert(run(f,true,out,debug)); assert(!f.reset&&eval_w==1920&&eval_h==1080);
    assert(state.color.view==f.output.view&&state.nr_jitter[0]==0);
    assert(pushed[0]==1920&&pushed[1]==1080&&pushed[2]==1280&&pushed[3]==720&&pushed[4]==1);
    fresh();nr_controls.requested=false;
    assert(!run(f,false,out,debug));assert(!allocations&&!dispatches&&evaluations==1);
    fresh();g_nr_compare=0;assert(!run(f,false,out,debug));assert(!allocations&&!dispatches);
    fresh();nr_controls.requested=false;g_nr_compare=1;assert(run(f,false,out,debug));
    fresh();issue=dlss_nr::Issue::missing_runtime;
    for (int n=0;n<100;++n) assert(!run(f,false,out,debug));
    assert(checks==1&&!allocations&&!dispatches&&!evaluations);
    fresh();nr_controls.native_hdr=true;assert(!run(f,false,out,debug));assert(!allocations&&!dispatches);
    fresh();f.depth.view=0;assert(!run(f,false,out,debug));assert(drops==1&&!allocations); f.depth.view=1;
    fresh();pipeline_ok=false;assert(!run(f,false,out,debug));assert(drops==1&&!dispatches);
    for (int n=0;n<100;++n) assert(!run(f,false,out,debug));assert(allocations==1);
    fresh();image_ok=false;assert(!run(f,false,out,debug));assert(drops==1&&!dispatches);
    fresh();evaluate_ok=false;nr_controls.debug=3;
    assert(!run(f,false,out,debug));assert(drops==1&&!composites);
    fresh(1);nr_controls.debug=4;assert(run(f,true,out,debug));
    assert(debug&&!evaluations&&composites==1&&out.view==state.nr_composed.view);
    fresh();nr_controls.debug=5;assert(run(f,false,out,debug));
    assert(debug&&!evaluations&&out.view==f.output.view);
    fresh(1);nr_controls.color=50;assert(run(f,true,out,debug));assert(out.view==state.nr_composed.view);
}
'''
    with tempfile.TemporaryDirectory(prefix="nr-stage-") as tmp:
        cpp = Path(tmp) / "stage.cpp"
        exe = Path(tmp) / "stage.exe"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-I", str(ROOT), str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
