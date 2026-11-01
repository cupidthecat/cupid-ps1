"""Original GPU information queries and saved read-latch continuation."""
from ..guest_program import Program

RESULT = 0x80000
ROWS = RESULT + 0x100
MAGIC = 0x494E464F
SEED, WINDOW = 0x12345, 0x54321
TOP = 0x12 | (0x34 << 10)
BOTTOM = 0x312 | (0x134 << 10)
OFFSET = 0x7FD | (0x7FB << 11)
CASES = [(command, index) for command in range(0x10, 0x20) for index in range(32)]


def expected(index):
    return (SEED, SEED, WINDOW, TOP, BOTTOM, OFFSET, SEED, 2,
            0, SEED, SEED, SEED, SEED, SEED, SEED, SEED)[index & 15]


def build():
    p = Program(); p.emit(0x40806000, 0, 0)
    p.constant(16, 0x1F801810); p.constant(17, 0x1F801000)
    p.constant(20, 0xA0000000 | RESULT); p.constant(21, 0xA0000000 | ROWS)

    def store(base, offset, value):
        p.constant(8, value); p.emit(0xAC080000 | base << 21 | offset)

    def capture(base, offset, destination, target=20):
        p.emit(0x8C080000 | base << 21 | offset, 0,
               0xAC080000 | target << 21 | destination)

    store(20, 0, MAGIC); store(20, 4, 1)
    store(20, 8, len(CASES)); store(20, 12, 0)
    for offset in range(16, 36, 4): store(20, offset, 0)
    store(16, 4, 0); store(16, 4, 0x04000003)
    store(16, 4, 0x10000007)
    store(16, 0, 0xE2000000 | WINDOW)
    capture(16, 4, 24)
    p.constant(10, 4); p.label('frame')
    store(17, 0x70, 0xFFFFFFFE); p.label('vblank')
    p.emit(0x8E280070, 0, 0x31080001); p.branch(4, 8, 0, 'vblank')
    p.emit(0x254AFFFF); p.branch(5, 10, 0, 'frame')
    capture(16, 0, 16)
    store(16, 4, 0x1000000A); capture(16, 0, 20); capture(16, 4, 28)
    # Include an ordinary RAM read as well as writes in LUT execution coverage.
    capture(20, 0, 32)
    p.constant(19, 0); p.constant(22, len(CASES)); p.constant(23, 0x765400)
    p.label('query')
    # Command = 10h + (case >> 5), index = case & 31.
    p.emit(0x00139142, 0x26520010, 0x00129600, 0x3269001F,
           0x02499025, 0x02579025)
    # Clear the latch's unused bits before seeding the documented fields.
    store(16, 4, 0x10000008)
    store(16, 0, 0xE2000000 | SEED); store(16, 4, 0x10000002)
    store(16, 0, 0xE2000000 | WINDOW)
    store(16, 0, 0xE3000000 | TOP); store(16, 0, 0xE4000000 | BOTTOM)
    store(16, 0, 0xE5000000 | OFFSET)
    p.emit(0xAEB20000); capture(16, 0, 4, 21)
    p.emit(0xAE120004)
    capture(16, 0, 8, 21); capture(16, 0, 12, 21)
    store(16, 0, 0xE2000000)
    capture(16, 0, 16, 21)
    p.emit(0x26B50014, 0x26730001, 0xAE93000C)
    p.branch(5, 19, 22, 'query')
    store(20, 4, 2)
    p.label('halt'); p.branch(4, 0, 0, 'halt')
    return p.finish()
