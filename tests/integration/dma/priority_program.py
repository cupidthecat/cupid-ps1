"""Original guest exposing all GPU/OTC priority pairs through shared RAM."""
from ..guest_program import Program

RESULT = 0x30000
ROWS = RESULT + 0x100
SOURCE = 0x50000
MAGIC = 0x5052494F
PIXEL = 0x23456789
CASES = [(read, gpu, otc) for read in (True, False) for gpu in range(8) for otc in range(8)]


def build():
    p = Program(); p.emit(0x40806000, 0, 0)
    p.constant(16, 0x1F801000); p.constant(17, 0x1F801810)
    p.constant(18, 0xA0000000 | SOURCE); p.constant(20, 0xA0000000 | RESULT)

    def store(base, offset, value):
        p.constant(8, value); p.emit(0xAC080000 | base << 21 | offset)

    def gp0(value):
        store(17, 0, value); p.emit(*([0] * 16))

    def capture(base, offset, destination):
        p.emit(0x8C080000 | base << 21 | offset, 0, 0xAE880000 | destination)

    store(20, 0, MAGIC); store(20, 4, 0); store(20, 8, len(CASES)); store(20, 12, 0)
    store(17, 4, 0)
    for index, (read, gpu, otc) in enumerate(CASES):
        store(16, 0xF0, 0); store(16, 0xF4, 0x7FC40000)
        store(17, 4, 0x02000000)
        store(17, 4, 0x04000003 if read else 0x04000002)
        if read:
            gp0(0xA0000000); gp0(0); gp0(0x00010002); gp0(PIXEL)
            gp0(0xC0000000); gp0(0); gp0(0x00010002)
        store(18, 0, 0x1F000000)
        store(16, 0xA0, SOURCE); store(16, 0xA4, 1)
        store(16, 0xA8, 0x11000000 if read else 0x11000001)
        store(16, 0xE0, SOURCE); store(16, 0xE4, 1); store(16, 0xE8, 0x11000000)
        if index == 0:
            # The first pair is armed with both master enables clear at frame 3.
            store(20, 4, 1)
            p.constant(10, 4); p.label('frame')
            store(16, 0x70, 0xFFFFFFFE); p.label('vblank')
            p.emit(0x8E080070, 0, 0x31080001); p.branch(4, 8, 0, 'vblank')
            p.emit(0x254AFFFF); p.branch(5, 10, 0, 'frame')
        store(16, 0xF0, (gpu | 8) << 8 | (otc | 8) << 24)
        offset = ROWS - RESULT + index * 20
        capture(18, 0, offset); capture(17, 4, offset + 4)
        capture(16, 0xF4, offset + 8); capture(16, 0xA8, offset + 12)
        capture(16, 0xE8, offset + 16); store(20, 12, index + 1)
    store(20, 4, 2)
    p.label('halt'); p.branch(4, 0, 0, 'halt')
    return p.finish()
