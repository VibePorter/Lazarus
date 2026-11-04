# qrenderdoc.exe --python tools\win\rdc_capture_at.py
# Launches out\pc\sa2.exe under RenderDoc (Vulkan), triggers one capture RDC_AT seconds in, saves it as RDC_OUT
# (.rdc) and ends the game. Env: RDC_AT (default 45), RDC_OUT, RDC_RUN (validation folder for log/userdata),
# RDC_EXTRA (';'-separated extra game arguments).
import os, shutil, subprocess, time, traceback
import renderdoc as rd

root = os.environ.get("RDC_ROOT", r"C:\Lazarus")  # qrenderdoc's Python has no __file__
exe_dir = os.path.join(root, "out", "pc")
exe = os.path.join(exe_dir, "sa2.exe")
at = float(os.environ.get("RDC_AT", "45"))
out_path = os.environ["RDC_OUT"]
run = os.environ["RDC_RUN"]
os.makedirs(os.path.join(run, "userdata"), exist_ok=True)
log = open(os.path.join(run, "rdc_capture.log"), "w")


def say(*a):
    log.write(" ".join(str(x) for x in a) + "\n")
    log.flush()


args = ["--fullscreen=false", "--window_start_minimized=false", "--audio_mute=true", "--sa2_renderer=renut",
        "--renut_gpu_backend=vulkan", "--mnk_mode=true", "--input_xbox_layout=true", "--me2_shared_input=true",
        "--license_mask=0xFFFFFFFF", "--sa2_render_height_handheld=360", "--sa2_render_height_docked=360",
        "--banjo_renut_store_cards=false", "--banjo_deko3d_alpha_spec=false", "--sa2_enhancements_report_s=0",
        '--log_file="%s"' % os.path.join(run, "runtime.log"), "--log_level=info", "--log_flush_interval=1",
        '--user_data_root="%s"' % os.path.join(run, "userdata"),
        '--game_data_root="%s"' % os.path.join(root, "assets", "game_root")]
args += [a for a in os.environ.get("RDC_EXTRA", "").split(";") if a]

pid = 0
try:
    opts = rd.CaptureOptions()
    result = rd.ExecuteAndInject(exe, exe_dir, " ".join(args), [], os.path.join(run, "cap"), opts, False)
    say("inject", result.result, "ident", result.ident)
    ctl = rd.CreateTargetControl("", result.ident, "rdc_capture_at", True)
    pid = ctl.GetPID()
    say("pid", pid)
    start = time.time()
    triggered = False
    path = None
    while time.time() - start < at + 60:
        msg = ctl.ReceiveMessage(None)
        if msg.type == rd.TargetControlMessageType.NewCapture:
            path = msg.newCapture.path
            say("new capture", path, "frame", msg.newCapture.frameNumber)
            break
        if msg.type == rd.TargetControlMessageType.Disconnected:
            say("disconnected")
            break
        if not triggered and time.time() - start >= at:
            ctl.TriggerCapture(1)
            triggered = True
            say("triggered at", round(time.time() - start, 1))
        time.sleep(0.1)
    if path:
        time.sleep(2)
        ctl.CopyCapture(0, out_path) if not os.path.exists(path) else shutil.copy(path, out_path)
        time.sleep(2)
        say("saved", out_path, os.path.exists(out_path))
    ctl.Shutdown()
except Exception:
    say(traceback.format_exc())
finally:
    if pid:
        subprocess.call(["taskkill", "/F", "/PID", str(pid)])
    log.close()
    os._exit(0)
