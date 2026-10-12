"""Run an isolated native probe and save first-chance failure evidence.

Uses Windows debugging APIs from a separate process so a stack-exhausted app
does not need to write its own dump. No third-party Python dependencies.
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import os
from pathlib import Path
import struct
import subprocess
import time

k = c.WinDLL('kernel32', use_last_error=True)
d = c.WinDLL('dbghelp', use_last_error=True)

class ExceptionRecord(c.Structure):
    _fields_ = [('code', w.DWORD), ('flags', w.DWORD), ('nested', c.c_void_p),
                ('address', c.c_void_p), ('count', w.DWORD), ('info', c.c_ulonglong * 15)]
class ExceptionInfo(c.Structure):
    _fields_ = [('record', ExceptionRecord), ('first', w.DWORD)]
class DebugUnion(c.Union):
    _fields_ = [('exception', ExceptionInfo), ('raw', c.c_byte * 160)]
class DebugEvent(c.Structure):
    _fields_ = [('kind', w.DWORD), ('pid', w.DWORD), ('tid', w.DWORD), ('data', DebugUnion)]
class DumpException(c.Structure):
    _fields_ = [('tid', w.DWORD), ('pointers', c.c_void_p), ('client', w.BOOL)]
class ExceptionPointers(c.Structure):
    _fields_ = [('record', c.c_void_p), ('context', c.c_void_p)]

k.WaitForDebugEvent.argtypes = [c.POINTER(DebugEvent), w.DWORD]
k.ContinueDebugEvent.argtypes = [w.DWORD, w.DWORD, w.DWORD]
k.OpenThread.argtypes = [w.DWORD, w.BOOL, w.DWORD]
k.OpenThread.restype = w.HANDLE
k.GetThreadContext.argtypes = [w.HANDLE, c.c_void_p]
k.ReadProcessMemory.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t)]
k.CloseHandle.argtypes = [w.HANDLE]
k.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, c.c_void_p, w.DWORD, w.DWORD, w.HANDLE]
k.CreateFileW.restype = w.HANDLE
d.MiniDumpWriteDump.argtypes = [w.HANDLE, w.DWORD, w.HANDLE, w.DWORD, c.c_void_p, c.c_void_p, c.c_void_p]
d.SymInitialize.argtypes = [w.HANDLE, c.c_char_p, w.BOOL]
d.SymFromAddr.argtypes = [w.HANDLE, c.c_ulonglong, c.POINTER(c.c_ulonglong), c.c_void_p]
d.SymCleanup.argtypes = [w.HANDLE]

def symbol(handle, address):
    buf = c.create_string_buffer(88 + 1024)
    struct.pack_into('<I', buf, 0, 88)
    struct.pack_into('<I', buf, 80, 1024)
    displacement = c.c_ulonglong()
    if d.SymFromAddr(handle, address, c.byref(displacement), buf):
        name = bytes(buf[84:]).split(b'\0', 1)[0].decode('utf-8', errors='replace')
        return f'{address:#x} {name}+{displacement.value:#x}'
    return None

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True)
    parser.add_argument('--library', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--display', action='store_true')
    args = parser.parse_args()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, LOCALAPPDATA=str(out / 'profile'), MUSICBOARD_TRACE_INPUT='1')
    (out / 'profile').mkdir(exist_ok=True)
    with (out / 'stdout.txt').open('wb') as stdout, (out / 'stderr.txt').open('wb') as stderr:
        proc = subprocess.Popen([str(Path(args.exe).resolve()), '--library', args.library,
            '--selftest-display' if args.display else '--selftest'],
            env=env, stdout=stdout, stderr=stderr, creationflags=0x2)
        event = DebugEvent()
        symbols_ready = False
        errors = 0
        deadline = time.monotonic() + 180
        while time.monotonic() < deadline:
            if not k.WaitForDebugEvent(c.byref(event), 1000):
                if proc.poll() is not None: break
                continue
            continuation = 0x10002
            if event.kind == 1:
                record = event.data.exception.record
                continuation = 0x80010001
                if record.code == 0x80000003: continuation = 0x10002
                if record.code in (0xc0000005, 0xc00000fd, 0xc0000409):
                    errors += 1
                    print(f'exception={record.code:#x} address={record.address:#x} first={event.data.exception.first}', flush=True)
                    if not symbols_ready:
                        symbols_ready = bool(d.SymInitialize(proc._handle, str(Path(args.exe).resolve().parent).encode(), True))
                    thread = k.OpenThread(0x0048, False, event.tid)
                    storage = c.create_string_buffer(1232 + 16)
                    aligned = (c.addressof(storage) + 15) & ~15
                    c.c_uint32.from_address(aligned + 48).value = 0x10001f
                    if k.GetThreadContext(thread, aligned):
                        rsp = c.c_uint64.from_address(aligned + 152).value
                        rip = c.c_uint64.from_address(aligned + 248).value
                        print('IP ' + (symbol(proc._handle, rip) or hex(rip)), flush=True)
                        memory = c.create_string_buffer(4096)
                        read = c.c_size_t()
                        k.ReadProcessMemory(proc._handle, rsp, memory, len(memory), c.byref(read))
                        for offset in range(0, read.value - 7, 8):
                            address = struct.unpack_from('<Q', memory, offset)[0]
                            if address < 0x10000: continue
                            name = symbol(proc._handle, address)
                            if name: print(f'stack+{offset:#x} {name}', flush=True)
                        pointers = ExceptionPointers(c.addressof(record), aligned)
                        info = DumpException(event.tid, c.addressof(pointers), False)
                        target = k.CreateFileW(str(out / f'failure-{errors}.dmp'), 0x40000000, 0, None, 2, 0x80, None)
                        ok = d.MiniDumpWriteDump(proc._handle, proc.pid, target, 0x1000, c.byref(info), None, None)
                        print(f'dump={bool(ok)} win32={c.get_last_error()}', flush=True)
                        k.CloseHandle(target)
                    k.CloseHandle(thread)
            k.ContinueDebugEvent(event.pid, event.tid, continuation)
            if event.kind == 5: break
        else:
            proc.terminate()
            raise RuntimeError('native probe exceeded 180 seconds')
        if symbols_ready: d.SymCleanup(proc._handle)
        exit_code=proc.wait(timeout=10)
        print(f'exit={exit_code} exceptions={errors}', flush=True)
        if exit_code != 0 or errors:
            raise SystemExit(1)

if __name__ == '__main__': main()
