"""Original forced read with no DREQ, paused across a restored frame boundary."""
from ..guest_program import Program

RESULT = 0x30000
SOURCE = 0x50000
MAGIC = 0x464F5243


def build():
    p = Program(); p.emit(0x40806000, 0, 0)
    p.constant(16, 0x1F801000); p.constant(17, 0x1F801810)
    p.constant(18, 0xA0000000 | SOURCE); p.constant(20, 0xA0000000 | RESULT)

    def store(base, offset, value):
        p.constant(8, value); p.emit(0xAC080000 | base << 21 | offset)

    def capture(base, offset, destination):
        p.emit(0x8C080000 | base << 21 | offset, 0, 0xAE880000 | destination)

    store(20, 0, MAGIC); store(20, 4, 0)
    store(17, 4, 0); store(17, 0, 0xE2000002)
    p.emit(*([0] * 16))
    store(17, 4, 0x10000002); store(17, 4, 0x04000003)
    capture(17, 0, 8); capture(17, 4, 12)
    for offset in range(0, 16, 4): store(18, offset, 0x55555555)
    store(16, 0xF0, 8 << 8); store(16, 0xF4, 0x00840000)
    store(16, 0xA0, SOURCE); store(16, 0xA4, 4)
    store(16, 0xA8, 0x11700100)
    p.constant(9, 1 << 29)
    p.emit(0x8E0800A8, 0, 0x01094025, 0xAE0800A8)
    capture(16, 0xA0, 16); capture(16, 0xA4, 20); capture(16, 0xA8, 24)
    capture(18, 0, 28); capture(18, 12, 32)
    store(20, 4, 1)
    p.constant(10, 4); p.label('frame')
    store(16, 0x70, 0xFFFFFFFE); p.label('vblank')
    p.emit(0x8E080070, 0, 0x31080001); p.branch(4, 8, 0, 'vblank')
    p.emit(0x254AFFFF); p.branch(5, 10, 0, 'frame')
    p.constant(9, 0xDFFFFFFF)
    p.emit(0x8E0800A8, 0, 0x01094024, 0xAE0800A8)
    p.constant(9, 1 << 24); p.label('poll')
    p.emit(0x8E0800A8, 0, 0x01094024); p.branch(5, 8, 0, 'poll')
    capture(16, 0xA0, 36); capture(16, 0xA4, 40); capture(16, 0xF4, 44)
    capture(18, 0, 48); capture(18, 12, 52); capture(17, 4, 56)
    store(20, 4, 2)
    p.label('halt'); p.branch(4, 0, 0, 'halt')
    return p.finish()
