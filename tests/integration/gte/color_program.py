"""Original GTE boundary, color-pipeline and signed-translation programs."""
from ..guest_program import Program

RESULT = 0x80000
CAPTURES = 0x82000
STRIDE = 64
MAGIC = 0x47544543
REGISTERS = (20, 21, 22, 25, 26, 27, 9, 10, 11, 63)
FIFO = (0x11010203, 0x22040506, 0x33070809)
VALUES = (-2147483648, -32769, -32768, -4096, -17, -16, -15, -1,
          0, 1, 15, 16, 4079, 4080, 4095, 4096, 32767, 32768, 2147483647)
COLOR_COMMANDS = (0x10, 0x11, 0x13, 0x14, 0x16, 0x1B, 0x1C,
                  0x1E, 0x20, 0x29, 0x2A, 0x3D, 0x3E, 0x3F)
CASES = [('boundary', i, sf, lm) for i in range(len(VALUES)) for sf in range(2) for lm in range(2)]
CASES += [('color', op, sf, lm) for op in COLOR_COMMANDS for sf in range(2) for lm in range(2)]
CASES += [('translation', op, sf, lm) for op in (1, 0x12, 0x30) for sf in range(2) for lm in range(2)]


def inputs(case):
    kind, value, sf, lm = case
    registers = {6: 0xA5000000, **{20 + i: value for i, value in enumerate(FIFO)}}
    if kind == 'boundary':
        registers.update({25 + i: VALUES[(value + i * 5) % len(VALUES)] for i in range(3)})
        op = 0x3E
    elif kind == 'color':
        op = value
        depth_product = op in (0x13, 0x14, 0x16, 0x29)
        registers[6] = 0xA5000000 if depth_product else 0xA5101010
        registers[8] = 0 if op in (0x11, 0x3E) else 4096
        for base in (9, 25, 45, 53): registers.update({base + i: -1 for i in range(3)})
    else:
        op = value
        registers.update({37 + i: value for i, value in enumerate((-3, -5, -9))})
    return 0x4A000000 | op | sf << 19 | lm << 10, registers


def boundary_expected(case):
    _, sample, _, lm = case
    values = [VALUES[(sample + i * 5) % len(VALUES)] for i in range(3)]
    colors = [min(255, max(0, value // 16)) for value in values]
    rgb = 0xA5000000 | colors[0] | colors[1] << 8 | colors[2] << 16
    flags, ir = 0, []
    for i, value in enumerate(values):
        clamped = min(32767, max(0 if lm else -32768, value))
        ir.append(clamped)
        if value != clamped: flags |= 1 << (24 - i)
        if value < 0 or value > 4095: flags |= 1 << (21 - i)
    if flags & 0x7F87E000: flags |= 0x80000000
    return tuple(value & 0xFFFFFFFF for value in (FIFO[1], FIFO[2], rgb, *values, *ir, flags))


def build():
    p = Program(); p.constant(8, 1 << 30); p.emit(0x40886000, 0, 0)
    p.constant(20, 0xA0000000 | RESULT)

    def store(offset, value):
        p.constant(8, value & 0xFFFFFFFF); p.emit(0xAE880000 | offset)

    store(0, MAGIC); store(4, 1); store(8, len(CASES)); store(12, 0)
    store(0x24, 0xA0090000)
    p.emit(0x8E950024, 0)
    p.constant(8, 0xA0000000 | CAPTURES); p.emit(0xAEA80000, 0x8EB30000, 0)
    for index, case in enumerate(CASES):
        p.constant(8, 0)
        for register in range(64):
            p.emit((0x48880000 if register < 32 else 0x48C80000) | (register % 32) << 11)
        opcode, values = inputs(case)
        for register, value in sorted(values.items()):
            p.constant(8, value & 0xFFFFFFFF)
            p.emit((0x48880000 if register < 32 else 0x48C80000) | (register % 32) << 11)
        p.emit(opcode)
        for i, register in enumerate(REGISTERS):
            p.emit((0x48080000 if register < 32 else 0x48480000) | (register % 32) << 11,
                   0, 0xAE680000 | i * 4)
        store(12, index + 1)
        p.emit(0x26730000 | STRIDE)
    store(4, 2)
    p.label('halt'); p.branch(4, 0, 0, 'halt')
    return p.finish()
