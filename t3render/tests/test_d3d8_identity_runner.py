"""Validate the result reader without requiring Windows or Wine."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import run_d3d8_identity_probe as probe
import trace_summary


ROUND_TRIP = ":QI(IUnknown).QI(IDirect3DDevice8)"


def device_records(path):
    device = {"event": "device", "path": path, "hr": "0x00000000", "success": True,
              "interface": "0x00000010", "iunknown": "0x00000010", "same_interface": True,
              "same_iunknown": True, "same_vtable": True}
    return [device, dict(device, path=path + ROUND_TRIP)]


def fixture_records():
    records = [
        {"event": "header", "schema": 1, "device": "0x00000010", "device_iunknown": "0x00000010", "auto_depth": False},
    ]
    records.extend(device_records("device.QueryInterface(IDirect3DDevice8)"))
    paths = list(probe.ROOTS) + [name for name in probe.GROUPS if name.startswith("device.")]
    mandatory = {"device.GetBackBuffer", "device.GetRenderTarget"}
    for path in paths:
        records.append({"event": "create", "path": path,
                        "hr": "0x00000000" if path in mandatory else "0x80004005",
                        "success": path in mandatory, "object_nonnull": path in mandatory})
        if path in mandatory:
            records.extend(device_records(path + ".GetDevice"))
            records.extend({"event": "container", "path": path + ".GetContainer", "iid": iid,
                            "hr": "0x80004002", "success": False,
                            "same_device_iunknown": None, "same_parent_iunknown": None}
                           for iid in ("IUnknown", "IDirect3DDevice8", "IDirect3DSwapChain8"))
    records.extend([{"event": "clear", "hr": "0x00000000", "success": True},
                    {"event": "present", "hr": "0x00000000", "success": True},
                    {"event": "end", "valid": True}])
    return records


class IdentityRunnerTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="t3render-identity-test-")
        self.addCleanup(self.directory.cleanup)
        self.log = Path(self.directory.name) / "result.log"

    def parse(self, records):
        self.log.write_text("diagnostic noise\n" + "".join(json.dumps(record, separators=(",", ":")) + "\n"
                                                         for record in records), encoding="utf-8")
        return probe.results_from_log(self.log)

    def test_unsupported_resources_remain_observations(self):
        records = fixture_records()
        self.assertEqual(self.parse(records), records)
        swap = next(record for record in records if record.get("path") == "device.CreateAdditionalSwapChain")
        swap.update(hr="0x00000000", success=True, object_nonnull=True)
        records[-3:-3] = [
            {"event": "device", "path": "swapchain.QueryInterface(IDirect3DDevice8)", "hr": "0x80004002",
             "success": False, "interface": None, "same_interface": None, "same_iunknown": None, "same_vtable": None},
            {"event": "create", "path": "swapchain.GetBackBuffer", "hr": "0x80004005",
             "success": False, "object_nonnull": False},
        ]
        self.assertEqual(self.parse(records), records)

    def test_successful_get_texture_must_have_complete_identity_results(self):
        records = fixture_records()
        texture = next(record for record in records if record.get("path") == "device.CreateTexture")
        texture.update(hr="0x00000000", success=True, object_nonnull=True)
        additions = [*device_records("texture.GetDevice"),
                     {"event": "create", "path": "texture.GetSurfaceLevel", "hr": "0x80004005",
                      "success": False, "object_nonnull": False},
                     {"event": "operation", "path": "device.SetTexture", "hr": "0x00000000", "success": True},
                     {"event": "create", "path": "device.GetTexture", "hr": "0x00000000",
                      "success": True, "object_nonnull": True}, *device_records("device.GetTexture.GetDevice"),
                     {"event": "resource_identity", "path": "device.GetTexture.IUnknown", "hr": "0x00000000",
                      "success": True, "iunknown": "0x00000020", "same_parent_iunknown": True},
                     {"event": "operation", "path": "device.SetTexture(null)", "hr": "0x00000000", "success": True}]
        records[-3:-3] = additions
        self.assertEqual(self.parse(records), records)
        failed_getter = copy.deepcopy(records)
        next(r for r in failed_getter if r.get("path") == "texture.GetDevice").update(
            hr="0x80004005", success=False, interface=None, same_interface=None, same_iunknown=None, same_vtable=None)
        with self.assertRaises(probe.RunError):
            self.parse(failed_getter)
        records.remove(next(item for item in additions if item.get("path") == "device.GetTexture.IUnknown"))
        with self.assertRaises(probe.RunError):
            self.parse(records)

    def test_rejects_partial_inconsistent_and_duplicate_records(self):
        for mutation in (
            lambda records: records.pop(),
            lambda records: records.insert(2, copy.deepcopy(records[1])),
            lambda records: records[1].update(same_iunknown=False),
            lambda records: records[1].update(same_vtable=False),
            lambda records: records[1].update(hr="0x80004002", success=False, interface=None,
                                              same_interface=None, same_iunknown=None, same_vtable=None),
            lambda records: records.pop(2),  # required QI round trip
            lambda records: records[2].update(hr="0x80004002", success=False, interface=None,
                                              same_interface=None, same_iunknown=None, same_vtable=None),
            lambda records: records[2].update(interface="0x00000020", iunknown="0x00000020",
                                              same_interface=False, same_iunknown=False, same_vtable=False),
            lambda records: next(r for r in records if r.get("path") == "device.GetBackBuffer").update(
                hr="0x80004005", success=False, object_nonnull=False),
            lambda records: next(r for r in records if r.get("path") == "device.GetRenderTarget.GetDevice").update(
                hr="0x80004005", success=False, interface=None, same_interface=None, same_iunknown=None, same_vtable=None),
            lambda records: next(r for r in records if r.get("path") == "device.CreateTexture").update(success=True),
            lambda records: next(r for r in records if r.get("path") == "device.CreateTexture").update(
                hr="0x00000000", success=True, object_nonnull=False),
            lambda records: next(r for r in records if r.get("path") == "device.CreateTexture").update(
                hr="0x00000000", success=True, object_nonnull=True),
        ):
            with self.subTest(mutation=mutation):
                records = fixture_records()
                mutation(records)
                with self.assertRaises(probe.RunError):
                    self.parse(records)

    def test_proxy_trace_requires_successful_clear_and_present_in_frame_zero(self):
        events = [
            {"event": "header", "format": "t3render-d3d8-diagnostic", "version": 1,
             "operation_results": 1, "clear_rect_limit": 256, "buffer_capture": {"requested": True}},
            {"event": "frame_begin", "frame": 0, "device": "0x10"},
            {"event": "clear", "frame": 0, "op_id": 1, "rect_count": 0, "rects_status": "none", "rects": [],
             "flags": 1, "color": 0xff000000, "depth": 1, "depth_bits": 0x3f800000, "stencil": 0},
            {"event": "clear_result", "frame": 0, "op_id": 1, "hr": 0},
            {"event": "frame_end", "frame": 0, "present_hr": 0, "draws": 0, "concurrent_operations": False},
            {"event": "capture_end", "frames": 1},
        ]
        def validate(data):
            self.log.write_text("".join(json.dumps(record) + "\n" for record in data), encoding="utf-8")
            return probe.validate_proxy_trace(self.log, "0x00000010")
        self.assertEqual(validate(events)["draw_count"], 0)
        for mutation in (lambda data: data[0]["buffer_capture"].update(requested=False),
                         lambda data: data[1].update(device="0x11"),
                         lambda data: data[2].update(color=0),
                         lambda data: data[3].update(hr=-1),
                         lambda data: data[4].update(present_hr=0x80004005),
                         lambda data: data[4].update(draws=1),
                         lambda data: data.pop(3)):
            with self.subTest(mutation=mutation):
                invalid = copy.deepcopy(events)
                mutation(invalid)
                with self.assertRaises((probe.RunError, trace_summary.TraceError)):
                    validate(invalid)


if __name__ == "__main__":
    unittest.main()
