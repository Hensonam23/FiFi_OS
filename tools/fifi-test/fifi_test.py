import socket, json, time, zlib, struct, sys

HMP_SOCK = '/home/aaron/src/linux-desktop/qemu-monitor.sock'
QMP_SOCK = '/home/aaron/src/linux-desktop/qemu-qmp.sock'
SCREEN_W, SCREEN_H = 1920, 1080

def hmp(cmd):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(HMP_SOCK); s.recv(4096)
    s.send((cmd+'\n').encode()); time.sleep(0.15)
    r = s.recv(65536); s.close(); return r

class QMP:
    def __init__(self):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(QMP_SOCK)
        self.f = self.s.makefile('rwb')
        self._read()  # greeting
        self._cmd({"execute":"qmp_capabilities"})
    def _read(self):
        while True:
            line = self.f.readline()
            if not line: return None
            obj = json.loads(line)
            if 'event' in obj: continue
            return obj
    def _cmd(self, c):
        self.f.write((json.dumps(c)+'\n').encode()); self.f.flush()
        return self._read()
    def move(self, x, y):
        # absolute via tablet: value range 0..32767
        ax = int(x * 32767 / SCREEN_W); ay = int(y * 32767 / SCREEN_H)
        self._cmd({"execute":"input-send-event","arguments":{"events":[
            {"type":"abs","data":{"axis":"x","value":ax}},
            {"type":"abs","data":{"axis":"y","value":ay}}]}})
    def click(self, x, y):
        self.move(x,y); time.sleep(0.05)
        self._cmd({"execute":"input-send-event","arguments":{"events":[
            {"type":"btn","data":{"button":"left","down":True}}]}})
        time.sleep(0.08)
        self._cmd({"execute":"input-send-event","arguments":{"events":[
            {"type":"btn","data":{"button":"left","down":False}}]}})
        time.sleep(0.05)
    def drag(self, x0, y0, x1, y1, steps=12):
        self.move(x0,y0); time.sleep(0.05)
        self._cmd({"execute":"input-send-event","arguments":{"events":[
            {"type":"btn","data":{"button":"left","down":True}}]}})
        time.sleep(0.08)
        for i in range(1,steps+1):
            self.move(int(x0+(x1-x0)*i/steps), int(y0+(y1-y0)*i/steps))
            time.sleep(0.03)
        self._cmd({"execute":"input-send-event","arguments":{"events":[
            {"type":"btn","data":{"button":"left","down":False}}]}})
        time.sleep(0.05)

def shot(name, scale=2):
    hmp(f'screendump /tmp/{name}.ppm'); time.sleep(0.4)
    with open(f'/tmp/{name}.ppm','rb') as f:
        f.readline(); wh=f.readline(); f.readline(); d=f.read()
    w,h=map(int,wh.split()); sw,sh=w//scale,h//scale
    rows=[]
    for y in range(sh):
        row=bytearray([0])
        for x in range(sw):
            i=((y*scale)*w+(x*scale))*3; row+=d[i:i+3]
        rows.append(bytes(row))
    raw=b''.join(rows); comp=zlib.compress(raw,9)
    def chunk(t,dd):
        c=struct.pack('>I',len(dd))+t+dd; return c+struct.pack('>I',zlib.crc32(t+dd)&0xffffffff)
    p=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',sw,sh,8,2,0,0,0))+chunk(b'IDAT',comp)+chunk(b'IEND',b'')
    with open(f'/tmp/{name}.png','wb') as f: f.write(p)
    return f'/tmp/{name}.png'

def crop(name, x0, y0, x1, y1, zoom=4):
    """Full-res crop of the live screen, nearest-neighbour zoomed so 1px artifacts are visible."""
    hmp(f'screendump /tmp/{name}_full.ppm'); time.sleep(0.4)
    with open(f'/tmp/{name}_full.ppm','rb') as f:
        f.readline(); wh=f.readline(); f.readline(); d=f.read()
    w,h=map(int,wh.split())
    cw,ch=x1-x0,y1-y0; sw,sh=cw*zoom,ch*zoom
    rows=[]
    for sy in range(sh):
        syy=y0+sy//zoom; row=bytearray([0])
        for sx in range(sw):
            sxx=x0+sx//zoom; i=(syy*w+sxx)*3; row+=d[i:i+3]
        rows.append(bytes(row))
    raw=b''.join(rows); comp=zlib.compress(raw,9)
    def chunk(t,dd):
        c=struct.pack('>I',len(dd))+t+dd; return c+struct.pack('>I',zlib.crc32(t+dd)&0xffffffff)
    p=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',sw,sh,8,2,0,0,0))+chunk(b'IDAT',comp)+chunk(b'IEND',b'')
    with open(f'/tmp/{name}.png','wb') as f: f.write(p)
    return f'/tmp/{name}.png'

if __name__ == '__main__':
    pass
