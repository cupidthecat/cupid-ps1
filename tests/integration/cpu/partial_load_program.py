"""Original load-replacement matrix with live serial and CD response reads."""
from ..guest_program import Program

RESULT = 0x30000
MAGIC = 0x504C4F44
# Instruction, first loaded value, second value bits, preserved register bits.
# The input words are 89ABCDEF and 13579BDF, with initial r9=AABBCCDD.
LOADS = (
    (0x80000000, 0xFFFFFFEF, 0xFFFFFFDF, 0),
    (0x84000000, 0xFFFFCDEF, 0xFFFF9BDF, 0),
    (0x8C000000, 0x89ABCDEF, 0x13579BDF, 0),
    (0x90000000, 0xEF, 0xDF, 0), (0x94000000, 0xCDEF, 0x9BDF, 0),
    (0x88000000, 0xEFBBCCDD, 0xDF000000, 0x00FFFFFF),
    (0x88000001, 0xCDEFCCDD, 0x9BDF0000, 0x0000FFFF),
    (0x88000002, 0xABCDEFDD, 0x579BDF00, 0x000000FF),
    (0x88000003, 0x89ABCDEF, 0x13579BDF, 0),
    (0x98000000, 0x89ABCDEF, 0x13579BDF, 0),
    (0x98000001, 0xAA89ABCD, 0x0013579B, 0xFF000000),
    (0x98000002, 0xAABB89AB, 0x1357, 0xFFFF0000),
    (0x98000003, 0xAABBCC89, 0x13, 0xFFFFFF00),
    (0x40007800, 2, 2, 0),
    (0x48000000, 0x13579BDF, 0x13579BDF, 0),
    (0x4800E000, 0x3DEF, 0x3DEF, 0),
    (0x48402800, 0x2468ACE0, 0x2468ACE0, 0),
)


def expected():
    values = []
    for target in (9, 12, 0):
        for _, first, _, _ in LOADS:
            for _, _, second, keep in LOADS:
                prior = first if target == 9 else 0x55667788
                values.extend((((prior & keep) | second) if target else 0,
                               0xAABBCCDD if target == 9 else first))
    return values


def build(pal=False):
    p = Program(); p.emit(0x40806000, 0, 0)
    p.constant(24, 0x40000000); p.emit(0x40986000, 0, 0)
    p.constant(24, 0x13579BDF); p.emit(0x48980000)
    p.constant(24, 0x3DEF); p.emit(0x4898E000)
    p.constant(24, 0x2468ACE0); p.emit(0x48D82800)
    p.constant(16, 0x1F801800); p.constant(17, 0x1F801000)
    p.constant(19, 0x1F801048); p.constant(20, 0xA0000000 | RESULT)
    p.constant(21, 0x1F801810)

    def store(base, offset, value):
        p.constant(24, value); p.emit(0xAC180000 | base << 21 | offset)

    store(20, 0, MAGIC); store(20, 4, 0); store(20, 8, int(pal))
    store(21, 4, 0); store(21, 4, 0x08000001 | (8 if pal else 0))
    store(17, 0x18, 0x22943)
    p.emit(0xA2000000); p.constant(24, 1); p.emit(0xA2180001, 0xA2180000)
    p.constant(25, 3)
    p.label('response'); p.emit(0x92180003, 0, 0x33180007)
    p.branch(5, 24, 25, 'response')
    p.constant(24, 0x38); p.emit(0xA6780000)
    p.constant(24, 0x1003); p.emit(0xA6780002)
    p.constant(8, 0xA0038000); p.constant(14, 0xA0038004)
    store(8, 0, 0x89ABCDEF); store(8, 4, 0x13579BDF)
    p.constant(21, 0xFFFE0130); store(21, 0, 0x1E988)
    p.constant(10, 0xA0000000 | (RESULT + 40))
    p.constant(22, 0)
    answers = expected()
    p.label('matrix')
    case = 0
    for target in (9, 12, 0):
        for first, _, _, _ in LOADS:
            for second, _, _, _ in LOADS:
                p.constant(9, 0xAABBCCDD); p.constant(12, 0x55667788)
                p.emit(first | (8 << 21 if first >> 26 >= 0x20 else 0) | 9 << 16,
                       second | (14 << 21 if second >> 26 >= 0x20 else 0) | target << 16,
                       0x01206821, 0xAD400000 | target << 16 | case * 8,
                       0xAD4D0000 | (case * 8 + 4))
                # Preserve a transient mismatch across subsequent matrix loops.
                p.constant(24, answers[case * 2])
                p.emit(target << 21 | 24 << 16 | 24 << 11 | 0x26, 0x02D8B025)
                p.constant(24, answers[case * 2 + 1])
                p.emit(13 << 21 | 24 << 16 | 24 << 11 | 0x26, 0x02D8B025)
                case += 1
    # Partial status/flag reads, including rt=zero, must retain the response.
    p.constant(24, 0x12345678)
    p.emit(0x8A190000, 0, 0x9A180003, 0, 0x8A000000, 0, 0x9A000003, 0,
           0xAE98000C, 0x92180000, 0, 0x33180020, 0xAE980010)
    p.constant(24, 0x12345678)
    p.emit(0x9A780002, 0, 0xAE980014)
    p.emit(0xAE960018)
    p.constant(24, 0x12345678); p.emit(0x9AB80002, 0, 0xAE98001C)
    p.constant(24, 0x12345678); p.emit(0x9A380062, 0, 0xAE980020)
    p.emit(0x8E980004, 0, 0x27180001, 0xAE980004)
    p.branch(4, 0, 0, 'matrix')
    return p.finish()
