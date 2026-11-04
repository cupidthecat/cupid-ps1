"""Original GP0 polylines spanning decoder, batch and staging boundaries."""
from ..guest_program import Program

RESULT, PIXELS, MAGIC = 0x30000, 0x31000, 0x504C494E
CASES = [(shaded, count) for shaded in (False, True) for count in (3, 2, 600, 2049, 5000)]


def point(index):
    return (32 + index % 100) | (24 + index // 100) << 16


def build():
    p = Program(); p.emit(0x40806000, 0, 0)
    p.constant(16, 0x1F801810); p.constant(17, 0x1F801110); p.constant(20, 0xA0000000 | RESULT)

    def store(base, offset, value):
        p.constant(8, value); p.emit(0xAC080000 | base << 21 | offset)

    def gp0(value):
        store(16, 0, value); p.emit(*([0] * 16))

    def ready(label):
        p.constant(9, 1 << 26); p.label(label)
        p.emit(0x8E080004, 0, 0x01094024); p.branch(4, 8, 0, label)

    store(20, 0, MAGIC); store(20, 4, 0); store(20, 8, len(CASES)); store(20, 12, 0)
    store(20, 0x20, 0xA0000000 | PIXELS); p.emit(0x8E930020, 0)
    store(16, 4, 0); gp0(0xE3000000); gp0(0xE407FFFF)
    for index, (shaded, count) in enumerate(CASES):
        gp0(0x02000000); gp0(0); gp0(256 | 128 << 16); ready(f'clear-{index}')
        gp0(0x580000FF if shaded else 0x480000FF)
        prefix = 3 if index == 0 else 1
        for vertex in range(prefix):
            if shaded and vertex: gp0(255)
            gp0(point(vertex))
        if index == 0:
            store(20, 4, 1); store(17, 4, 0x100); store(17, 0, 0)
            p.constant(9, 450); p.label('pause')
            p.emit(0x8E280000, 0, 0x0109402B); p.branch(5, 8, 0, 'pause')
        if count > prefix:
            p.constant(10, count - prefix); p.constant(11, 32 + prefix)
            p.constant(12, 24); p.constant(13, 132); p.constant(14, 255)
            p.label(f'vertices-{index}')
            if shaded: p.emit(0xAE0E0000)
            p.emit(0x000C4400, 0x010B4025, 0xAE080000, 0x256B0001)
            p.branch(5, 11, 13, f'next-{index}')
            p.constant(11, 32); p.emit(0x258C0001)
            p.label(f'next-{index}'); p.emit(0x254AFFFF)
            p.branch(5, 10, 0, f'vertices-{index}')
        gp0(0x50005000); ready(f'draw-{index}')
        for sample, vertex in enumerate((0, count - 1)):
            gp0(0xC0000000); gp0(point(vertex)); gp0(1 | 1 << 16)
            p.emit(0x8E080000, 0, 0xAE680000 | sample * 4)
        p.emit(0x26730008); store(20, 12, index + 1)
    store(20, 4, 2)
    p.label('halt'); p.branch(4, 0, 0, 'halt')
    return p.finish()
