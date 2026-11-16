"""World-space bounce uses geometry, visibility and lighting independent of camera."""
from pathlib import Path
import shutil
import subprocess
import struct
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "tools"))
import scene_world_gi_fixture as fixture
import scene_pbr_variants as variants

HARNESS = r'''
#include "scene_world_gi.hpp"
#include <iostream>
#include <iterator>
#include <numeric>
#include <string>
int main(int argc, char** argv) {
    if (argc!=2) return 3;
    std::string data((std::istreambuf_iterator<char>(std::cin)),{});
    t3render::pbr::Frame frame;
    if (!t3render::pbr::ParsePacket(reinterpret_cast<const std::uint8_t*>(data.data()),data.size(),frame)) return 4;
    std::vector<std::uint32_t> draws(frame.draws.size()), casters(frame.offline_casters.size());
    std::iota(draws.begin(),draws.end(),0u); std::iota(casters.begin(),casters.end(),0u);
    const std::string mode(argv[1]);
    if (mode=="spot" || mode=="away") {
        frame.lights[0].intensity=10; frame.lights[0].direction[2]=mode=="spot" ? 1 : -1;
        frame.lights[0].cos_inner=.6f; frame.lights[0].cos_outer=.3f;
    }
    if (mode=="parallel") {
        const auto original=frame.draws[0];
        for (unsigned i=0; i<150; ++i) {
            auto copy=original; copy.first_vertex=static_cast<std::uint32_t>(frame.vertices.size());
            for (unsigned v=0; v<original.vertex_count; ++v) {
                auto vertex=frame.vertices[original.first_vertex+v]; vertex.y+=3+i*.05f;
                frame.vertices.push_back(vertex);
            }
            draws.push_back(static_cast<std::uint32_t>(frame.draws.size())); frame.draws.push_back(copy);
        }
    }
    if (mode=="budget") frame.draws[0].vertex_count=125001;
    std::vector<t3render::world_gi::Vec> first, second; std::string error;
    if (!t3render::world_gi::Bake(frame,draws,casters,first,error)) {
        if (mode=="budget" && error=="world GI exceeds vertex/sample budget") return 0;
        std::cerr<<error<<"\n"; return 5;
    }
    if (mode=="budget") return 6;
    frame.view[12]+=2; frame.camera[0]+=2;
    if (!t3render::world_gi::Bake(frame,draws,casters,second,error) || first.size()!=second.size() ||
        std::memcmp(first.data(),second.data(),first.size()*sizeof(first[0]))) return 7;
    std::cout<<first[0].x<<" "<<first[0].y<<" "<<first[0].z<<"\n";
}
'''


class WorldGiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            raise unittest.SkipTest("host C++ compiler required")
        cls.temp = tempfile.TemporaryDirectory(prefix="t3render-world-gi-parser-")
        cls.binary = Path(cls.temp.name)/"bake"
        subprocess.run([compiler,"-std=c++17","-O2","-Wall","-Wextra","-Werror","-I",str(HERE.parent/"src"),
                        "-x","c++","-o",str(cls.binary),"-"],input=HARNESS,text=True,
                       check=True,capture_output=True,timeout=60)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def bake(self, raw, mode="default"):
        result=subprocess.run([str(self.binary),mode],input=raw,capture_output=True,timeout=20)
        self.assertEqual(result.returncode,0,result.stderr.decode())
        return tuple(map(float,result.stdout.split()))

    def test_offscreen_emission_is_blocked_in_world_space(self):
        lit=self.bake(fixture.fixture())
        blocked=self.bake(fixture.fixture(blocked=True))
        shadow_only=self.bake(fixture.fixture(blocked=True,shadow_only=True))
        off=self.bake(fixture.fixture(emitting=False))
        self.assertGreater(lit[0],.1)
        self.assertEqual(lit[1:],(0,0))
        self.assertLess(blocked[0],.0001)
        self.assertLess(shadow_only[0],.0001)
        self.assertEqual(off,(0,0,0))

    def test_direct_spot_orientation_and_work_budget(self):
        source=fixture.fixture(emitting=False)
        self.assertGreater(self.bake(source,"spot")[0],.001)
        self.assertEqual(self.bake(source,"away"),(0,0,0))
        self.assertEqual(self.bake(source,"budget"),())

    def test_parallel_bake_is_camera_independent_and_deterministic(self):
        self.assertGreater(self.bake(fixture.fixture(),"parallel")[0],0)

    def test_emissive_opaque_blocker_still_blocks_other_light(self):
        packet=bytearray(fixture.fixture(blocked=True))
        offset=variants.packet_signatures(bytes(packet))[0][2][1]
        struct.pack_into("<3f",packet,offset-12,0,2,0)
        self.assertLess(self.bake(bytes(packet))[0],.0001)


if __name__=="__main__":
    unittest.main()
