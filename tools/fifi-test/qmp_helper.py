#!/usr/bin/env python3
"""QMP/HMP helper with persistent connection for reliable timing."""
import sys, socket, time, json, subprocess, os

HMP_SOCK = '/home/aaron/src/linux-desktop/qemu-monitor.sock'
QMP_SOCK = '/home/aaron/src/linux-desktop/qemu-qmp.sock'
SCREEN_W, SCREEN_H = 1920, 1080

# ── HMP (simple, short-lived) ────────────────────────────────────────────────

def hmp(cmd):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(5.0)
    s.connect(HMP_SOCK)
    s.recv(4096)
    s.send((cmd + '\n').encode())
    time.sleep(0.3)
    s.recv(4096)
    s.close()

# ── QMP (persistent connection for reliable timing) ──────────────────────────

class QMP:
    def __init__(self):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(5.0)
        self.s.connect(QMP_SOCK)
        # Read greeting
        buf = b''
        while not buf.endswith(b'\n'):
            buf += self.s.recv(4096)
        # Negotiate
        self.s.send(b'{"execute":"qmp_capabilities"}\n')
        time.sleep(0.1)
        self.s.recv(4096)

    def _cmd(self, obj):
        self.s.send((json.dumps(obj) + '\n').encode())
        time.sleep(0.05)
        try: self.s.recv(4096)
        except: pass

    def close(self):
        self.s.close()

    def move(self, x, y):
        ax = int(x * 32767 / SCREEN_W)
        ay = int(y * 32767 / SCREEN_H)
        self._cmd({'execute':'input-send-event','arguments':{'events':[
            {'type':'abs','data':{'axis':'x','value':ax}},
            {'type':'abs','data':{'axis':'y','value':ay}}]}})

    def btn(self, down):
        self._cmd({'execute':'input-send-event','arguments':{'events':[
            {'type':'btn','data':{'button':'left','down':down}}]}})

    def click(self, x, y):
        self.move(x, y); time.sleep(0.04)
        self.btn(True); time.sleep(0.06)
        self.btn(False); time.sleep(0.04)

    def dblclick(self, x, y):
        self.click(x, y); time.sleep(0.15); self.click(x, y)

    def key(self, k):
        self._cmd({'execute':'input-send-event','arguments':{'events':[
            {'type':'key','data':{'key':{'type':'qcode','data':k},'down':True}}]}})
        time.sleep(0.03)
        self._cmd({'execute':'input-send-event','arguments':{'events':[
            {'type':'key','data':{'key':{'type':'qcode','data':k},'down':False}}]}})
        time.sleep(0.03)

def shot(path):
    hmp(f'screendump {path}.ppm')
    time.sleep(0.4)
    subprocess.run(['pnmscale', '0.5', f'{path}.ppm'], stdout=open(f'{path}_tmp.pnm','wb'), check=True)
    subprocess.run(['pnmtopng', f'{path}_tmp.pnm'], stdout=open(f'{path}.png','wb'), check=True)
    os.unlink(f'{path}.ppm')
    os.unlink(f'{path}_tmp.pnm')
    print(f'{path}.png')

if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'shot'
    if cmd == 'shot':
        shot(sys.argv[2] if len(sys.argv) > 2 else '/tmp/fifi_shot')
    elif cmd == 'click':
        q = QMP(); q.click(int(sys.argv[2]), int(sys.argv[3])); q.close()
    elif cmd == 'dblclick':
        q = QMP(); q.dblclick(int(sys.argv[2]), int(sys.argv[3])); q.close()
    elif cmd == 'move':
        q = QMP(); q.move(int(sys.argv[2]), int(sys.argv[3])); q.close()
