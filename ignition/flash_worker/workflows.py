"""Temporal workflows for badge build and flash operations."""
import asyncio
import re
from datetime import timedelta

from temporalio import workflow
from temporalio.common import RetryPolicy

with workflow.unsafe.imports_passed_through():
    from flash_worker.activities import (
        build_firmware,
        detect_badge_devices,
        detect_badge_ports,
        flash_badge_images,
        prepare_badge_devices_for_flashing,
        prepare_flash_artifacts,
        resolve_badge_port,
        verify_badge_boot_marker,
        verify_badge_ble,
    )

TASK_QUEUE = "badge-flash"
MAX_CONCURRENT_FLASHES = 32
FLASH_START_STAGGER_SECONDS = 0

_BUILD_RETRY  = RetryPolicy(maximum_attempts=1)   # builds are deterministic
_DETECT_RETRY = RetryPolicy(maximum_attempts=3)
_FLASH_RETRY  = RetryPolicy(
    maximum_attempts=3,
    initial_interval=timedelta(seconds=5),
    backoff_coefficient=2.0,
    maximum_interval=timedelta(seconds=30),
)
_VERIFY_RETRY = RetryPolicy(maximum_attempts=1)
_PREPARE_RETRY = RetryPolicy(maximum_attempts=1)
_ACTIVITY_ONCE = RetryPolicy(maximum_attempts=1)
_FLASH_ATTEMPTS = 3
_BOOT_ATTEMPTS = 2
_BLE_ATTEMPTS = 1


def _safe_id_part(value: str, fallback: str = "unknown") -> str:
    value = re.sub(r"[^A-Za-z0-9_.-]+", "-", value or "").strip("-")
    return (value or fallback)[:80]


def _badge_label(index: int) -> str:
    return f"Badge {index + 1:02d}"


@workflow.defn
class BuildFirmwareWorkflow:
    """Build firmware for a PlatformIO environment.

    Full build output (last 150 lines) stored in Temporal history.
    Browse at http://localhost:8233.
    """

    @workflow.run
    async def run(self, env: str, firmware_dir: str = "") -> dict:
        return await workflow.execute_activity(
            build_firmware,
            args=[env, firmware_dir],
            start_to_close_timeout=timedelta(minutes=5),
            heartbeat_timeout=timedelta(seconds=45),
            retry_policy=_BUILD_RETRY,
        )


@workflow.defn
class BadgeFlashWorkflow:
    """Flash and verify one durable badge identity."""

    def __init__(self) -> None:
        self._label = ""
        self._usb_id = ""
        self._initial_port = ""
        self._current_port = ""
        self._phase = "waiting"
        self._attempts = {"flash": 0, "boot": 0, "ble": 0}
        self._result_summary = ""
        self._boot_log_tail = ""
        self._ble_log_tail = ""
        self._workflow_id = ""

    @workflow.query
    def status(self) -> dict:
        return {
            "label": self._label,
            "workflow_id": self._workflow_id,
            "usb_id": self._usb_id,
            "initial_port": self._initial_port,
            "current_port": self._current_port,
            "phase": self._phase,
            "attempts": dict(self._attempts),
            "result_summary": self._result_summary,
            "boot_log_tail": self._boot_log_tail,
            "ble_log_tail": self._ble_log_tail,
        }

    async def _resolve_current_device(self, device: dict, timeout_s: int = 20) -> dict:
        deadline = workflow.now() + timedelta(seconds=timeout_s)
        last = {
            "success": False,
            "port": "",
            "device": device,
            "error": "Badge has not been resolved yet.",
        }

        while True:
            last = await workflow.execute_activity(
                resolve_badge_port,
                args=[device],
                start_to_close_timeout=timedelta(seconds=15),
                retry_policy=_DETECT_RETRY,
            )
            if last.get("success"):
                resolved_device = last.get("device") or device
                self._current_port = last.get("port") or resolved_device.get("port", "")
                return last
            if workflow.now() >= deadline:
                return last
            await workflow.sleep(timedelta(seconds=2))

    def _base_result(self, success: bool, device: dict, output: str = "", error: str = "",
                     duration_s: float = 0.0, boot_log: str = "", ble_log: str = "") -> dict:
        return {
            "success": success,
            "label": self._label,
            "workflow_id": self._workflow_id,
            "port": self._current_port or self._initial_port,
            "device": device,
            "usb_id": self._usb_id,
            "phase": self._phase,
            "output": output,
            "boot_log": boot_log,
            "ble_log": ble_log,
            "error": error,
            "duration_s": round(duration_s, 1),
        }

    @workflow.run
    async def run(self, request: dict) -> dict:
        env = request["env"]
        firmware_dir = request.get("firmware_dir", "")
        device = dict(request.get("device") or {})
        self._label = request.get("label") or "Badge"
        self._usb_id = device.get("usb_id") or device.get("serial") or device.get("port", "")
        self._initial_port = device.get("port", "")
        self._current_port = self._initial_port
        self._workflow_id = workflow.info().workflow_id

        output_sections: list[str] = []
        total_duration = 0.0
        flashed: dict | None = None

        for attempt in range(1, _FLASH_ATTEMPTS + 1):
            self._attempts["flash"] = attempt
            self._phase = "resolving"
            resolved = await self._resolve_current_device(device)
            if not resolved.get("success"):
                self._result_summary = resolved.get("error", "Badge was not found.")
                output_sections.append(self._result_summary)
            else:
                device = dict(resolved.get("device") or device)
                self._phase = "flashing"
                try:
                    flashed = await workflow.execute_activity(
                        flash_badge_images,
                        args=[env, self._current_port, firmware_dir],
                        start_to_close_timeout=timedelta(minutes=5),
                        heartbeat_timeout=timedelta(seconds=60),
                        retry_policy=_ACTIVITY_ONCE,
                    )
                except Exception as e:
                    flashed = {
                        "success": False,
                        "port": self._current_port,
                        "output": "",
                        "error": str(e),
                        "duration_s": 0.0,
                    }

                total_duration += float(flashed.get("duration_s") or 0.0)
                output_sections.append(flashed.get("output", ""))
                if flashed.get("success"):
                    self._result_summary = "Flash complete."
                    break
                self._result_summary = flashed.get("error", "Flash failed.")

            if attempt < _FLASH_ATTEMPTS:
                await workflow.sleep(timedelta(seconds=5 * attempt))

        if not flashed or not flashed.get("success"):
            self._phase = "failed"
            return self._base_result(
                False,
                device,
                "\n\n".join(s for s in output_sections if s),
                self._result_summary or "Flash failed.",
                total_duration,
            )

        verify: dict | None = None
        self._phase = "booting"
        await self._resolve_current_device(device)

        for attempt in range(1, _BOOT_ATTEMPTS + 1):
            self._attempts["boot"] = attempt
            self._phase = "verifying_boot"
            resolved = await self._resolve_current_device(device)
            if resolved.get("success"):
                device = dict(resolved.get("device") or device)

            try:
                verify = await workflow.execute_activity(
                    verify_badge_boot_marker,
                        args=[self._current_port, 75],
                        start_to_close_timeout=timedelta(seconds=105),
                    heartbeat_timeout=timedelta(seconds=30),
                    retry_policy=_VERIFY_RETRY,
                )
            except Exception as e:
                verify = {
                    "success": False,
                    "port": self._current_port,
                    "output": "",
                    "boot_log": "",
                    "error": str(e),
                    "duration_s": 0.0,
                }

            total_duration += float(verify.get("duration_s") or 0.0)
            self._boot_log_tail = verify.get("boot_log") or verify.get("output", "")
            self._result_summary = (
                "Boot marker observed." if verify.get("success")
                else verify.get("error", "Boot marker verification failed.")
            )
            if verify.get("success"):
                break
            if attempt < _BOOT_ATTEMPTS:
                self._phase = "booting"
                await workflow.sleep(timedelta(seconds=3))

        ok = bool(verify and verify.get("success"))
        ble_verify: dict | None = None
        if ok:
            ble_verify = {
                "success": True,
                "port": self._current_port,
                "output": "[verify] BLE verification skipped.",
                "ble_log": "[verify] BLE verification skipped.",
                "error": "",
                "duration_s": 0.0,
            }
            self._ble_log_tail = ble_verify["ble_log"]
            self._result_summary = "Boot marker observed; BLE verification skipped."
        self._phase = "done" if ok else "failed"
        output = "\n\n".join(s for s in [
            *output_sections,
            (verify or {}).get("output", ""),
            (ble_verify or {}).get("output", ""),
        ] if s)
        return self._base_result(
            ok,
            device,
            output,
            "" if ok else self._result_summary,
            total_duration,
            (verify or {}).get("boot_log", ""),
            (ble_verify or {}).get("ble_log", ""),
        )


@workflow.defn
class FlashBadgesWorkflow:
    """Prepare artifacts, detect connected badges, and start one child per badge."""

    def __init__(self) -> None:
        self._phase = "starting"
        self._env = ""
        self._ports: list[str] = []
        self._devices: list[dict] = []
        self._children: list[dict] = []
        self._passed_count = 0
        self._failed_count = 0
        self._expected_count = 0

    @workflow.query
    def status(self) -> dict:
        return {
            "phase": self._phase,
            "env": self._env,
            "ports": list(self._ports),
            "detected_count": len(self._devices),
            "expected_count": self._expected_count,
            "children": [dict(c) for c in self._children],
            "badges": {c["workflow_id"]: c["phase"] for c in self._children},
            "passed_count": self._passed_count,
            "failed_count": self._failed_count,
        }

    @workflow.run
    async def run(
        self,
        env: str,
        firmware_dir: str = "",
        rebuild_filesystem: bool = False,
        expected_count: int = 0,
    ) -> dict:
        self._env = env
        self._expected_count = max(0, int(expected_count or 0))
        self._phase = "preparing"
        prepared = await workflow.execute_activity(
            prepare_flash_artifacts,
            args=[env, firmware_dir, rebuild_filesystem],
            start_to_close_timeout=timedelta(minutes=3),
            heartbeat_timeout=timedelta(seconds=60),
            retry_policy=_PREPARE_RETRY,
        )
        if not prepared["success"]:
            self._phase = "failed"
            return {
                "success": False, "env": env, "ports": [], "results": [],
                "error": prepared.get("error", "Failed to prepare flash artifacts."),
            }

        self._phase = "detecting"

        devices: list = await workflow.execute_activity(
            prepare_badge_devices_for_flashing,
            start_to_close_timeout=timedelta(seconds=60),
            retry_policy=_DETECT_RETRY,
        )

        if not devices:
            self._phase = "failed"
            return {
                "success": False, "env": env, "ports": [], "results": [],
                "error": "No badges detected. Check USB connections and upload mode.",
            }

        self._devices = [dict(d) for d in devices]
        self._ports = [d.get("port", "") for d in self._devices]

        if self._expected_count and len(self._devices) != self._expected_count:
            self._phase = "failed"
            return {
                "success": False,
                "env": env,
                "ports": self._ports,
                "detected_count": len(self._devices),
                "expected_count": self._expected_count,
                "results": [],
                "error": (
                    f"Expected {self._expected_count} badge(s), "
                    f"detected {len(self._devices)}. Refusing to flash."
                ),
            }

        self._phase = "flashing"

        batch_id = _safe_id_part(workflow.info().workflow_id, "batch")
        seen_child_ids: set[str] = set()
        active: dict[str, asyncio.Task] = {}
        results_by_child: dict[str, dict] = {}

        for i, device in enumerate(self._devices):
            label = _badge_label(i)
            identity = (
                device.get("usb_id")
                or device.get("serial")
                or device.get("location")
                or device.get("port")
                or f"badge-{i + 1}"
            )
            base_child_id = f"badge-flash-{batch_id}-{_safe_id_part(identity)}"
            child_id = base_child_id
            duplicate = 2
            while child_id in seen_child_ids:
                child_id = f"{base_child_id}-{duplicate}"
                duplicate += 1
            seen_child_ids.add(child_id)

            child_info = {
                "label": label,
                "workflow_id": child_id,
                "usb_id": device.get("usb_id", ""),
                "initial_port": device.get("port", ""),
                "current_port": device.get("port", ""),
                "phase": "starting",
            }
            self._children.append(child_info)

            handle = await workflow.start_child_workflow(
                BadgeFlashWorkflow.run,
                {
                    "env": env,
                    "firmware_dir": firmware_dir,
                    "device": device,
                    "label": label,
                    "batch_workflow_id": workflow.info().workflow_id,
                },
                id=child_id,
                task_queue=workflow.info().task_queue,
                parent_close_policy=workflow.ParentClosePolicy.TERMINATE,
                memo={
                    "ignition_kind": "badge_flash",
                    "env": env,
                    "usb_id": device.get("usb_id", ""),
                    "initial_port": device.get("port", ""),
                },
                static_summary=f"Ignition badge flash: {label}",
                static_details=(
                    f"Flash `{env}` badge `{label}` "
                    f"({device.get('usb_id') or device.get('port', 'unknown')})."
                ),
            )
            child_info["phase"] = "running"
            active[child_id] = handle

        while active:
            if not active:
                break

            done_tasks, _ = await workflow.wait(
                active.values(),
                return_when="FIRST_COMPLETED",
            )
            done_child_ids = [
                child_id for child_id, task in active.items() if task in done_tasks
            ]

            for child_id in done_child_ids:
                task = active.pop(child_id)
                try:
                    r = await task
                except Exception as e:
                    r = {"success": False, "port": "", "output": "",
                         "error": str(e), "duration_s": 0}
                for child in self._children:
                    if child["workflow_id"] == child_id:
                        child["phase"] = "done" if r.get("success") else "failed"
                        child["current_port"] = r.get("port", child.get("current_port", ""))
                        break
                if r.get("success"):
                    self._passed_count += 1
                else:
                    self._failed_count += 1
                results_by_child[child_id] = r

        results = [results_by_child[c["workflow_id"]] for c in self._children]

        all_ok = all(r["success"] for r in results)
        self._phase = "done" if all_ok else "partial"

        return {
            "success": all_ok,
            "env": env,
            "ports": self._ports,
            "detected_count": len(self._devices),
            "children": [dict(c) for c in self._children],
            "results": results,
            "error": "" if all_ok else f"{sum(not r['success'] for r in results)}/{len(results)} badges failed.",
        }
