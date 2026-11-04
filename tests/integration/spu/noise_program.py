"""Shared noise at different voice pitches, with dynamic RAM and MMIO stores."""
from ..guest_program import Program

CLOCK = 17


def build():
    program = Program()
    # Select the clock before the first 768-tick audio sample.
    program.constant(16, 0x1F801C00); program.constant(8, 0xC000 | CLOCK << 8)
    program.emit(0xA60801AA)
    # Reload addresses from RAM so native slow/LUT/mapped paths actually execute.
    program.constant(18, 0xA0030000); program.emit(0xAE500000)
    program.constant(8, 0xA0030040); program.emit(0xAE480004, 0x8E500000, 0x8E510004, 0)
    program.constant(8, 0x1357); program.emit(0xAE280004)
    for voice, pitch in [(1, 0x400), (3, 0x3000)]:
        for register, value in [(0, 0x3FFF), (2, 0x3FFF), (4, pitch), (6, 0x200), (8, 0xF), (10, 0)]:
            program.constant(8, value); program.emit(0xA6080000 | voice * 16 | register)
    for register, value in [(0x180, 0x3FFF), (0x182, 0x3FFF), (0x194, 0xA), (0x188, 0xA)]:
        program.constant(8, value); program.emit(0xA6080000 | register)
    program.label('loop'); program.emit(0x8E280004, 0, 0xAE280008)
    program.branch(4, 0, 0, 'loop')
    return program.finish()
