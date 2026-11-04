"""ME2 race fixes on the Switch SDK (masseffect-nx + SHD patch), the same as the local patches of the PC SDK
(C:\\ME2_Recomp\\tools\\rexglue-sdk). Idempotent: run after every fresh SDK checkout (sdk_local_fixes.sh calls it).

1. InputSystem: XamInput* run on several guest threads at once and race on drivers_/devices_/assignment_
   (heap corruption 0xC0000374 on PC). One recursive mutex around every public entry point.
2. XThread::Create / Restore: the new thread read thread_name_ while XThread::set_name() could write it
   (heap corruption on PC). Copy it under thread_lock_.
3. Eden: a zero-timeout nvFenceWait (NVK's vkGetFenceStatus poll) becomes nvhost-ctrl EventWaitAsync with
   timeout 0, which never returns under Eden (ring thread -> presenter -> GetFenceStatus, gdb 2026-10-24). With
   RexSwitchSetFencePollRead(1) the poll reads the syncpoint instead (SyncptRead) and answers like libnx does
   (success or LibnxNvidiaError_Timeout). Off by default: the console keeps the libnx path.
"""
import re
import sys
from pathlib import Path

SDK = Path(sys.argv[1] if len(sys.argv) > 1 else '/root/nx/masseffect-nx/sdk')


def fix_input():
    h = SDK / 'include/rex/input/input_system.h'
    s = h.read_text()
    if 'ME2: one lock' not in s:
        # The last private member block: put the mutex right after the drivers_ declaration.
        m = re.search(r'\n(\s*)std::vector<std::unique_ptr<InputDriver>> drivers_;\n', s)
        if not m:
            sys.exit('input_system.h: drivers_ declaration not found')
        ind = m.group(1)
        s = (s[:m.end()] + f'{ind}// ME2: one lock around every public entry point (XamInput* from several guest threads).\n'
             f'{ind}mutable std::recursive_mutex mutex_;\n' + s[m.end():])
        if '#include <mutex>' not in s:
            s = s.replace('#pragma once\n', '#pragma once\n\n#include <mutex>\n', 1)
        h.write_text(s)
        print('input_system.h: mutex_ added')
    c = SDK / 'src/input/input_system.cpp'
    s = c.read_text()
    n = 0
    for fn in ('void InputSystem::Shutdown()', 'void InputSystem::AddDriver(', 'void InputSystem::AttachWindow(',
               'void InputSystem::SetActiveCallback(', 'void InputSystem::SetDeviceAssignment(',
               'X_RESULT InputSystem::GetCapabilities(', 'X_RESULT InputSystem::GetState(',
               'X_RESULT InputSystem::SetState(', 'X_RESULT InputSystem::GetKeystroke('):
        i = s.find(fn)
        if i < 0:
            sys.exit(f'input_system.cpp: {fn} not found')
        b = s.find('{\n', i) + 2
        body_head = s[b:b + 200]
        if 'std::lock_guard lock(mutex_);' in body_head.split('\n', 2)[0] + body_head.split('\n', 2)[1]:
            continue
        # After SCOPE_profile_cpu_f("hid"); when it is the first line.
        first = s[b:s.find('\n', b) + 1]
        at = b + len(first) if 'SCOPE_profile_cpu_f' in first else b
        s = s[:at] + '  std::lock_guard lock(mutex_);\n' + s[at:]
        n += 1
    c.write_text(s)
    print(f'input_system.cpp: {n} locks added')


def fix_xthread():
    c = SDK / 'src/system/xthread.cpp'
    s = c.read_text()
    a = ('    thread_->set_name(thread_name_);\n\n    PROFILE_THREAD_ENTER(thread_name_.c_str());\n')
    a_new = ('    // ME2: XThread::set_name() may run concurrently, copy the name under its lock.\n'
             '    std::string name;\n    {\n      std::lock_guard<std::mutex> lock(thread_lock_);\n'
             '      name = thread_name_;\n      thread_->set_name(name);\n    }\n\n'
             '    PROFILE_THREAD_ENTER(name.c_str());\n')
    b = ('      thread->thread_->set_name(thread->name());\n\n      PROFILE_THREAD_ENTER(thread->name().c_str());\n')
    b_new = ('      // ME2: same race as XThread::Create.\n      std::string name;\n      {\n'
             '        std::lock_guard<std::mutex> lock(thread->thread_lock_);\n        name = thread->thread_name_;\n'
             '        thread->thread_->set_name(name);\n      }\n\n      PROFILE_THREAD_ENTER(name.c_str());\n')
    done = 0
    for old, new in ((a, a_new), (b, b_new)):
        if new in s:
            continue
        k = s.count(old)
        if k != 1:
            sys.exit(f'xthread.cpp: expected 1 site, found {k}:\n{old}')
        s = s.replace(old, new)
        done += 1
    c.write_text(s)
    print(f'xthread.cpp: {done} sites fixed')


def fix_fence_poll():
    c = SDK / 'src/ui/switch_perf.cpp'
    s = c.read_text()
    if 'RexSwitchSetFencePollRead' in s:
        print('switch_perf.cpp: fence poll already patched')
        return
    old = ('Result __wrap_nvFenceWait(NvFence* f, s32 timeout_us) {\n'
           '  p2::gpuwait::Scope gpu_fence_scope(timeout_us==0?p2::gpuwait::FencePoll:p2::gpuwait::FenceBlock);\n'
           '  const u64 from = armGetSystemTick();\n')
    if s.count(old) != 1:
        sys.exit('switch_perf.cpp: __wrap_nvFenceWait head not found')
    new = ('// ME2 (Eden): zero-timeout polls read the syncpoint instead of nvhost-ctrl EventWaitAsync(timeout 0), which\n'
           '// never returns under Eden. Off unless the app turns it on (me2_eden_fence_poll).\n'
           'static std::atomic<int> g_me2_fence_poll_read{0};\n'
           'extern "C" void RexSwitchSetFencePollRead(int on) { g_me2_fence_poll_read.store(on, std::memory_order_relaxed); }\n'
           'static u32 Me2CtrlFd() {\n'
           '  static u32 fd = [] { u32 v = 0; return R_SUCCEEDED(nvOpen(&v, "/dev/nvhost-ctrl")) ? v : 0u; }();\n'
           '  return fd;\n'
           '}\n\n'
           + old +
           '  if (timeout_us == 0 && f && f->id != UINT32_MAX && g_me2_fence_poll_read.load(std::memory_order_relaxed)) {\n'
           '    u32 value = 0;\n'
           '    const u32 fd = Me2CtrlFd();\n'
           '    Result rc = MAKERESULT(Module_LibnxNvidia, LibnxNvidiaError_Timeout);\n'
           '    if (fd && R_SUCCEEDED(nvioctlNvhostCtrl_SyncptRead(fd, f->id, &value)) && s32(value - f->value) >= 0) rc = 0;\n'
           '    Note(kFenceQuery, from);\n'
           '    return rc;\n'
           '  }\n')
    s = s.replace(old, new)
    if '#include <atomic>' not in s:
        s = s.replace('#include', '#include <atomic>\n#include', 1)
    c.write_text(s)
    print('switch_perf.cpp: zero-timeout fence poll via SyncptRead (switchable)')


def fix_depth_bias_clamp():
    # 4. The native renderer's colour-pass depth bias (me2_p2_color_depth_bias, after mgr-nx-deko) needs a
    #    non-zero depthBiasClamp, which Vulkan allows only with the depthBiasClamp feature enabled.
    h = SDK / 'include/rex/ui/vulkan/device.h'
    s = h.read_text()
    if 'bool depthBiasClamp' not in s:
        old = '    bool depthClamp = false;\n'
        if s.count(old) != 1:
            sys.exit('device.h: depthClamp property not found')
        h.write_text(s.replace(old, old + '    bool depthBiasClamp = false;\n'))
        print('device.h: depthBiasClamp property added')
    c = SDK / 'src/ui/vulkan/vulkan_device.cpp'
    s = c.read_text()
    if 'XE_UI_VULKAN_FEATURE(depthBiasClamp)' not in s:
        old = '    XE_UI_VULKAN_FEATURE(depthClamp)\n'
        if s.count(old) != 1:
            sys.exit('vulkan_device.cpp: depthClamp feature not found')
        c.write_text(s.replace(old, old + '    XE_UI_VULKAN_FEATURE(depthBiasClamp)\n'))
        print('vulkan_device.cpp: depthBiasClamp feature enabled')


fix_input()
fix_xthread()
fix_fence_poll()
fix_depth_bias_clamp()
