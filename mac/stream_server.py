#!/usr/bin/env python3
"""Stream DJI Osmo Pocket (UVC) as MJPEG for the Switch NRO viewer."""

from __future__ import annotations

import argparse
import json
import re
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse
from pathlib import Path

BOUNDARY = b"osmoframe"
PORT_DEFAULT = 8765
DEVICE_HINTS = ("OsmoPocket", "Osmo Pocket", "DJI")

# Sizes the Osmo Pocket 4 UVC actually offers; we scale from these.
SOURCE_SIZES = ["1920x1080", "1280x720", "3840x2160"]
SCALES = ["640:360", "960:540", "1280:720"]
# The Pocket 4 UVC only advertises 24/25/30 fps. Anything else makes ffmpeg
# fail to open the device ("Selected framerate is not supported").
FPS_CHOICES = [24, 25, 30]
QUALITY_CHOICES = [3, 5, 8, 12, 18]


class Settings:
    """Live-tunable capture settings, shared with the Switch menu."""

    def __init__(self, device: str, size: str, scale: str, fps: int, quality: int):
        self.lock = threading.Lock()
        self.device = device
        self.size = size
        self.scale = scale
        self.fps = fps
        self.quality = quality

    def snapshot(self) -> dict:
        with self.lock:
            return {
                "size": self.size,
                "scale": self.scale,
                "fps": self.fps,
                "quality": self.quality,
            }

    def update(self, **kwargs) -> bool:
        """Returns True when something that needs an ffmpeg restart changed."""
        restart = False
        with self.lock:
            for key, value in kwargs.items():
                if value is None:
                    continue
                if getattr(self, key) == value:
                    continue
                setattr(self, key, value)
                if key in ("size", "scale", "fps", "quality"):
                    restart = True
        return restart

    def cycle(self, key: str, choices: list, step: int = 1) -> object:
        with self.lock:
            cur = getattr(self, key)
            try:
                idx = choices.index(cur)
            except ValueError:
                idx = 0
            new = choices[(idx + step) % len(choices)]
            setattr(self, key, new)
            return new

    def preset(self, name: str) -> bool:
        presets = {
            "nitido": {"scale": "1280:720", "fps": 30, "quality": 3},
            "equilibrado": {"scale": "1280:720", "fps": 30, "quality": 5},
            "rapido": {"scale": "960:540", "fps": 30, "quality": 8},
            "turbo": {"scale": "640:360", "fps": 30, "quality": 12},
        }
        if name not in presets:
            return False
        return self.update(**presets[name])


def lan_ips() -> list[str]:
    ips: list[str] = []
    # Prefer ipconfig (works under launchd where ifconfig PATH can be weird)
    for iface in ("en0", "en1", "en2"):
        try:
            out = subprocess.check_output(
                ["ipconfig", "getifaddr", iface], text=True, errors="ignore"
            ).strip()
            if re.fullmatch(r"\d+\.\d+\.\d+\.\d+", out) and not out.startswith("127."):
                ips.append(out)
        except Exception:
            pass
    try:
        out = subprocess.check_output(["/sbin/ifconfig"], text=True, errors="ignore")
    except Exception:
        out = ""
    current = None
    for line in out.splitlines():
        if line and not line.startswith("\t") and not line.startswith(" "):
            current = line.split(":")[0]
        m = re.search(r"inet (\d+\.\d+\.\d+\.\d+)", line)
        if not m or not current:
            continue
        ip = m.group(1)
        if ip.startswith("127.") or current.startswith("lo"):
            continue
        if current.startswith(("en", "bridge", "wlan")):
            ips.append(ip)
    # prefer en0 / 192.168.* first
    ips.sort(key=lambda x: (0 if x.startswith("192.168.") else 1, x))
    return list(dict.fromkeys(ips))


def list_avfoundation_devices() -> list[str]:
    proc = subprocess.run(
        ["ffmpeg", "-f", "avfoundation", "-list_devices", "true", "-i", ""],
        capture_output=True,
        text=True,
        errors="ignore",
    )
    text = proc.stderr + proc.stdout
    names: list[str] = []
    in_video = False
    for line in text.splitlines():
        if "AVFoundation video devices" in line:
            in_video = True
            continue
        if "AVFoundation audio devices" in line:
            break
        if not in_video:
            continue
        m = re.search(r"\[(\d+)\]\s+(.+)$", line)
        if m:
            names.append(m.group(2).strip())
    return names


def pick_device(preferred: str | None) -> str:
    devices = list_avfoundation_devices()
    if preferred:
        for d in devices:
            if preferred.lower() in d.lower():
                return d
        if preferred.isdigit():
            return preferred
        return preferred
    for d in devices:
        for hint in DEVICE_HINTS:
            if hint.lower() in d.lower():
                return d
    if devices:
        return devices[0]
    raise SystemExit(
        "Nenhuma câmera encontrada. Conecte a Osmo em modo webcam e tente de novo.\n"
        f"ffmpeg devices: {devices or '(vazio)'}"
    )


class Ptz:
    """Talks to the long-lived ptzctl helper (UVC pan/tilt on the Osmo)."""

    def __init__(self, binary: Path):
        self.binary = binary
        self.lock = threading.Lock()
        self.proc: subprocess.Popen[str] | None = None
        self.pan = 90.0
        self.tilt = 0.0
        self.pan_min, self.pan_max = -38.0, 215.0
        self.tilt_min, self.tilt_max = -45.0, 94.0
        self.available = False
        self._start()

    def _start(self) -> None:
        if not self.binary.is_file():
            print(f"[ptz] {self.binary} ausente — controle do gimbal off", flush=True)
            return
        try:
            self.proc = subprocess.Popen(
                [str(self.binary)],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
                bufsize=1,
            )
        except OSError as exc:
            print(f"[ptz] falhou iniciar: {exc}", flush=True)
            return

        with self.lock:
            first = self._readline()
            if first and first.startswith("range ") and first != "range err":
                parts = first.split()
                if len(parts) == 5:
                    try:
                        self.pan_min, self.pan_max, self.tilt_min, self.tilt_max = map(
                            float, parts[1:]
                        )
                        self.available = True
                        print(
                            f"[ptz] pronto pan[{self.pan_min:.0f},{self.pan_max:.0f}] "
                            f"tilt[{self.tilt_min:.0f},{self.tilt_max:.0f}]",
                            flush=True,
                        )
                    except ValueError:
                        pass
            if self.available:
                pos = self._command("g")
                if pos and pos.startswith("pos "):
                    try:
                        _, p, t = pos.split()
                        self.pan, self.tilt = float(p), float(t)
                    except ValueError:
                        pass

    def _readline(self) -> str | None:
        if not self.proc or not self.proc.stdout:
            return None
        try:
            return self.proc.stdout.readline().strip()
        except (OSError, ValueError):
            return None

    def _command(self, cmd: str) -> str | None:
        if not self.proc or not self.proc.stdin:
            return None
        try:
            self.proc.stdin.write(cmd + "\n")
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError, ValueError):
            return None
        return self._readline()

    def _clamp(self) -> None:
        self.pan = max(self.pan_min, min(self.pan_max, self.pan))
        self.tilt = max(self.tilt_min, min(self.tilt_max, self.tilt))

    def move(self, dpan: float, dtilt: float) -> bool:
        if not self.available:
            return False
        with self.lock:
            self.pan += dpan
            self.tilt += dtilt
            self._clamp()
            out = self._command(f"s {self.pan:.2f} {self.tilt:.2f}")
            return out == "ok"

    def goto(self, pan: float, tilt: float) -> bool:
        if not self.available:
            return False
        with self.lock:
            self.pan, self.tilt = pan, tilt
            self._clamp()
            return self._command(f"s {self.pan:.2f} {self.tilt:.2f}") == "ok"

    def center(self) -> bool:
        if not self.available:
            return False
        with self.lock:
            self.pan = (self.pan_min + self.pan_max) / 2.0
            self.tilt = 0.0
            return self._command(f"s {self.pan:.2f} {self.tilt:.2f}") == "ok"

    def state(self) -> dict:
        return {
            "available": self.available,
            "pan": round(self.pan, 2),
            "tilt": round(self.tilt, 2),
            "range": {
                "pan_min": self.pan_min,
                "pan_max": self.pan_max,
                "tilt_min": self.tilt_min,
                "tilt_max": self.tilt_max,
            },
        }

    def stop(self) -> None:
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.proc.kill()


class FrameStore:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._jpeg: bytes | None = None
        self._seq = 0
        self._updated = threading.Condition(self._lock)

    def set(self, jpeg: bytes) -> None:
        with self._updated:
            self._jpeg = jpeg
            self._seq += 1
            self._updated.notify_all()

    def get(self) -> bytes | None:
        with self._lock:
            return self._jpeg

    def seq(self) -> int:
        with self._lock:
            return self._seq

    def drain(self, last_seq: int) -> int:
        """Skip to the newest frame, discarding anything queued in between."""
        with self._lock:
            if self._seq == last_seq:
                return last_seq
            return self._seq

    def wait_next(self, last_seq: int, timeout: float = 2.0) -> tuple[int, bytes | None]:
        with self._updated:
            ok = self._updated.wait_for(lambda: self._seq != last_seq, timeout=timeout)
            if not ok:
                return last_seq, self._jpeg
            return self._seq, self._jpeg


class Capture:
    """Runs ffmpeg and keeps only the newest frame (no backlog → low latency)."""

    def __init__(self, settings: "Settings", store: FrameStore):
        self.settings = settings
        self.store = store
        self.proc: subprocess.Popen[bytes] | None = None
        self.stop_flag = threading.Event()
        self.thread: threading.Thread | None = None
        self.restart_lock = threading.Lock()
        self.gen = 0
        self.latest: bytes | None = None
        self.latest_lock = threading.Lock()
        self.reader: threading.Thread | None = None
        self.fail_streak = 0

    def _cmd(self) -> list[str]:
        s = self.settings
        vf_parts = [f"fps={s.fps}"]
        if s.scale:
            vf_parts.append(f"scale={s.scale}")
        return [
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "error",
            # low-latency: never buffer, always hand over the newest frame
            "-fflags",
            "nobuffer",
            "-flags",
            "low_delay",
            "-f",
            "avfoundation",
            "-use_wallclock_as_timestamps",
            "1",
            "-framerate",
            str(s.fps),
            "-video_size",
            s.size,
            "-i",
            f"{s.device}:none",
            "-an",
            "-vf",
            ",".join(vf_parts),
            "-f",
            "mjpeg",
            "-q:v",
            str(s.quality),
            "-muxdelay",
            "0",
            "-flush_packets",
            "1",
            "pipe:1",
        ]

    def start(self) -> None:
        self.stop_flag.clear()
        self.thread = threading.Thread(target=self._supervise, daemon=True)
        self.thread.start()

    def _supervise(self) -> None:
        while not self.stop_flag.is_set():
            with self.restart_lock:
                gen = self.gen
            cmd = self._cmd()
            print(f"[capture] {' '.join(cmd)}", flush=True)
            try:
                self.proc = subprocess.Popen(
                    cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE
                )
            except OSError as exc:
                print(f"[capture] falhou iniciar ffmpeg: {exc}", flush=True)
                break

            produced = self._pump(self.proc, gen)

            if self.stop_flag.is_set():
                break
            # restart only when settings changed; otherwise the pipeline died
            with self.restart_lock:
                changed = self.gen != gen
            if changed:
                self.fail_streak = 0
                continue

            err = b""
            if self.proc.stderr:
                try:
                    err = self.proc.stderr.read() or b""
                except Exception:
                    pass
            text = err.decode(errors="ignore")
            print(f"[capture] ffmpeg ended: {text[:300]}", flush=True)

            # A profile the camera rejects would otherwise spin forever. Fall
            # back to something known-good instead of burning CPU.
            if not produced:
                self.fail_streak += 1
                if self.fail_streak >= 3:
                    print("[capture] 3 falhas seguidas — voltando ao perfil seguro "
                          "(1280:720 @30, q5)", flush=True)
                    self.settings.update(scale="1280:720", fps=30, quality=5)
                    self.fail_streak = 0
                    continue
                time.sleep(1.0)
            else:
                self.fail_streak = 0
                time.sleep(0.3)

    def _pump(self, proc: subprocess.Popen[bytes], gen: int) -> bool:
        """Reads frames until the process ends. Returns True if any frame came out."""
        assert proc.stdout is not None
        buf = bytearray()
        produced = False
        while not self.stop_flag.is_set():
            with self.restart_lock:
                if self.gen != gen:
                    break
            chunk = proc.stdout.read(4096)
            if not chunk:
                break
            buf.extend(chunk)
            while True:
                start = buf.find(b"\xff\xd8")
                if start < 0:
                    buf.clear()
                    break
                end = buf.find(b"\xff\xd9", start + 2)
                if end < 0:
                    if start > 0:
                        del buf[:start]
                    break
                jpeg = bytes(buf[start : end + 2])
                del buf[: end + 2]
                if 1024 < len(jpeg) < 8_000_000:
                    self.store.set(jpeg)
                    produced = True
        self._kill(proc)
        return produced

    @staticmethod
    def _kill(proc: subprocess.Popen[bytes]) -> None:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=1)
            except subprocess.TimeoutExpired:
                proc.kill()

    def apply(self) -> None:
        """Restart ffmpeg with the current settings."""
        with self.restart_lock:
            self.gen += 1

    def stop(self) -> None:
        self.stop_flag.set()
        with self.restart_lock:
            self.gen += 1
        if self.thread:
            self.thread.join(timeout=3)


def make_handler(store: FrameStore, meta: dict, ptz: Ptz, settings: Settings, capture):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt: str, *args) -> None:
            sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

        def _json(self, obj, code: int = 200) -> None:
            payload = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def do_GET(self) -> None:  # noqa: N802
            parsed = urlparse(self.path)
            path = parsed.path
            qs = parse_qs(parsed.query)

            if path == "/config":
                # GET reports; query params change one field at a time
                if "preset" in qs:
                    settings.preset(qs["preset"][0])
                elif "cycle" in qs:
                    key = qs["cycle"][0]
                    step = int(qs.get("step", ["1"])[0])
                    if key == "size":
                        settings.cycle("size", SOURCE_SIZES, step)
                    elif key == "scale":
                        settings.cycle("scale", SCALES, step)
                    elif key == "fps":
                        settings.cycle("fps", FPS_CHOICES, step)
                    elif key == "quality":
                        settings.cycle("quality", QUALITY_CHOICES, step)
                else:
                    changed = {}
                    if "scale" in qs:
                        changed["scale"] = qs["scale"][0]
                    if "fps" in qs:
                        changed["fps"] = int(qs["fps"][0])
                    if "quality" in qs:
                        changed["quality"] = int(qs["quality"][0])
                    if "size" in qs:
                        changed["size"] = qs["size"][0]
                    if changed and settings.update(**changed):
                        capture.apply()

                self._json(
                    {
                        "ok": True,
                        "settings": settings.snapshot(),
                        "choices": {
                            "size": SOURCE_SIZES,
                            "scale": SCALES,
                            "fps": FPS_CHOICES,
                            "quality": QUALITY_CHOICES,
                        },
                        "presets": ["nitido", "equilibrado", "rapido", "turbo"],
                    }
                )
                return

            if path in ("/", "/index.html"):
                body = (
                    "<!doctype html><meta charset=utf-8>"
                    "<title>Osmo → Switch</title>"
                    "<body style='margin:0;background:#111;color:#eee;font:16px sans-serif'>"
                    "<p style='padding:12px'>Osmo stream — Switch NRO usa "
                    f"<code>http://{meta['primary_ip']}:{meta['port']}/stream</code></p>"
                    "<img style='max-width:100%;height:auto' src='/stream'>"
                    "</body>"
                ).encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return

            if path == "/info":
                payload = json.dumps(meta, indent=2).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
                return

            if path in ("/ptz", "/ptz/move"):
                if "action" in qs and qs["action"][0] == "center":
                    ok = ptz.center()
                else:
                    try:
                        dx = float(qs.get("dx", ["0"])[0])
                        dy = float(qs.get("dy", ["0"])[0])
                    except ValueError:
                        self._json({"ok": False, "error": "bad dx/dy"}, 400)
                        return
                    if "pan" in qs and "tilt" in qs:
                        ok = ptz.goto(float(qs["pan"][0]), float(qs["tilt"][0]))
                    else:
                        ok = ptz.move(dx, dy)
                self._json({"ok": bool(ok), **ptz.state()})
                return

            if path in ("/ptz/state", "/status"):
                self._json(ptz.state())
                return

            if path == "/snapshot.jpg":
                jpeg = store.get()
                if not jpeg:
                    self.send_error(503, "no frame yet")
                    return
                self.send_response(200)
                self.send_header("Content-Type", "image/jpeg")
                self.send_header("Content-Length", str(len(jpeg)))
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self.wfile.write(jpeg)
                return

            if path == "/stream":
                self.send_response(200)
                self.send_header(
                    "Content-Type",
                    f"multipart/x-mixed-replace; boundary={BOUNDARY.decode()}",
                )
                self.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
                self.send_header("Connection", "close")
                self.end_headers()
                try:
                    self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                    # small send buffer keeps frames from queueing inside the OS
                    self.connection.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)
                except OSError:
                    pass
                last = -1
                try:
                    while True:
                        last, jpeg = store.wait_next(last, timeout=2.0)
                        if not jpeg:
                            continue
                        # Skip to the newest frame: replaying the queue is what
                        # reads as camera lag on the Switch.
                        last = store.drain(last)
                        newest = store.get()
                        if newest is not None:
                            jpeg = newest
                        header = (
                            b"--"
                            + BOUNDARY
                            + b"\r\nContent-Type: image/jpeg\r\nContent-Length: "
                            + str(len(jpeg)).encode()
                            + b"\r\n\r\n"
                        )
                        self.wfile.write(header)
                        self.wfile.write(jpeg)
                        self.wfile.write(b"\r\n")
                        self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError, OSError):
                    return

            self.send_error(404)

    return Handler


def write_host_file(path: Path, ip: str, port: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(f"{ip}:{port}\n", encoding="utf-8")
    print(f"[config] wrote {path} -> {ip}:{port}", flush=True)


def main() -> None:
    ap = argparse.ArgumentParser(description="Osmo Pocket MJPEG server for Switch")
    ap.add_argument("--port", type=int, default=PORT_DEFAULT)
    ap.add_argument("--device", default=None, help="AVFoundation name or index")
    # Pocket 4 UVC modes: 1920x1080, 1080x1920, 1728x3072, 3840x2160
    ap.add_argument("--size", default="1920x1080")
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("--quality", type=int, default=8, help="ffmpeg -q:v (2=best, 31=worst)")
    ap.add_argument(
        "--scale",
        default="1280:720",
        help="ffmpeg scale WxH for Switch (empty = no scale)",
    )
    ap.add_argument(
        "--host-file",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "switch" / "romfs" / "host.txt",
    )
    args = ap.parse_args()

    device = pick_device(args.device)
    ips = lan_ips()
    primary = ips[0] if ips else "127.0.0.1"
    meta = {
        "device": device,
        "port": args.port,
        "primary_ip": primary,
        "ips": ips,
        "stream_url": f"http://{primary}:{args.port}/stream",
        "size": args.size,
        "scale": args.scale,
        "fps": args.fps,
    }
    print(json.dumps(meta, indent=2), flush=True)
    write_host_file(args.host_file, primary, args.port)
    # also drop a copy where the NRO looks on SD after install helper
    write_host_file(
        Path(__file__).resolve().parents[1] / "dist" / "host.txt",
        primary,
        args.port,
    )

    store = FrameStore()
    scale = args.scale.strip() or None
    settings = Settings(device, args.size, scale, args.fps, args.quality)
    cap = Capture(settings, store)
    cap.start()

    ptz = Ptz(Path(__file__).resolve().parent / "ptzctl")
    meta["ptz"] = ptz.available
    meta["settings"] = settings.snapshot()

    handler = make_handler(store, meta, ptz, settings, cap)
    server = ThreadingHTTPServer(("0.0.0.0", args.port), handler)
    # Serve immediately so Switch can connect while ffmpeg warms up
    http_thread = threading.Thread(target=server.serve_forever, daemon=True)
    http_thread.start()
    print(f"[http] listening on 0.0.0.0:{args.port}", flush=True)
    print(f"[http] open http://{primary}:{args.port}/", flush=True)

    for i in range(100):
        if store.get():
            print(f"[capture] first frame after {i * 0.1:.1f}s", flush=True)
            break
        time.sleep(0.1)
    else:
        print("[warn] ainda sem frame — fecha GimbalBar e confirma modo webcam", flush=True)

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\nstopping…", flush=True)
    finally:
        server.shutdown()
        server.server_close()
        cap.stop()
        ptz.stop()


if __name__ == "__main__":
    # avoid address reuse issues on quick restart
    socket.setdefaulttimeout(None)
    main()
