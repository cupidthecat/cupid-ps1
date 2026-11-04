"""Original channel-6 guest, including a transfer pending at the save boundary."""
from ..guest_program import Program

RESULT = 0x30000
MAGIC = 0x4F544343
TABLE = 0x50000
CASES = [0x11000000 | sync << 9 | direction | reverse << 1 | chop << 8 | snoop << 30
         for snoop in range(2) for chop in range(2) for reverse in range(2)
         for sync in range(4) for direction in range(2)]


def build():
    p = Program(); p.emit(0x40806000, 0, 0)
    p.constant(16, 0x1F801000); p.constant(17, 0x1F801110)
    p.constant(19, 0xA0000000 | TABLE); p.constant(20, 0xA0000000 | RESULT)

    def store(base, offset, value):
        p.constant(8, value); p.emit(0xAC080000 | base << 21 | offset)

    def capture(base, offset, destination):
        p.emit(0x8C080000 | base << 21 | offset, 0, 0xAE880000 | destination)

    store(20, 0, MAGIC); store(20, 4, 0); store(20, 8, len(CASES)); store(20, 12, 0)
    p.emit(0x8E880000, 0, 0xAE880000)
    for index, control in enumerate(CASES):
        for i in range(4): store(19, i * 4, 0x44444444)
        store(16, 0xF0, 0)
        store(16, 0xE0, TABLE + 12); store(16, 0xE4, 4)
        store(16, 0xE8, control)
        if index == 0:
            store(20, 4, 1)
            store(17, 4, 0x100); store(17, 0, 0)
            p.constant(9, 450); p.label('pause')
            p.emit(0x8E280000, 0, 0x0109402B); p.branch(5, 8, 0, 'pause')
        offset = 16 + index * 32
        capture(16, 0xE8, offset)
        store(16, 0xF0, 8 << 24)
        capture(16, 0xE8, offset + 4)
        capture(16, 0xE0, offset + 8); capture(16, 0xE4, offset + 12)
        for i in range(4): capture(19, i * 4, offset + 16 + i * 4)
        store(20, 12, index + 1)
    store(20, 4, 2)
    p.label('halt'); p.branch(4, 0, 0, 'halt')
    return p.finish()
