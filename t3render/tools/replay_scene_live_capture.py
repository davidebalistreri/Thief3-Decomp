#!/usr/bin/env python3
"""Exercise the live driver with recorded callbacks, as CapturedReplay only.

The temporary fixture contains trace addresses but never writes them to its
report or to SceneIR. This is an offline diagnostic, not a live TDS session.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

import trace_summary

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "ir"))
import scene_ir  # noqa: E402

MAX_CATALOG = 64 * 1024 * 1024
MAX_EVENTS = 10000


def uint(value, width=32):
    if type(value) is not int or not 0 <= value < (1 << width):
        raise ValueError("invalid bounded integer in capture")
    return f"{value}ull" if width == 64 else f"{value}u"


def truth(value):
    if type(value) is not bool:
        raise ValueError("invalid boolean in capture")
    return "true" if value else "false"


def float_bits(value):
    if isinstance(value, bool) or not isinstance(value, (float, int)) or not math.isfinite(value):
        raise ValueError("invalid float in capture")
    try:
        return struct.unpack("<I", struct.pack("<f", value))[0]
    except (OverflowError, struct.error) as exc:
        raise ValueError("float outside float32 range") from exc


def success(value):
    if type(value) is not int or not -(1 << 31) <= value < (1 << 32):
        raise ValueError("invalid HRESULT in capture")
    return truth((value & 0x80000000) == 0)


def fingerprint(stat):
    return stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns


def read_capture(path, requested):
    path = Path(path)
    if not 1 <= len(requested) <= 3 or any(type(x) is not int or x < 0 for x in requested):
        raise ValueError("one to three frame IDs are required")
    if requested != list(range(requested[0], requested[0] + len(requested))):
        raise ValueError("frames must be consecutive and increasing")
    before = path.stat()
    summary = trace_summary.summarize(path)
    after_validation = path.stat()
    selected = {frame: [] for frame in requested}
    header = None
    digest = hashlib.sha256()
    events = 0
    keep = {"frame_begin", "frame_end", "scene_bridge_frame", "scene_bridge_observation",
            "scene_bridge_submission_begin", "scene_bridge_submission_end", "draw", "draw_result", "clear", "clear_result"}
    with path.open("rb") as stream:
        opened = os.fstat(stream.fileno())
        for raw in stream:
            digest.update(raw)
            row = json.loads(raw, object_pairs_hook=trace_summary._object, parse_constant=trace_summary._constant)
            if row.get("event") == "header":
                header = row
            elif row.get("frame") in selected and row.get("event") in keep:
                events += 1
                if events > MAX_EVENTS:
                    raise ValueError("capture exceeds replay event limit")
                if row["event"] == "draw":
                    row = {key: row.get(key) for key in ("event", "frame", "op_id", "thread_id", "submission_ticket",
                           "call", "primitive_type", "start_index", "primitive_count", "min_vertex_index", "num_vertices",
                           "viewport", "transforms")}
                selected[row["frame"]].append(row)
        finished = os.fstat(stream.fileno())
    if len({fingerprint(s) for s in (before, after_validation, opened, finished, path.stat())}) != 1:
        raise ValueError("capture changed during validation")
    capability = (header or {}).get("scene_bridge") or {}
    if (capability.get("install_status") != "installed" or
            capability.get("submission_install_status") != "installed" or
            capability.get("reentry_detection") != "saturating_counter_v1"):
        raise ValueError("capture lacks the installed scoped probe or reentry evidence")
    if any(sum(item["frame"] == frame for item in summary["frames"]) != 1 for frame in requested):
        raise ValueError("requested frame is missing or ambiguous")
    device = None
    thread = None
    for frame, rows in selected.items():
        starts = [r for r in rows if r["event"] == "frame_begin"]
        ends = [r for r in rows if r["event"] == "frame_end"]
        windows = [r for r in rows if r["event"] == "scene_bridge_frame"]
        if len(starts) != 1 or len(ends) != 1 or len(windows) != 1:
            raise ValueError("frame boundaries are missing or ambiguous")
        window = windows[0]
        current_thread = window.get("present_thread")
        if (window.get("status") != "synchronized" or window.get("previous_present_observed") is not True or
                window.get("probe_reentered") is not False or window.get("reset_seen") is not False or
                window.get("reset_pending") is not False or ends[0].get("had_reset") is not False or
                window.get("submission_pending") != 0 or window.get("submission_dropped") != 0 or
                not trace_summary._uint(window.get("submission_started")) or not window.get("submission_started") or
                window.get("submission_completed") != window.get("submission_started") or
                not trace_summary._uint(current_thread) or not current_thread or
                window.get("previous_present_thread") != current_thread or window.get("probe_thread") != current_thread or
                window.get("draw_thread") != current_thread):
            raise ValueError("frame is not a complete same-thread Present window")
        if device is None:
            device, thread = starts[0].get("device"), current_thread
        if (not isinstance(device, str) or not re.fullmatch(r"0x[0-9a-fA-F]{1,8}", device) or not int(device, 16) or
                starts[0].get("device") != device or current_thread != thread):
            raise ValueError("device or Present thread changed")
        for row in rows:
            if row["event"] in ("scene_bridge_observation", "scene_bridge_submission_begin",
                                "scene_bridge_submission_end", "draw") and row.get("thread_id") != thread:
                raise ValueError("recorded callback thread differs from Present")
    return selected, digest.hexdigest(), thread


class Emitter:
    def __init__(self):
        self.lines = []

    def add(self, line):
        self.lines.append(line)

    def number(self, target, value):
        self.add(f"{target}={uint(value)};")

    def boolean(self, target, value):
        self.add(f"{target}={truth(value)};")

    def value(self, target, value):
        if value is not None:
            self.add(f"{target}={{{uint(value)},true}};")

    def text(self, target, value, available, capacity):
        if value is None:
            return
        if not isinstance(value, str) or len(value) >= capacity or not all(32 <= ord(c) < 127 for c in value):
            raise ValueError("invalid bounded text in capture")
        # Hex escapes prevent trace text from becoming generated C++ syntax.
        literal = '"' + ''.join(f"\\x{ord(c):02x}" for c in value) + '"'
        self.add(f"{available}=true;std::memcpy({target},{literal},{len(value) + 1});")

    def array(self, target, values, count, available=None):
        if values is None:
            return
        if not isinstance(values, list) or len(values) != count:
            raise ValueError("invalid bounded array in capture")
        if available:
            self.add(f"{available}=true;")
        payload = ','.join(uint(value) for value in values)
        self.add(f"{{const std::uint32_t a[{count}]={{{payload}}};std::memcpy({target},a,sizeof(a));}}")

    def entry(self, target, value):
        self.number(target + ".list_index", value["list_index"])
        for key in ("address", "vtable", "type", "flags", "owner_candidate", "owner_object_index", "owner_fname", "owner_class", "owner_backlink"):
            self.value(target + "." + key, value.get(key))
        for key in ("owner_table_member", "owner_stable"):
            self.boolean(target + "." + key, value[key])
        self.text(target + ".owner_name", value.get("owner_name"), target + ".owner_name_available", 64)
        for key, count, available in (("owner_location_bits", 3, "owner_location_available"),
                                      ("owner_rotation_bits", 3, "owner_rotation_available"),
                                      ("object_matrix_bits", 16, "object_matrix_available")):
            self.array(target + "." + key, value.get(key), count, target + "." + available)

    def named(self, target, value):
        for key in ("address", "index", "fname", "outer"):
            self.value(target + "." + key, value.get(key))
        for key in ("member", "stable"):
            self.boolean(target + "." + key, value[key])
        self.text(target + ".name", value.get("name"), target + ".name_available", 64)

    def sections(self, target, values, mesh):
        if len(values) > 16:
            raise ValueError("too many captured sections")
        for index, section in enumerate(values):
            item = f"{target}[{index}]"
            if mesh:
                self.text(item + ".name", section["name"], item + ".name_available", 128)
            self.text(item + ".material_name", section["material_name"], item + ".material_name_available", 128)
            for key in ("triangles", "first_vertex", "vertex_count", "first_index", "index_count", "unknown"):
                self.number(item + "." + key, section[key])

    def observation(self, value, index):
        self.add(f"ProbeSnapshot probe{index}(){{ProbeSnapshot p{{}};")
        for key in ("thread_id", "self", "caller", "entries_attempted"):
            self.number("p." + key, value[key])
        for key in ("known_caller", "pass_stable", "object_table_complete", "entries_truncated"):
            self.boolean("p." + key, value[key])
        for key in ("pass", "count", "array"):
            self.value("p." + key, value.get(key))
        camera = value["current_camera"]
        self.value("p.current_camera.address", camera.get("address"))
        self.boolean("p.current_camera.stable", camera["stable"])
        for field, available in (("derived_matrix_bits", "derived_matrix_available"), ("projection_bits", "projection_available")):
            self.array("p.current_camera." + field, camera.get(field), 16, "p.current_camera." + available)
        entries = value["entries"]
        if len(entries) > 128 or len(entries) != value["entries_attempted"]:
            raise ValueError("invalid captured entry count")
        for i, entry in enumerate(entries):
            self.entry(f"p.entries[{i}]", entry)
        target = value["target"]
        for key in ("object_scan_complete", "list_scan_complete", "object_scan_truncated", "list_scan_truncated", "outer_truncated"):
            self.boolean("p.target." + key, target[key])
        for key in ("objects_scanned", "actors_named_target", "list_scanned", "list_occurrences", "outer_count"):
            self.number("p.target." + key, target[key])
        if target["entry"] is not None:
            self.add("p.target.entry_available=true;")
            self.entry("p.target.entry", target["entry"])
        self.named("p.target.owner_class", target["owner_class"])
        if len(target["outer"]) > 4:
            raise ValueError("too many captured outers")
        for i, outer in enumerate(target["outer"]):
            self.named(f"p.target.outer[{i}]", outer)
        asset = target["asset"]
        statuses = {"unknown": "Unknown", "unreadable": "Unreadable", "descriptor_unsupported": "DescriptorUnsupported",
                    "local_property_missing": "LocalPropertyMissing", "property_malformed": "PropertyMalformed",
                    "object_not_unique": "ObjectNotUnique", "proxy_mismatch": "ProxyMismatch",
                    "resource_mismatch": "ResourceMismatch", "candidate": "Candidate"}
        if asset["status"] not in statuses:
            raise ValueError("invalid asset status")
        self.add("p.target.asset.status=AssetStatus::" + statuses[asset["status"]] + ";")
        for key in ("descriptor_match", "local_property_found", "proxy_live_member", "proxy_vtable_match", "resource_vtable_match", "stable"):
            self.boolean("p.target.asset." + key, asset[key])
        self.text("p.target.asset.name", asset["name"], "p.target.asset.name_available", 128)
        if asset.get("audit") is not None:
            audit = asset["audit"]
            if len(audit["chain"]) > 256:
                raise ValueError("asset audit exceeds replay limit")
            self.add("p.target.asset.audit.available=true;")
            for key in ("proxy", "resource"):
                self.number("p.target.asset.audit." + key, audit[key])
            self.number("p.target.asset.audit.chain_count", len(audit["chain"]))
        node = target["mesh_node"]
        for key in ("pool_scan_complete", "pool_scan_truncated", "stable", "sections_truncated"):
            self.boolean("p.target.mesh_node." + key, node[key])
        for key in ("nodes_scanned", "occurrences"):
            self.number("p.target.mesh_node." + key, node[key])
        for key in ("address", "vtable", "name_address", "section_array", "instance_materials"):
            self.value("p.target.mesh_node." + key, node[key])
        self.text("p.target.mesh_node.name", node["name"], "p.target.mesh_node.name_available", 128)
        if node["counts"] is not None:
            self.add("p.target.mesh_node.counts_available=true;")
            for key, field in (("vertices", "vertex_count"), ("triangles", "triangle_count"), ("indices", "index_count"), ("sections", "section_count")):
                self.number("p.target.mesh_node." + field, node["counts"][key])
        self.array("p.target.mesh_node.bounds_bits", node["bounds_bits"], 6, "p.target.mesh_node.bounds_available")
        if node["sections"] is not None:
            self.add("p.target.mesh_node.sections_available=true;")
            self.number("p.target.mesh_node.sections_copied", len(node["sections"]))
            self.sections("p.target.mesh_node.sections", node["sections"], True)
        witness = value.get("map_witness")
        if witness is not None:
            self.add("p.map_witness.available=true;")
            for key in ("list_stable", "object_table_complete"):
                self.boolean("p.map_witness." + key, witness[key])
            self.entry("p.map_witness.entry", witness["entry"])
            self.value("p.map_witness.owner_outer", witness["owner_outer"])
            self.named("p.map_witness.owner_class", witness["owner_class"])
            self.named("p.map_witness.outer", witness["outer"])
        bsp = value["bsp"]
        for key in ("renderer", "source", "blocks"):
            self.value("p.bsp." + key, bsp[key])
        for key in ("block_count", "type5_seen", "member_count"):
            self.number("p.bsp." + key, bsp[key])
        if len(bsp["candidates"]) > 16:
            raise ValueError("too many BSP candidates")
        self.number("p.bsp.candidates_copied", len(bsp["candidates"]))
        for key in ("source_stable", "candidates_truncated"):
            self.boolean("p.bsp." + key, bsp[key])
        for i, block in enumerate(bsp["candidates"]):
            part = f"p.bsp.candidates[{i}]"
            for key in ("list_index", "source_index"):
                self.number(part + "." + key, block[key])
            for key in ("address", "section_array"):
                self.value(part + "." + key, block[key])
            for key in ("stable", "sections_truncated"):
                self.boolean(part + "." + key, block[key])
            if block["counts"] is not None:
                self.add(part + ".metadata_available=true;")
                for key, field in (("vertices", "vertex_count"), ("triangles", "triangle_count"), ("indices", "index_count"), ("sections", "section_count")):
                    self.number(part + "." + field, block["counts"][key])
            for key, length in (("bounds_bits", 6), ("transform_bits", 16)):
                self.array(part + "." + key, block[key], length)
            if block["sections"] is not None:
                self.add(part + ".sections_available=true;")
                self.number(part + ".sections_copied", len(block["sections"]))
                self.sections(part + ".sections", block["sections"], False)
        self.add("return p;}")

    def submission(self, value, index):
        self.add(f"SubmissionSample scope{index}(){{SubmissionSample s{{}};s.valid=true;")
        for key in ("self", "pass", "element", "vtable", "type", "owner", "owner_index", "owner_fname", "backlink", "mesh_node",
                    "asset_proxy", "asset_resource", "bsp_source", "bsp_blocks", "bsp_index"):
            self.number("s." + key, value[key])
        self.array("s.matrix", value["matrix_bits"], 16)
        self.text("s.asset_name", value["asset_name"], "s.asset_available", 128)
        self.add("return s;}")

    def draw(self, value, index):
        self.add(f"DrawObservation state{index}(){{")
        transforms, viewport = value.get("transforms") or {}, value.get("viewport")
        world = transforms.get("world") or []
        if (not viewport or len(transforms.get("view") or []) != 16 or
                len(transforms.get("projection") or []) != 16 or not world or len(world[0] or []) != 16):
            self.add("return {};}")
            return
        self.add("DrawObservation s{};s.available=true;")
        for name, values in (("view", transforms["view"]), ("projection", transforms["projection"]), ("world", world[0])):
            self.array("s." + name + ".data()", [float_bits(v) for v in values], 16)
        self.add("s.viewport={" + ','.join(uint(viewport[k]) for k in ("x", "y", "width", "height")) + ",0,0};")
        for source, target in (("min_z", "min_depth"), ("max_z", "max_depth")):
            self.add(f"{{const std::uint32_t b={uint(float_bits(viewport[source]))};std::memcpy(&s.viewport.{target},&b,4);}}")
        self.add("return s;}")


CPP_HEAD = r'''#include "scene_live_driver.hpp"
#include "scene_live_flat.hpp"
#include <cstring>
#include <iomanip>
#include <iostream>
#include <unordered_map>
#include <vector>
using namespace t3render::scene_bridge;
using namespace t3render::live;
bool digest(const std::uint8_t*,std::size_t,std::uint8_t* output,void* context) noexcept {
    std::memcpy(output,context,32);return true;
}
struct ScopeRecord {std::uint64_t ticket;SubmissionSample sample;};
struct DrawRecord {DrawStart start;DrawObservation state;ThreadToken thread;};
template<class T> void array(const T& values) {
    std::cout<<'[';bool first=true;for (const auto& value:values) {
        if (!first) std::cout<<',';first=false;std::cout<<value;
    } std::cout<<']';
}
void report(unsigned long long frame,WindowResult result,const SceneDriver& driver) {
    const auto current=driver.current();const auto stats=driver.stats();
    std::cout<<"{\"trace_frame\":"<<frame<<",\"result\":"<<std::quoted(
        result==WindowResult::Published ? "published" : result==WindowResult::Primed ? "primed" : "rejected")
        <<",\"selected_probes\":"<<stats.selected_probes<<",\"bed_scopes\":"<<stats.bed_scopes
        <<",\"bsp_scopes\":"<<stats.bsp_scopes<<",\"draw_geometry_bad\":"<<stats.draw_geometry_bad
        <<",\"draw_camera_bad\":"<<stats.draw_camera_bad<<",\"draw_world_bad\":"<<stats.draw_world_bad
        <<",\"draw_viewport_bad\":"<<stats.draw_viewport_bad<<",\"losses\":"<<stats.losses
        <<",\"last_rejection\":"<<stats.last_rejection;
    if (current) {
        ResidentFlatFrame flat;const bool valid=BuildResidentFlatFrame(*current,flat);
        std::uint64_t triangles=0;for (const auto& d:flat.draws) triangles+=d.index_count/3;
        std::cout<<",\"source\":\"captured_replay\",\"replay_frame\":"<<current->frame_id
            <<",\"map\":"<<std::quoted(current->map)<<",\"map_sha256\":"<<std::quoted(current->map_sha256)
            <<",\"flat_valid\":"<<(valid?"true":"false")<<",\"flat_draws\":"<<flat.draws.size()
            <<",\"flat_triangles\":"<<triangles<<",\"camera\":{\"view\":";
        array(current->camera.view);std::cout<<",\"projection\":";array(current->camera.projection);
        std::cout<<",\"viewport\":{\"x\":"<<current->camera.x<<",\"y\":"<<current->camera.y
            <<",\"width\":"<<current->camera.width<<",\"height\":"<<current->camera.height
            <<",\"min_depth\":"<<current->camera.min_depth<<",\"max_depth\":"<<current->camera.max_depth<<"}},\"meshes\":[";
        bool first=true;for (const auto& mesh:current->meshes) {
            if (!first) std::cout<<',';first=false;const auto& s=mesh.selection;
            std::cout<<"{\"id\":"<<std::quoted(s.instance_id)<<",\"resource_id\":"<<std::quoted(s.resource_id)
                <<",\"generation\":"<<s.generation<<",\"sections\":";array(s.sections);
            std::cout<<",\"transform\":";array(s.transform);std::cout<<'}';
        }
        std::cout<<"],\"bsp\":[";first=true;for (const auto& block:current->bsp) {
            if (!first) std::cout<<',';first=false;const auto& s=block.selection;
            std::cout<<"{\"id\":"<<std::quoted(s.resource_id)<<",\"sections\":";array(s.sections);std::cout<<'}';
        } std::cout<<']';
    }
    std::cout<<"}\n";
}
'''


def generate(rows, trace_sha, catalog_digest, present_thread, render_pass):
    emitter = Emitter()
    observations, scopes, draws = [], [], []
    for events in rows.values():
        for event in events:
            if event["event"] == "scene_bridge_observation":
                observations.append(event)
            elif event["event"] == "scene_bridge_submission_begin":
                scopes.append(event)
            elif event["event"] == "draw":
                draws.append(event)
    for i, event in enumerate(observations):
        emitter.observation(event, i)
    for i, event in enumerate(scopes):
        emitter.submission(event, i)
    for i, event in enumerate(draws):
        emitter.draw(event, i)
    obs_ids = {id(item): i for i, item in enumerate(observations)}
    scope_ids = {id(item): i for i, item in enumerate(scopes)}
    draw_ids = {id(item): i for i, item in enumerate(draws)}
    body = ["int main(){std::cout<<std::setprecision(9);",
            "std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(std::cin)),{});",
            "std::uint8_t expected[32]={" + ','.join(str(x) for x in catalog_digest) + "};",
            "LoadedCatalog catalog;if(!LoadCatalog(bytes.data(),bytes.size(),digest,expected,catalog))return 10;",
            f'FrameProvenance source;source.origin=FrameOrigin::CapturedReplay;source.session_id="capture-replay";source.capture_sha256="{trace_sha}";',
            f"auto driver=SceneDriver::Create(std::move(catalog),source,{uint(render_pass)},1);if(!driver)return 11;",
            "std::unordered_map<std::uint64_t,ScopeRecord> scopes;std::unordered_map<std::uint64_t,DrawRecord> draws;",
            f"ThreadToken boundary{{{uint(present_thread)},1}};auto first=driver->BeginPresent(boundary);",
            "if(driver->EndPresent(first,boundary,true)!=WindowResult::Primed)return 12;"]
    for frame, events in rows.items():
        stack = []
        for event in events:
            kind = event["event"]
            thread = event.get("thread_id", present_thread)
            if kind == "scene_bridge_observation":
                body.append(f"{{ThreadToken t{{{uint(thread)},1}};auto id=driver->BeginProbe(t);driver->EndProbe(id,t,probe{obs_ids[id(event)]}());}}")
            elif kind == "scene_bridge_submission_begin":
                ticket = event["ticket"]
                stack.append(ticket)
                sample = f"scope{scope_ids[id(event)]}()" if event["stage"] == 1 else "SubmissionSample{}"
                body.append(f"{{ThreadToken t{{{uint(thread)},1}};auto sample={sample};auto id=driver->BeginScope(t,{uint(event['stage'])},{uint(event['section'])});driver->SetScopeSample(id,t,sample);scopes.emplace({uint(ticket,64)},ScopeRecord{{id,sample}});}}")
            elif kind == "scene_bridge_submission_end":
                if not stack or stack.pop() != event["ticket"]:
                    raise ValueError("unbalanced recorded scopes")
                sample = "s.sample" if event["stable"] else "SubmissionSample{}"
                body.append(f"{{auto it=scopes.find({uint(event['ticket'],64)});if(it==scopes.end())return 20;auto s=it->second;driver->EndScope(s.ticket,ThreadToken{{{uint(thread)},1}},{sample});scopes.erase(it);}}")
            elif kind == "draw":
                ticket = event.get("submission_ticket")
                if ticket != (stack[-1] if stack else None):
                    raise ValueError("draw and recorded scope are inconsistent")
                scope = f"scopes.at({uint(ticket,64)}).ticket" if ticket is not None else "0"
                args = (truth(event["call"] == "DrawIndexedPrimitive"), truth(event["primitive_type"] == 4),
                        *(uint(event[k]) for k in ("start_index", "primitive_count", "min_vertex_index", "num_vertices")))
                body.append(f"{{ThreadToken t{{{uint(thread)},1}};IndexedDraw args{{{','.join(args)}}};auto draw=driver->BeginDraw(t,{scope},args);draws.emplace({uint(event['op_id'],64)},DrawRecord{{draw,state{draw_ids[id(event)]}(),t}});}}")
            elif kind == "clear":
                scope = f"scopes.at({uint(stack[-1],64)}).ticket" if stack else "0"
                body.append(f"{{ThreadToken t{{{uint(present_thread)},1}};auto draw=driver->BeginDraw(t,{scope},IndexedDraw{{}});draws.emplace({uint(event['op_id'],64)},DrawRecord{{draw,DrawObservation{{}},t}});}}")
            elif kind in ("draw_result", "clear_result"):
                body.append(f"{{auto it=draws.find({uint(event['op_id'],64)});if(it==draws.end())return 21;auto d=it->second;driver->EndDraw(d.start.ticket,d.thread,{success(event['hr'])},d.state,d.state);draws.erase(it);}}")
            elif kind == "frame_end":
                if stack:
                    raise ValueError("pending scope at Present")
                body.append(f"{{auto id=driver->BeginPresent(boundary);auto result=driver->EndPresent(id,boundary,{success(event['present_hr'])});report({uint(frame,64)},result,*driver);}}")
        if stack:
            raise ValueError("unclosed scope in frame")
    body.append("return scopes.empty() && draws.empty() ? 0 : 22;}")
    return CPP_HEAD + '\n'.join(emitter.lines) + '\n' + '\n'.join(body)


def compare_snapshots(report, directory, trace_sha, render_pass):
    def float32(value):
        bits = float_bits(value)
        return 0 if bits == 0x80000000 else bits

    for item in report:
        path = Path(directory) / f"frame-{item['trace_frame']}.scene.json"
        scene = scene_ir.SceneSnapshot.from_json(path.read_bytes())
        if (scene.source.kind != "runtime" or scene.source.frame_id != item["trace_frame"] or
                scene.source.fingerprints.get("trace") != trace_sha or scene.source.map != item.get("map") or
                scene.source.fingerprints.get("map") != item.get("map_sha256") or item.get("result") != "published" or
                not item.get("flat_valid") or scene.camera is None):
            raise ValueError("reference scene provenance or replay output does not match")
        camera = item["camera"]
        if any([float32(v) for v in getattr(scene.camera, key)] != [float32(v) for v in camera[key]]
               for key in ("view", "projection")):
            raise ValueError("camera differs from reference scene")
        viewport = scene.camera.viewport
        expected_viewport = {"x": viewport.x, "y": viewport.y, "width": viewport.width, "height": viewport.height,
                             "min_depth": viewport.min_depth, "max_depth": viewport.max_depth}
        for key, value in expected_viewport.items():
            if key in ("min_depth", "max_depth"):
                if float32(value) != float32(camera["viewport"][key]):
                    raise ValueError("viewport depth differs from reference scene")
            elif value != camera["viewport"][key]:
                raise ValueError("viewport differs from reference scene")
        bed = [mesh for mesh in scene.mesh_instances if mesh.id.endswith(":StaticMeshActor__1520")]
        if len(bed) != 1 or len(item["meshes"]) != 1 or item["meshes"][0]["id"] != bed[0].id:
            raise ValueError("bed identity differs from reference scene")
        actual = item["meshes"][0]
        if (actual["resource_id"] != bed[0].resource_id or
                [float32(v) for v in actual["transform"]] != [float32(v) for v in bed[0].transform]):
            raise ValueError("bed resource or transform differs from reference scene")
        expected = {}
        for submission in scene.submissions:
            if submission.render_pass != render_pass or submission.stage != 1:
                continue
            if submission.target_kind == "bsp_block" or submission.target_id == bed[0].id:
                expected.setdefault(submission.target_id, set()).add(submission.section)
        actual_sections = {block["id"]: set(block["sections"]) for block in item["bsp"]}
        actual_sections[actual["id"]] = set(actual["sections"])
        if expected != actual_sections:
            raise ValueError("selected bed/BSP sections differ from reference scene")
        item["reference_match"] = "camera_bed_and_bsp_sections"


def replay(trace, catalog, frames, render_pass=0, scene_dir=None):
    rows, trace_sha, thread = read_capture(trace, frames)
    catalog_bytes = Path(catalog).read_bytes()
    if len(catalog_bytes) < 40 or len(catalog_bytes) > MAX_CATALOG:
        raise ValueError("invalid catalog size")
    catalog_digest = hashlib.sha256(catalog_bytes[:-32]).digest()
    if catalog_bytes[-32:] != catalog_digest:
        raise ValueError("catalog fingerprint mismatch")
    compiler = shutil.which(os.environ.get("CXX", "c++"))
    if not compiler:
        raise ValueError("C++ compiler not found")
    source = generate(rows, trace_sha, catalog_digest, thread, render_pass)
    with tempfile.TemporaryDirectory(prefix="t3render-captured-driver-") as directory:
        path = Path(directory)
        (path / "fixture.cpp").write_text(source)
        binary = path / "fixture"
        sources = ("ir/scene_live.cpp", "ir/scene_live_window.cpp", "src/scene_live_binding.cpp", "src/scene_live_catalog.cpp",
                   "src/scene_live_driver.cpp", "src/scene_live_flat.cpp", "src/scene_live_materials.cpp", "src/scene_live_lighting.cpp")
        command = [compiler, "-std=c++17", "-O0", "-pthread", "-I" + str(ROOT / "src"), str(path / "fixture.cpp"),
                   *(str(ROOT / item) for item in sources), "-o", str(binary)]
        subprocess.run(command, check=True, capture_output=True, timeout=120)
        result = subprocess.run([str(binary)], input=catalog_bytes, check=True, capture_output=True, timeout=120)
    report = [json.loads(line, object_pairs_hook=trace_summary._object, parse_constant=trace_summary._constant)
              for line in result.stdout.splitlines()]
    if [item.get("trace_frame") for item in report] != frames:
        raise ValueError("driver fixture did not report every requested frame")
    previous = None
    for item in report:
        camera = item.get("camera")
        item["camera_changed_from_previous"] = (None if camera is None or previous is None else
                                                 camera["view"] != previous["view"] or
                                                 camera["projection"] != previous["projection"])
        previous = camera
    if scene_dir:
        compare_snapshots(report, scene_dir, trace_sha, render_pass)
    return {"source": "captured_replay", "trace_sha256": trace_sha,
            "catalog_sha256": hashlib.sha256(catalog_bytes).hexdigest(), "render_pass": render_pass, "stage": 1,
            "frames": report,
            "limitations": ["This replays logged callbacks, not the live TDS process or DX12 viewer.",
                            "Thread birth is synthetic; clear thread is inferred from the recorded Present thread.",
                            "Draw state after the call is assumed equal to the recorded pre-draw state; a stable scope reuses its recorded initial sample.",
                            "JSON float values can lose the sign of zero in recorded draw state.",
                            "Unlogged invalid scopes and generic per-scope asset samples are not reconstructed.",
                            "Offline catalog geometry and fingerprints remain candidate data; GPU inputs and full visibility are not established."]}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--catalog", type=Path, required=True)
    parser.add_argument("--frames", type=int, nargs="+", required=True)
    parser.add_argument("--render-pass", type=int, default=0)
    parser.add_argument("--compare-scene-dir", type=Path)
    args = parser.parse_args(argv)
    try:
        print(json.dumps(replay(args.trace, args.catalog, args.frames, args.render_pass, args.compare_scene_dir),
                         indent=2, allow_nan=False))
    except (OSError, ValueError, KeyError, TypeError, IndexError, subprocess.SubprocessError, trace_summary.TraceError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
