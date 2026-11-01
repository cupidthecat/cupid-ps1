"""Original serial partial stores and retained CD response data."""
from ..guest_program import Program

RESULT = 0x30000
CAPTURES = 0x32000
MAGIC = 0x50415254
SOURCES = (0xAABB0C01, 0xFEDC2202)
MODES = (0, 0x13F, 0x120)
CONTROLS = (0, 0xC00, 0x2100)
VALUES = (((0x0C01, 0x0100, 0x0C01, 0x0100), (0xAA, 0xAABB, 0xBB0C, 0x0C01)),
          ((0x2202, 0x0200, 0x2202, 0x0200), (0xFE, 0xFEDC, 0xDC22, 0x2202)))
CASES = [(port, left, offset, baseline, source) for port in range(2) for left in range(2)
         for offset in range(4) for baseline in range(3) for source in range(2)]


def expected(case):
    port, left, offset, baseline, source = case
    upper = not left and offset >= 2
    return ((MODES[baseline] if upper else VALUES[source][left][offset]) & (0xFF if port else 0x13F),
            VALUES[source][left][offset] if upper else CONTROLS[baseline])


def build():
    p = Program(); p.emit(0x40806000, 0, 0, 0)
    p.constant(16, 0x1F801800); p.constant(18, 0x1F801018)
    p.constant(19, 0x1F801110); p.constant(21, 0xA0000000 | RESULT)

    def store(base, offset, value):
        p.constant(8, value); p.emit(0xAC080000 | base << 21 | offset)

    def byte(offset, value):
        p.constant(8, value); p.emit(0xA2080000 | offset)

    def wait_irq(label):
        byte(0, 1); p.label(label)
        p.emit(0x92080003, 0, 0x31080007); p.branch(4, 8, 0, label)

    def getstat(label):
        byte(0, 0); byte(1, 1); wait_irq(label)

    store(21, 0, MAGIC); store(21, 8, len(CASES)); store(21, 0x10, 0xFFFFFFFF)
    store(21, 0x24, 0xA0031000); store(21, 0x28, 0xA0000000 | CAPTURES)
    p.emit(0x8EB40024, 0, 0x8EB10028, 0)
    store(20, 0, SOURCES[0]); store(20, 4, SOURCES[1])
    store(18, 0, 0x22943)
    getstat('first-stat')
    p.emit(0x92080000, 0, 0xAEA8000C)
    store(21, 4, 1)
    store(19, 4, 0x100); store(19, 0, 0)
    p.constant(9, 450); p.label('pause')
    p.emit(0x8E680000, 0, 0x0109402B); p.branch(5, 8, 0, 'pause')
    p.constant(9, 0); p.emit(0xBA090003)
    p.emit(0x92080000, 0, 0xAEA80010, 0x92080001, 0, 0xAEA80014)
    byte(3, 0x1F)
    getstat('second-stat')
    p.constant(9, 0); p.emit(0xAA090000)
    p.emit(0x92080000, 0, 0xAEA80018, 0x92080001, 0, 0xAEA8001C)
    byte(0, 1); byte(3, 0x1F)

    for port, left, offset, baseline, source in CASES:
        store(21, 0x20, 0x1F801058 if port else 0x1F801048)
        p.emit(0x8EB60020, 0)
        p.constant(14, MODES[baseline]); p.emit(0xA6CE0000)
        p.constant(15, CONTROLS[baseline]); p.emit(0xA6CF0002)
        p.emit(0x8E890000 | source * 4, 0, (0xAAC90000 if left else 0xBAC90000) | offset)
        p.emit(0x96CC0000, 0x96CD0002, 0, 0xAE2C0000, 0xAE2D0004, 0x26310008)

    # A held byte bus routes both enabled bytes to the parameter port.
    store(18, 0, 0x20943); byte(0, 0)
    p.constant(9, 0x3412); p.emit(0xBA090002)
    byte(1, 0x0D); wait_irq('set-filter'); p.emit(0xAEA80030, 0x92080001, 0)
    byte(3, 0x1F)
    byte(0, 0); byte(1, 0x0F); wait_irq('get-mode')
    p.emit(0xAEA80034)
    for i in range(5): p.emit(0x92080001, 0, 0xA2A80040 | i)
    p.emit(0x92080000, 0, 0xAEA80048)
    byte(3, 0x1F)
    store(21, 4, 2)
    p.label('halt'); p.branch(4, 0, 0, 'halt')
    return p.finish()
