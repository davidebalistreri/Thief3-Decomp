#!/usr/bin/env python3
"""Generate an offscreen renderer fixture from a recorded bedroom pose.

The fixture is a reconstruction: generic actor/resource associations are
catalog candidates, not recovered GPU bindings. Its OFF image is simulated.
"""
import argparse
import hashlib
import json
import math
import re
from pathlib import Path

import compare_live_light_diagnostics as diagnostics
import replay_scene_live_capture as replay
import scene_material_overrides
import trigger_live_v01


def words(values, count):
    if len(values) != count or any(type(v) is not int or not 0 <= v <= 0xffffffff for v in values):
        raise ValueError("invalid recorded uint32 array")
    return ",".join(f"{v}u" for v in values)


def string(value):
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9_:.-]+", value):
        raise ValueError("invalid recorded identifier")
    return '"' + value + '"'


def float_words(values):
    if not isinstance(values, list) or len(values) != 16:
        raise ValueError("invalid recorded matrix")
    return words([replay.float_bits(v) for v in values], 16)


def telemetry_frame(path, frame):
    matches = []
    with path.open() as stream:
        for line in stream:
            row = json.loads(line)
            if row.get("event") == "frame" and row.get("frame") == frame:
                matches.append(row)
    if len(matches) != 1 or not matches[0].get("light_raw_available"):
        raise ValueError("missing or ambiguous lighting telemetry frame")
    return matches[0]


def actor_candidates(events):
    observations = [r for r in events if r["event"] == "scene_bridge_observation" and r["known_caller"]]
    if not observations:
        raise ValueError("recorded visible list is missing")
    entries = {e["address"]: e for e in observations[0]["entries"]
               if e["owner_name"] and e["object_matrix_bits"]}
    scopes = {r["ticket"]: r for r in events if r["event"] == "scene_bridge_submission_begin" and r["stage"] == 1}
    stable = {r["ticket"] for r in events if r["event"] == "scene_bridge_submission_end" and r["stable"]}
    successful = {r["op_id"] for r in events if r["event"] == "draw_result" and r["hr"] == 0}
    actors = {}
    for draw in events:
        if (draw["event"] != "draw" or draw["op_id"] not in successful or
                draw.get("call") != "DrawIndexedPrimitive" or draw.get("primitive_type") != 4):
            continue
        scope = scopes.get(draw.get("submission_ticket"))
        if not scope or scope["ticket"] not in stable:
            continue
        entry = entries.get(scope["element"])
        if (not entry or scope["owner"] != entry["owner_candidate"] or
                scope["matrix_bits"] != entry["object_matrix_bits"]):
            continue
        name = entry["owner_name"] + "__" + str((entry["owner_fname"] >> 16) - 1)
        # Candlesticks are inserted by the same viewer preview as the live run.
        if name.startswith("D_384__"):
            continue
        string(name)
        if name not in actors:
            actors[name] = (entry["object_matrix_bits"], set())
        if actors[name][0] != entry["object_matrix_bits"]:
            raise ValueError("ambiguous actor transform")
        actors[name][1].add(scope["section"])
    return actors


def link_source(name, link):
    if link["status"] != "candidate" or link["transform_bits"] is None:
        raise ValueError("benchmark requires two recorded candle links")
    enabled = link.get("enabled_candidate")
    if enabled not in (True, False, None):
        raise ValueError("invalid candle enabled observation")
    state = "Unknown" if enabled is None else "On" if enabled else "Off"
    inherited = "true" if link.get("enabled_inherited") is True else "false"
    return (f"batch->{name}.status=t3render::scene_bridge::CandleLinkStatus::Candidate;"
            f"batch->{name}.factor_8c_bits={words([link['field_8c_bits']], 1)};"
            f"batch->{name}.transform_available=true;batch->{name}.transform_bits={{{words(link['transform_bits'], 16)}}};"
            f"batch->{name}.enabled.status=t3render::scene_bridge::CandleEnabledStatus::{state};"
            f"batch->{name}.enabled.inherited={inherited};\n")


def light_source(item):
    result = ['{RawLightItem r;', f"r.candidate.id={string(item['id'])};",
              f"r.candidate.generation={replay.uint(item['generation'], 64)};r.candidate.binding=BindingEvidence::Candidate;"]
    for field in ("24", "34", "68"):
        result.append(f"r.field_{field}={{{words(item['field_'+field+'_bits'], 3)}}};")
    for field in ("58", "5c", "60", "64", "74", "78", "7c", "88", "8c", "90"):
        value = item["field_" + field + ("_bits" if field in ("74", "78", "7c", "88", "8c", "90") else "")]
        result.append(f"r.field_{field}={words([value], 1)};")
    result.append("Vec3 p{};std::memcpy(p.data(),r.field_34.data(),sizeof(float)*3);"
                  "r.candidate.position={p,BindingEvidence::Candidate};"
                  "f.lights.push_back(r.candidate);batch->items.push_back(std::move(r));}\n")
    return "".join(result)


HEAD = r'''#include "scene_live_catalog_win.hpp"
#include "scene_live_dx12.hpp"
#include "scene_live_lighting.hpp"
#include "scene_live_wall_displacement.hpp"
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iostream>
using namespace t3render::live;
#define CHECK(x) do { if (!(x)) {std::cerr<<"failed line "<<__LINE__<<" " #x<<"\n";return 1;} } while(0)
int wmain(int argc,wchar_t** argv) {
CHECK(argc>=6 && argc<=10);LoadedCatalog c;CHECK(LoadCatalogFile(argv[1],c));
CHECK(c.bedroom_materials && c.bed_override);auto epoch=c.residency->epochs();
FrameInput f;f.map_epoch=epoch.map;f.device_epoch=epoch.device;f.render_pass=0;f.stage=1;
f.synchronized=true;f.conventions=c.conventions;f.provenance.origin=FrameOrigin::Synthetic;
f.provenance.session_id="offline-benchmark-reconstruction";
'''

TAIL = r'''
f.light_coverage=LightCoverage::Partial;CHECK(c.residency->Publish(f));auto snap=c.residency->current();CHECK(snap);
RawLightFrame raw;raw.map_epoch=snap->map_epoch;raw.device_epoch=snap->device_epoch;raw.continuity=snap->continuity;
raw.frame_id=snap->frame_id;raw.provenance=snap->provenance;raw.camera=snap->camera;raw.batch=batch;
CHECK(RawLightFrameMatches(raw,*snap));
const int mode=_wtoi(argv[5]);CHECK(mode>=0 && mode<=3);
bool gi=false,warm_only=false,cool_only=false,source_selected=false;unsigned bounce_sources=3;
CHECK(!mode || argc>=7);
for(int i=mode ? 7 : 6;i<argc;++i) {
    if(std::wcscmp(argv[i],L"gi")==0 && !gi)gi=true;
    else if(std::wcscmp(argv[i],L"warm")==0 && !warm_only && !cool_only)warm_only=true;
    else if(std::wcscmp(argv[i],L"cool")==0 && !warm_only && !cool_only)cool_only=true;
    else if(std::wcscmp(argv[i],L"wall")==0 && !source_selected){bounce_sources=1;source_selected=true;}
    else if(std::wcscmp(argv[i],L"door")==0 && !source_selected){bounce_sources=2;source_selected=true;}
    else if(std::wcscmp(argv[i],L"none")==0 && !source_selected){bounce_sources=0;source_selected=true;}
    else CHECK(false);
}
if(mode) {
    auto file=_wfopen(argv[6],L"rb");CHECK(file);
    auto materials=std::make_shared<BedroomMaterialLibrary>(*c.bedroom_materials);
    const auto width=materials->materials[0].channels[3].mips.front().width;
    CHECK(width && width<=1024 && !(width&(width-1)));t3render::scene::Texture height;
    for(auto size=width;;size/=2) {
        t3render::scene::Mip mip{size,size,std::vector<std::uint8_t>(std::size_t(size)*size*4)};
        CHECK(std::fread(mip.rgba.data(),1,mip.rgba.size(),file)==mip.rgba.size());
        height.mips.push_back(std::move(mip));if(size==1)break;
    }
    CHECK(std::fgetc(file)==EOF && std::fclose(file)==0);
    materials->wall_height=std::move(height);materials->wall_height_source_sha256=HEIGHT_SOURCE_SHA256;
    c.bedroom_materials=std::move(materials);
}
WallDisplacementCache displacement;
auto make_draw=[&](bool on,ResidentFlatFrame& draw) {
    if(!BuildResidentFlatFrame(*snap,draw,c.bed_override.get(),true,mode>0,nullptr,
                               bool(c.bedroom_materials->original_materials)))return false;
    if(mode==1)for(auto& d:draw.draws)if(d.wall_relief==2)d.wall_relief=1;
    if(mode==3){WallDisplacementStats stats;if(!displacement.Apply(*snap,draw,c.bedroom_materials,stats))return false;
        std::cout<<"wall_source="<<stats.source_triangles<<" wall_displaced="<<stats.displaced_triangles<<"\n";}
    AppendCandlePreview(*snap,raw,on && !cool_only,draw);
    AppendCandlePreview(*snap,raw,!cool_only,draw,6);return true;
};
auto gpu=FlatD3D12::Create(nullptr,128u*1024u*1024u,c.bedroom_materials,false,gi);CHECK(gpu);
std::vector<std::uint8_t> pixels;
auto write=[&](const wchar_t* path){auto out=_wfopen(path,L"wb");if(!out)return false;
    std::fprintf(out,"P6\n%u %u\n255\n",snap->camera.width,snap->camera.height);
    for(std::size_t i=0;i<pixels.size();i+=4)if(std::fwrite(&pixels[i],1,3,out)!=3){std::fclose(out);return false;}
    return std::fclose(out)==0;};
ResidentFlatFrame on;CHECK(make_draw(true,on));
CHECK(gpu->PrepareCandlePreview(*snap,cool_only ? nullptr : &raw,on,true,cool_only ? nullptr : &raw,!warm_only,bounce_sources)
      && gpu->Submit() && gpu->Finish(&pixels) && write(argv[2]));
ResidentFlatFrame off;CHECK(make_draw(false,off));
CHECK(gpu->PrepareCandlePreview(*snap,nullptr,off,true,cool_only ? nullptr : &raw,!warm_only,bounce_sources)
      && gpu->Submit() && gpu->Finish(&pixels) && write(argv[4]));
const auto start=GetTickCount64();
for(unsigned i=0;i<3;++i)
    CHECK(gpu->PrepareCandlePreview(*snap,cool_only ? nullptr : &raw,on,true,cool_only ? nullptr : &raw,!warm_only,bounce_sources)
          && gpu->Submit() && gpu->Finish(&pixels));
const auto elapsed=GetTickCount64()-start;
ResidentFlatFrame flat;CHECK(BuildResidentFlatFrame(*snap,flat));
CHECK(gpu->PrepareResident(flat,true) && gpu->Submit() && gpu->Finish(&pixels) && write(argv[3]));
std::cout<<"offline_live_renderer=ok meshes="<<snap->meshes.size()<<" bsp="<<snap->bsp.size()
         <<" draws="<<on.draws.size()<<" off=simulated gi="<<gi
         <<" three_readback_frames_ms="<<elapsed<<"\n";return 0;}
'''


def generate(trace, telemetry, catalog, trace_frame, light_frame, height_digest=None):
    rows, digest, _ = replay.read_capture(trace, [trace_frame])
    report = replay.replay(trace, catalog, [trace_frame])
    selected = report["frames"][0]
    if selected.get("result") != "published":
        raise ValueError("captured replay did not publish the benchmark frame")
    light = telemetry_frame(telemetry, light_frame)
    reference = telemetry_frame(telemetry, trace_frame)
    with trace.open() as stream:
        header = json.loads(stream.readline())
    if (light["map"] != selected["map"] or light["map_sha256"] != selected["map_sha256"] or
            light["width"] != selected["camera"]["viewport"]["width"] or
            light["height"] != selected["camera"]["viewport"]["height"] or
            light["session_id"] != reference["session_id"] or
            light["session_id"] != header.get("live_session_id") or
            any(light.get(k) != reference.get(k) for k in
                ("map", "map_sha256", "map_epoch", "device_epoch", "continuity", "viewport"))):
        raise ValueError("trace and lighting telemetry disagree on scene or viewport")
    if any(replay.float_bits(a) != replay.float_bits(b) for a, b in zip(
            selected["camera"]["view"], reference["view"])):
        raise ValueError("replayed camera differs from reference telemetry")
    if any(replay.float_bits(light["view"][i]) != replay.float_bits(reference["view"][i])
           for i in (0, 1, 2, 4, 5, 6, 8, 9, 10)):
        raise ValueError("benchmark camera rotated between recorded frames")
    if float_words(light["projection"]) != float_words(reference["projection"]):
        raise ValueError("benchmark projection changed between recorded frames")
    reference_position = diagnostics.camera_position(reference["view"])
    light_position = diagnostics.camera_position(light["view"])
    if reference_position is None or light_position is None:
        raise ValueError("invalid recorded camera")
    pose_delta = math.dist(reference_position, light_position)
    if pose_delta >= .5:
        raise ValueError("benchmark camera moved between recorded frames")
    raw = light["light_raw"]
    if raw["candle_link"]["actor_number"] != 14 or raw["other_candle_link"]["actor_number"] != 6:
        raise ValueError("benchmark requires the left candle selected")
    if any(not (state and state['enabled'] and state['factor'] is not None and state['factor'] > 0)
           for state in (trigger_live_v01.candle(light), trigger_live_v01.candle(light, other=True))):
        raise ValueError("benchmark requires both recorded candles on")
    if raw["skipped"] or raw["truncated"] or raw["listed"] != len(raw["items"]):
        raise ValueError("incomplete recorded light list")
    actors = actor_candidates(rows[trace_frame])
    out = [f'#define HEIGHT_SOURCE_SHA256 "{height_digest or ""}"\n', HEAD,
           f"f.frame_id={replay.uint(light_frame, 64)};\n"]
    for field in ("view", "projection"):
        out.append(f"{{const std::uint32_t bits[] = {{{float_words(light[field])}}};"
                   f"std::memcpy(f.camera.{field}.data(),bits,sizeof(bits));}}\n")
    v = light["viewport"]
    out.append(f"f.camera.x={replay.uint(v['x'])};f.camera.y={replay.uint(v['y'])};"
               f"f.camera.width={replay.uint(v['width'])};f.camera.height={replay.uint(v['height'])};"
               f"{{const std::uint32_t bits[]={{{words([replay.float_bits(v['min_depth']), replay.float_bits(v['max_depth'])], 2)}}};"
               "std::memcpy(&f.camera.min_depth,bits,sizeof(float));std::memcpy(&f.camera.max_depth,bits+1,sizeof(float));}"
               "f.camera.viewport_source=ViewportSource::ScopedDraw;\n")
    prefix = "actor:" + selected["map"] + ":" + selected["map_sha256"] + ":"
    for name, (matrix, sections) in actors.items():
        out.append(f"{{auto i=c.binding.static_mesh_actors.find({string(prefix+name)});"
                   "if(i!=c.binding.static_mesh_actors.end()){"
                   "MeshSelection m;m.instance_id=i->first;m.resource_id=i->second.resource->id;"
                   "m.matched_resource=i->second.resource;m.material_ids=i->second.material_ids;"
                   "m.binding=BindingEvidence::Candidate;m.identity_scope=IdentityScope::VisibleList;m.generation=1;"
                   f"const std::uint32_t bits[]={{{words(matrix, 16)}}};std::memcpy(m.transform.data(),bits,sizeof(bits));"
                   f"for(auto sec:std::initializer_list<unsigned>{{{','.join(replay.uint(s) for s in sorted(sections))}}})"
                   "if(sec<m.matched_resource->geometry.sections.size())m.sections.push_back(sec);"
                   f"if(!m.sections.empty())f.meshes.push_back(std::move(m));}}else std::cout<<\"missing={name}\\n\";}}\n")
    for block in selected["bsp"]:
        out.append(f"{{const std::string id={string(block['id'])};std::shared_ptr<const Resource> r;"
                   "for(auto& v:c.binding.bsp)if(v->id==id)r=v;CHECK(r);BspSelection b;b.resource_id=id;"
                   "b.matched_resource=r;b.binding=BindingEvidence::Candidate;b.identity_scope=IdentityScope::VisibleList;"
                   f"b.sections={{{','.join(replay.uint(s) for s in block['sections'])}}};f.bsp.push_back(std::move(b));}}\n")
    out.append(f"auto batch=std::make_shared<RawLightBatch>();batch->listed={replay.uint(raw['listed'])};"
               "batch->candle_actor_number=14;\n")
    out.append(link_source("candle_link", raw["candle_link"]))
    out.append(link_source("other_candle_link", raw["other_candle_link"]))
    out.extend(light_source(item) for item in raw["items"])
    out.append(TAIL)
    evidence = {"source": "offline_reconstruction", "trace_frame": trace_frame, "lighting_frame": light_frame,
                "trace_sha256": digest, "telemetry_sha256": hashlib.sha256(telemetry.read_bytes()).hexdigest(),
                "catalog_sha256": report["catalog_sha256"], "actor_candidates": len(actors),
                "bsp_from_replay": len(selected["bsp"]), "off": "simulated",
                "pose_distance_t3": pose_delta, "wall_height_source_sha256": height_digest,
                "limitations": report["limitations"]}
    return "".join(out), evidence


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trace", required=True, type=Path)
    parser.add_argument("--telemetry", required=True, type=Path)
    parser.add_argument("--catalog", required=True, type=Path)
    parser.add_argument("--trace-frame", required=True, type=int)
    parser.add_argument("--lighting-frame", required=True, type=int)
    parser.add_argument("--output", required=True, type=Path, help="new C++ source file")
    parser.add_argument("--wall-height-materials", type=Path, help="Godot materials directory for offline wall comparison")
    parser.add_argument("--height-output", type=Path, help="new raw mip file used only by the offline fixture")
    args = parser.parse_args()
    if bool(args.wall_height_materials) != bool(args.height_output):
        parser.error("wall-height-materials and height-output must be supplied together")
    if args.output.exists() or (args.height_output and args.height_output.exists()):
        parser.error("output paths must not already exist")
    if args.height_output and args.output.resolve() == args.height_output.resolve():
        parser.error("source and height output must be different files")
    height = (scene_material_overrides.GodotTextures(args.wall_height_materials, 512).resolve("stone_wall_v2", "height")
              if args.wall_height_materials else None)
    source, evidence = generate(args.trace, args.telemetry, args.catalog, args.trace_frame, args.lighting_frame,
                                height["source_sha256"] if height else None)
    with args.output.open("x") as stream:
        stream.write(source)
    if height:
        try:
            payload = b"".join(height["mips"])
            with args.height_output.open("xb") as stream:
                stream.write(payload)
            evidence["wall_height_payload_sha256"] = hashlib.sha256(payload).hexdigest()
        except Exception:
            args.output.unlink()
            raise
    print(json.dumps(evidence, indent=2))


if __name__ == "__main__":
    main()
