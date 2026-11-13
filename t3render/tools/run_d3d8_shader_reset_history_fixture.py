#!/usr/bin/env python3
"""Compare opt-in pre-Reset declaration hypotheses in the isolated fixture."""
import argparse
import json
import math
import os
from pathlib import Path
import shutil
import sys
import tempfile

from run_synthetic_up_replay import RunError, check_pe32, run_stage, sha256, valid_path, windows_path
from run_d3d8_stateblock_experiment import check_stage_loader
import run_d3d8_shader_reset_fixture as base
import trace_to_ir


def inspect_history(path, observation, scenario, program_capture=False):
    # Both the standard validator and the IR parser see the same private bytes.
    data = Path(path).read_bytes()
    with tempfile.TemporaryDirectory(prefix="t3-reset-history-") as directory:
        snapshot = Path(directory) / "trace.jsonl"; snapshot.write_bytes(data)
        base.inspect_trace(snapshot, observation, True, scenario)
        ir = trace_to_ir.convert(snapshot)
    capability = ir["compatibility"]["d3d8"]["header"].get("declaration_history")
    expected = {"requested": True, "words_per_observation": 1024, "words_per_frame": 262144,
                "words_per_device": 8192}
    if (not isinstance(capability, dict) or capability.get("requested") is not True or
            any(type(capability.get(k)) is not int or capability[k] != v for k, v in expected.items() if k != "requested")):
        raise RunError("history capability differs")
    program_cap = ir["compatibility"]["d3d8"]["header"].get("vertex_program_capture")
    if program_capture and program_cap != {"requested": True, "words_per_observation": 1024, "words_per_frame": 262144}:
        raise RunError("vertex-program capability differs")
    if not program_capture and program_cap is not None:
        raise RunError("unexpected vertex-program capability")
    epochs = []
    for phase, frame in enumerate(ir["frames"]):
        if len(frame["commands"]) != 1: raise RunError("history fixture has an unexpected draw count")
        command = frame["commands"][0]
        raw = command["compatibility"]["d3d8"]["draw"]
        history, validated = raw.get("vertex_declaration_pre_reset"), command.get("vertex_declaration_pre_reset")
        # If the deliberately invalid Reset unexpectedly succeeds, the
        # reference marks the run PARTIAL; it did not actually invalidate the
        # retained data as a failed Reset would.
        had_failed_reset = scenario == "recovery" and base.failed(base.hresult(observation["resets"][0]))
        should_exist = phase > 0 and not had_failed_reset and observation["phases"][phase]["original"]
        if program_capture:
            program = raw.get("vertex_program")
            prior_program = raw.get("vertex_program_pre_reset")
            ir_program = command.get("vertex_program_observed")
            ir_prior_program = command.get("vertex_program_pre_reset")
            if phase == 0 and observation["phases"][phase]["original"]:
                if program != {"status": "candidate", "version": "vs_1_1", "generation": 1, "words": base.PROGRAM}:
                    raise RunError("pre-Reset vertex program differs")
                if not isinstance(ir_program, dict) or ir_program.get("status") != "candidate" or ir_program.get("words") != base.PROGRAM:
                    raise RunError("pre-Reset vertex program was not paired")
            elif program is not None or ir_program is not None:
                raise RunError("program candidate appeared after Reset or fallback")
            if should_exist:
                if not isinstance(prior_program, dict) or not isinstance(history, dict):
                    raise RunError("missing pre-Reset program hypothesis")
                expected_program = {**history, "words": base.PROGRAM}
                if prior_program != expected_program:
                    raise RunError("pre-Reset program hypothesis differs")
                if ir_prior_program != {**expected_program, "shader_value": observation["handle"],
                                        "shader_stable": True, "draw_hresult": 0}:
                    raise RunError("pre-Reset program hypothesis was not paired")
            elif prior_program is not None or ir_prior_program is not None:
                raise RunError("pre-Reset program appeared without eligible history")
        if not should_exist:
            if "vertex_declaration_pre_reset" in raw or "vertex_declaration_pre_reset" in command:
                raise RunError("history appeared before Reset or after failed Reset/recovery")
            continue
        fields = {"status", "source", "generation", "reset_epoch", "reset_hr", "rebind_epoch", "words"}
        if not isinstance(history, dict) or set(history) != fields:
            raise RunError("missing or malformed pre-Reset hypothesis")
        reset_hr = base.hresult(observation["resets"][phase if scenario == "recovery" else phase - 1])
        if (history.get("status") != "hypothesis" or history.get("source") != "application_memory_before_reset" or
                type(history.get("generation")) is not int or history["generation"] != 1 or
                type(history.get("reset_hr")) is not int or history["reset_hr"] != reset_hr or base.failed(reset_hr) or
                type(history.get("reset_epoch")) is not int or not 0 < history["reset_epoch"] < 2**64 or
                type(history.get("rebind_epoch")) is not int or not history["reset_epoch"] < history["rebind_epoch"] < 2**64 or
                history.get("words") != base.WORDS):
            raise RunError("invalid pre-Reset hypothesis contents")
        if epochs and history["reset_epoch"] <= epochs[-1][1]: raise RunError("reset epochs did not advance")
        epochs.append((history["reset_epoch"], history["rebind_epoch"]))
        expected_ir = {**history, "shader_value": observation["handle"], "shader_stable": True, "draw_hresult": 0}
        if validated != expected_ir: raise RunError("validated IR did not retain the distinct stable hypothesis")
        standard = command.get("vertex_declaration_observed")
        if (not isinstance(standard, dict) or standard.get("status") != "unavailable" or
                standard.get("reason") != "reset_attempt" or standard.get("scan_status") != "complete" or
                standard.get("generation") != 1 or standard.get("words") is not None):
            raise RunError("history changed the ordinary declaration observation")
    return {"hypotheses": len(epochs), "epochs": epochs}


def execute(engine, bottle, fixture, proxy, output, timeout, program_capture=False):
    engine, bottle, fixture, proxy, output = map(valid_path, (engine, bottle, fixture, proxy, output))
    if any(not str(p).isascii() or '"' in str(p) for p in (fixture, proxy, output)):
        raise RunError("fixture, proxy and output paths must be ASCII without double quotes")
    wine, server, frameworks = engine / "engine/bin/wine", engine / "engine/bin/wineserver", engine / "frameworks"
    if not all(p.is_file() and os.access(p, os.X_OK) for p in (wine, server)) or not frameworks.is_dir():
        raise RunError("invalid Highball engine")
    z = bottle / "dosdevices/z:"
    if not bottle.is_dir() or not z.is_symlink() or os.readlink(z) != "/": raise RunError("bottle must map Z: to /")
    check_pe32(fixture, False); check_pe32(proxy, True)
    if not math.isfinite(timeout) or timeout <= 0: raise RunError("timeout must be positive and finite")
    for protected in (engine, bottle, fixture.parent, proxy.parent):
        if output.resolve() == protected.resolve() or protected.resolve() in output.resolve().parents:
            raise RunError("output must be outside engine, bottle and binary directories")
    try: output.mkdir()
    except OSError as exc: raise RunError(f"cannot create output directory: {exc}") from exc
    env = {k: v for k, v in os.environ.items() if not k.startswith("T3RENDER_CAPTURE_")}
    for key in ("WINEDLLPATH", "WINEPATH", "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH"): env.pop(key, None)
    env.update({"WINEPREFIX": str(bottle), "WINESERVER": str(server),
                "WINEDLLPATH_PREPEND": f"{bottle}/.highball-dlss/wine:{engine}/renderers/d3dmetal-tsshim/wine:{engine}/renderers/d3dmetal/wine",
                "DYLD_FALLBACK_LIBRARY_PATH": f"{frameworks}:{frameworks}/GStreamer.framework/Versions/1.0/lib",
                "DYLD_FALLBACK_FRAMEWORK_PATH": str(frameworks), "WINEDEBUG": "-all,+loaddll"})
    manifest = {"schema": 1, "test": "synthetic_d3d8_pre_reset_hypothesis", "status": "FAIL",
                "vertex_program_capture": program_capture,
                "fixture_sha256": sha256(fixture), "proxy_sha256": sha256(proxy), "scenarios": {},
                "limitations": ["Synthetic x86 fixture only; no TDS process or game data.",
                                "Pre-Reset bytes are a hypothesis, not proof of driver state or shader identity.",
                                "No device queries or drawing occur between failed Reset and successful recovery.",
                                "Failed Reset alone is not observed; native Windows and gameplay are not tested."]}
    stage = "baseline"
    try:
        reference = base.execute(str(engine), str(bottle), str(fixture), str(proxy), str(output / "reference"), timeout)
        partial = reference["status"] != "PASS"
        for scenario in base.SCENARIOS:
            stage = scenario
            directory = output / scenario; directory.mkdir()
            executable = directory / fixture.name; shutil.copyfile(fixture, executable)
            dll = directory / "d3d8.dll"; shutil.copyfile(proxy, dll)
            if sha256(executable) != manifest["fixture_sha256"] or sha256(dll) != manifest["proxy_sha256"]:
                raise RunError("fixture or proxy copy differs")
            trace = output / f"{scenario}.jsonl"; log = output / f"{scenario}.log"
            outputs = [output / f"{scenario}_{i}.ppm" for i in range(3)]
            child = env.copy()
            child.update({"WINEDLLOVERRIDES": "d3d8=n,b;nvapi64,nvngx=n,b;winemenubuilder.exe=d",
                          "T3RENDER_CAPTURE_FRAME": "0", "T3RENDER_CAPTURE_FRAMES": "3",
                          "T3RENDER_CAPTURE_DECLARATIONS": "1", "T3RENDER_CAPTURE_DECLARATION_HISTORY": "1",
                          "T3RENDER_CAPTURE_PATH": windows_path(trace)})
            if program_capture:
                child["T3RENDER_CAPTURE_VERTEX_PROGRAMS"] = "1"
            args = [wine, executable]
            for phase, path in enumerate(outputs): args += [f"--out{phase}", windows_path(path)]
            args += ["--scenario", scenario]
            run_stage(stage, args, directory, child, log, timeout)
            observed = base.inspect_log(log, scenario)
            check_stage_loader(log, bottle, dll)
            history = inspect_history(trace, observed, scenario, program_capture)
            ref_dir = output / "reference" / scenario
            baseline = base.inspect_log(ref_dir / "baseline.log", scenario)
            if observed["events"] != baseline["events"] or observed["parity"] != baseline["parity"]:
                raise RunError("history opt-in changed fixture HRESULT or shader observations")
            for phase, path in enumerate(outputs):
                actual = base.inspect_image(path, observed["phases"][phase]["original"])
                original = base.inspect_image(ref_dir / f"baseline_{phase}.ppm", baseline["phases"][phase]["original"])
                if actual != original: raise RunError("history opt-in changed RGB pixels")
            partial = partial or observed["partial"] or reference["scenarios"][scenario]["status"] != "PASS"
            manifest["scenarios"][scenario] = {"history": history, "log_sha256": sha256(log),
                                                "trace_sha256": sha256(trace),
                                                "ppm_sha256": [sha256(p) for p in outputs]}
        manifest["reference_result_sha256"] = sha256(output / "reference/result.json")
        manifest["status"] = "PARTIAL" if partial else "PASS"
    except (RunError, OSError, ValueError, trace_to_ir.trace_summary.TraceError) as exc:
        manifest["failed_stage"] = stage; manifest["error"] = str(exc)
    with (output / "result.json").open("x", encoding="utf-8") as destination:
        json.dump(manifest, destination, indent=2); destination.write("\n")
    if manifest["status"] == "FAIL": raise RunError(f"{stage}: {manifest['error']}; partial output: {output}")
    return manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("engine", "bottle", "fixture", "proxy", "output-dir"): parser.add_argument("--" + option, required=True)
    parser.add_argument("--timeout", type=float, default=45)
    parser.add_argument("--vertex-programs", action="store_true", help="check separate pre-Reset program hypotheses")
    args = parser.parse_args(argv)
    try: result = execute(args.engine, args.bottle, args.fixture, args.proxy, args.output_dir, args.timeout,
                          args.vertex_programs)
    except (RunError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr); return 1
    print(f"{result['status']}: synthetic pre-Reset hypothesis fixture; results: {args.output_dir}")
    return 0 if result["status"] == "PASS" else 3


if __name__ == "__main__": raise SystemExit(main())
