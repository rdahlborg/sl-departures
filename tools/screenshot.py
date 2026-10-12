"""Grab a screenshot from the board over USB serial and save it as PNG.

usage: python tools/screenshot.py [port] [out.png]
"""
import struct, sys, time, zlib
import serial


def grab(s):
    """Capture the screen. Returns (width, height, rows of RGB bytes)."""
    s.reset_input_buffer()
    s.write(b'S')
    # The board redraws the screen and prints each strip as it is flushed:
    # SNAP-AREA x1 y1 x2 y2 followed by one SNAP: line of hex RGB565 per row.
    size, fb, area, y, deadline = None, None, None, 0, time.time() + 30
    while time.time() < deadline:
        line = s.readline().decode(errors='replace').strip()
        if line.startswith('SNAP-ERR'):
            sys.exit(line)
        if line.startswith('SNAP-BEGIN'):
            size = tuple(map(int, line.split()[1:3]))
            fb = [bytearray(size[0] * 3) for _ in range(size[1])]
        elif line.startswith('SNAP-AREA') and size:
            area = tuple(map(int, line.split()[1:5]))
            y = area[1]
        elif line.startswith('SNAP:') and area:
            # Log lines from other tasks can land mid-row; keep the clean prefix.
            hexpart = line[5:]
            n = next((i for i, ch in enumerate(hexpart) if ch not in '0123456789abcdef'),
                     len(hexpart))
            row = bytes.fromhex(hexpart[:n - n % 2])
            for i in range(0, len(row) - 1, 2):
                v = row[i] | row[i + 1] << 8       # RGB565, little endian
                x = area[0] + i // 2
                if 0 <= x < size[0] and 0 <= y < size[1]:
                    fb[y][x * 3:x * 3 + 3] = bytes((((v >> 11) & 31) * 255 // 31,
                                                    ((v >> 5) & 63) * 255 // 63,
                                                    (v & 31) * 255 // 31))
            y += 1
        elif 'SNAP-END' in line and size:
            return size[0], size[1], [bytes(r) for r in fb]
    sys.exit('timed out waiting for screenshot')


def chunk(tag, data):
    c = tag + data
    return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c))


def png(w, h, rows):
    raw = b''.join(b'\0' + r for r in rows)  # PNG filter: none
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


if __name__ == '__main__':
    port = sys.argv[1] if len(sys.argv) > 1 else '/dev/cu.usbmodem101'
    out = sys.argv[2] if len(sys.argv) > 2 else 'screenshot.png'
    w, h, rows = grab(serial.Serial(port, 115200, timeout=0.5))
    open(out, 'wb').write(png(w, h, rows))
    print(f'saved {out} ({w}x{h})')
