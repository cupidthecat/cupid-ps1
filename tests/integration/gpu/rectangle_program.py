"""Original rectangle packets, VRAM readback and partially queued restore."""
from ..guest_program import Program

RESULT = 0x30000
PIXELS = 0x31000
MAGIC = 0x5254464C
WIDTH, HEIGHT = 8, 4
# Indexed depths and direct color, four flip settings, both U parities/wrap ends.
CASES = [(depth, flip, uv) for depth in range(3) for flip in range(4) for uv in (0, 1, 254, 255)]


def expected(case):
    depth, flip, uv = case
    pixels = []
    for y in range(HEIGHT):
        for x in range(WIDTH):
            u = (((uv | 1) - x) if flip & 1 else uv + x) & 255
            v = (uv - y if flip & 2 else uv + y) & 255
            if depth == 0:
                index = ((v * 256 + u // 4) >> ((u % 4) * 4)) & 15
                color = index * 1057
            elif depth == 1:
                index = ((v * 256 + u // 2) >> ((u % 2) * 8)) & 255
                color = (index & 31) | ((index & 31) << 5) | ((index & 31) << 10)
            else:
                color = v * 256 + u
            pixels.append(color)
    return tuple(pixels)


def build():
    p = Program(); p.emit(0x40806000, 0, 0)
    p.constant(16, 0x1F801810); p.constant(17, 0x1F801110)
    p.constant(20, 0xA0000000 | RESULT)

    def store(base, offset, value):
        p.constant(8, value); p.emit(0xAC080000 | base << 21 | offset)

    def gp0(value):
        store(16, 0, value); p.emit(*([0] * 16))

    def wait(label):
        p.constant(9, 1 << 26); p.label(label)
        p.emit(0x8E080004, 0, 0x01094024); p.branch(4, 8, 0, label)

    store(20, 0, MAGIC); store(20, 4, 0); store(20, 8, len(CASES)); store(20, 12, 0)
    store(16, 4, 0); gp0(0xE3000000); gp0(0xE407FFFF); gp0(0xE5000000); gp0(0xE6000000)
    # Only rows addressed by the boundary cases, each containing (V << 8) | U.
    for row in (*range(5), *range(251, 256)):
        gp0(0xA0000000); gp0(640 | (256 + row) << 16); gp0(256 | 1 << 16)
        p.constant(10, row * 256); p.constant(11, (row + 1) * 256)
        p.label(f'texture-{row}')
        p.emit(0x254C0001, 0x000C6400, 0x018A6025, 0xAE0C0000, 0x254A0002)
        p.branch(5, 10, 11, f'texture-{row}')
        wait(f'texture-ready-{row}')
    gp0(0xA0000000); gp0(400 << 16); gp0(256 | 1 << 16)
    for index in range(0, 256, 2):
        gp0((index & 31) * 1057 | (((index + 1) & 31) * 1057) << 16)
    wait('palette-ready')
    store(20, 0x20, 0xA0000000 | PIXELS); p.emit(0x8E930020, 0)
    for index, (depth, flip, uv) in enumerate(CASES):
        gp0(0x02000000); gp0(32 | 24 << 16); gp0(16 | HEIGHT << 16)
        gp0(0xE1000000 | 10 | 16 | depth << 7 | flip << 12)
        gp0(0x65808080); gp0(32 | 24 << 16)
        if index == 0:
            # The frame-3 checkpoint occurs while this packet lacks UV/size.
            store(20, 4, 1)
            store(17, 4, 0x100); store(17, 0, 0)
            p.constant(9, 450); p.label('pause')
            p.emit(0x8E280000, 0, 0x0109402B); p.branch(5, 8, 0, 'pause')
        gp0(uv | uv << 8 | (400 << 6) << 16); gp0(WIDTH | HEIGHT << 16)
        wait(f'draw-ready-{index}')
        gp0(0xC0000000); gp0(32 | 24 << 16); gp0(WIDTH | HEIGHT << 16)
        for offset in range(WIDTH * HEIGHT // 2):
            p.emit(0x8E0A0000, 0, 0xAE6A0000 | offset * 4)
        p.emit(0x26730000 | (WIDTH * HEIGHT * 2))
        store(20, 12, index + 1)
    store(20, 4, 2)
    p.label('halt'); p.branch(4, 0, 0, 'halt')
    return p.finish()
