"""Original guest with a chopped transfer spanning saved frame boundaries."""
from ..guest_program import Program

RESULT = 0x30000
SOURCE = 0x50000
MAGIC = 0x43484F50


def build():
    p = Program(); p.emit(0x40806000, 0, 0)
    p.constant(16, 0x1F801000); p.constant(17, 0x1F801810)
    p.constant(18, 0xA0000000 | SOURCE); p.constant(20, 0xA0000000 | RESULT)

    def store(base, offset, value):
        p.constant(8, value); p.emit(0xAC080000 | base << 21 | offset)

    def capture(base, offset, destination):
        p.emit(0x8C080000 | base << 21 | offset, 0, 0xAE880000 | destination)

    store(20, 0, MAGIC); store(20, 4, 0)
    store(18, 0, 0x1F000000)
    p.constant(19, 0xA0000000 | (SOURCE + 65535 * 4)); store(19, 0, 0x1F000000)
    store(16, 0xF0, 8 << 8); store(16, 0xF4, 0x00840000)
    store(17, 4, 0); store(17, 4, 0x04000002)
    store(16, 0xA0, SOURCE); store(16, 0xA4, 0)
    store(16, 0xA8, 0x11700101)  # One word, 128 CPU clocks, 65,536 words.
    p.emit(0x8E0A00A0, 0x8E0B00A4, 0, 0xAE8A000C, 0xAE8B0010)
    capture(17, 4, 8)  # The first burst has already reached the GPU.
    store(17, 4, 0x02000000)
    store(20, 4, 1)
    p.emit(0x8E880000, 0, 0xAE880000)  # Retain a RAM load in every native configuration.
    p.constant(9, 1 << 24); p.label('poll')
    p.emit(0x8E0800A8, 0, 0x01094024); p.branch(5, 8, 0, 'poll')
    capture(16, 0xA0, 20); capture(16, 0xA4, 24)
    capture(16, 0xF4, 28); capture(17, 4, 32)
    store(20, 4, 2)
    p.label('halt'); p.branch(4, 0, 0, 'halt')
    return p.finish()
