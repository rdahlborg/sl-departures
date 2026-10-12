"""Record the animations in screenshots/ from a running board.

usage: python tools/animate.py [port] [scene ...]

Each scene drives the board through USB serial with the same keys as the
button ('n' press, 'p' double press, 'h' hold), plus 'B'/'T' for the boot
screen demo, and saves the frames as an animated PNG, scaled 3x like the
other screenshots. Start with the board online, showing Brommaplan on the
green line, with nothing open. Scenes:

  board    boot screen with the train, then the live departure board
  station  choosing Slussen on the red line (leaves the board there)
  phone    opening Mobil... and its QR code
"""
import struct, sys, time, zlib
import serial
from screenshot import grab, chunk

SCALE = 3


def scale(rows):
    out = []
    for r in rows:
        wide = b''.join(r[i:i + 3] * SCALE for i in range(0, len(r), 3))
        out += [wide] * SCALE
    return out


def changed_box(a, b):
    """Bounding box (x0, y0, x1, y1) of the pixels that differ, or None."""
    ys = [y for y in range(len(a)) if a[y] != b[y]]
    if not ys:
        return None
    x0, x1 = len(a[0]) // 3, 0
    for y in ys:
        ra, rb = a[y], b[y]
        first = next(i for i in range(0, len(ra), 3) if ra[i:i + 3] != rb[i:i + 3])
        last = next(i for i in range(len(ra) - 3, -1, -3) if ra[i:i + 3] != rb[i:i + 3])
        x0, x1 = min(x0, first // 3), max(x1, last // 3 + 1)
    return x0, ys[0], x1, ys[-1] + 1


def apng(frames):
    """frames: list of (rows, duration_ms). Only the changed part of each
    frame is stored."""
    merged = []
    for rows, ms in frames:
        if merged and merged[-1][0] == rows:
            merged[-1][1] += ms
        else:
            merged.append([rows, ms])
    w, h = len(merged[0][0][0]) // 3, len(merged[0][0])
    out = [b'\x89PNG\r\n\x1a\n',
           chunk(b'IHDR', struct.pack('>IIBBBBB', w * SCALE, h * SCALE, 8, 2, 0, 0, 0)),
           chunk(b'acTL', struct.pack('>II', len(merged), 0))]
    seq, prev = 0, None
    for rows, ms in merged:
        x0, y0, x1, y1 = (0, 0, w, h) if prev is None else changed_box(prev, rows)
        part = [r[x0 * 3:x1 * 3] for r in rows[y0:y1]]
        data = zlib.compress(b''.join(b'\0' + r for r in scale(part)), 9)
        out.append(chunk(b'fcTL', struct.pack('>IIIIIHHBB', seq, (x1 - x0) * SCALE,
                                              (y1 - y0) * SCALE, x0 * SCALE, y0 * SCALE,
                                              ms, 1000, 0, 0)))
        seq += 1
        if prev is None:
            out.append(chunk(b'IDAT', data))
        else:
            out.append(chunk(b'fdAT', struct.pack('>I', seq) + data))
            seq += 1
        prev = rows
    out.append(chunk(b'IEND', b''))
    return b''.join(out)


class Board:
    def __init__(self, port):
        self.s = serial.Serial(port, 115200, timeout=0.5)
        self.frames = []

    def keys(self, keys, settle=0.5):
        for k in keys:
            self.s.write(k.encode())
            time.sleep(settle)

    def shot(self, ms):
        self.frames.append((grab(self.s)[2], ms))

    def save(self, name):
        path = f'screenshots/{name}.png'
        open(path, 'wb').write(apng(self.frames))
        print(f'saved {path} ({len(self.frames)} frames)')
        self.frames = []


def board_scene(b):
    # The boot screen, stepped frame by frame: two runs of the train.
    b.keys('B')
    run = []
    for i in range(21):
        b.keys('T' if i else '', 0.15)
        run.append((grab(b.s)[2], 80 if i < 20 else 300))
    b.frames += run * 2
    b.keys('B')
    # The board itself; trains glide between updates.
    time.sleep(1)
    for i in range(10):
        b.shot(600)
        time.sleep(0.3)
    b.frames[-1] = (b.frames[-1][0], 2500)
    b.save('board')


def station_scene(b):
    b.shot(1500)
    b.keys('h')                 # line list, on the green line
    b.shot(1000)
    b.keys('n')                 # red line
    b.shot(1000)
    b.keys('h')                 # its stations, from the first
    b.shot(800)
    for i in range(6):          # down to Slussen
        b.keys('n', 0.35)
        b.shot(250 if i < 5 else 1200)
    b.keys('h')                 # choose it
    b.shot(1500)
    time.sleep(4)               # first departures
    b.shot(3000)
    b.save('choose-station')


def phone_scene(b):
    b.shot(1200)
    b.keys('h')
    b.shot(700)
    for i in range(3):          # past the three lines to Mobil...
        b.keys('n', 0.35)
        b.shot(300 if i < 2 else 1200)
    b.keys('h')
    b.shot(4000)
    b.keys('n')                 # close the QR code
    b.save('phone-settings')


SCENES = {'board': board_scene, 'phone': phone_scene, 'station': station_scene}

if __name__ == '__main__':
    port = sys.argv[1] if len(sys.argv) > 1 else '/dev/cu.usbmodem101'
    b = Board(port)
    for name in sys.argv[2:] or SCENES:
        SCENES[name](b)
