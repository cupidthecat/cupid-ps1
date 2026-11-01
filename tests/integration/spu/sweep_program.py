"""Original guest MMIO checks for stereo gain readback and completed fades."""
from ..guest_program import Program

RESULT_ADDRESS = 0x30040
RESULT_MAGIC = 0x53575053


def build():
    program = Program()
    program.constant(16, 0x1F801C00); program.constant(18, 0xA0030000)
    program.emit(0xAE500000)
    program.constant(8, 0xA0000000 + RESULT_ADDRESS)
    program.emit(0xAE480004, 0x8E500000, 0x8E510004, 0)
    for register, value in [(0, 0x1000), (2, 0x2000), (0x180, 0x30), (0x182, 0x20)]:
        program.constant(8, value); program.emit(0xA6080000 | register)
    for register, value in [(0x200, 0x2000), (0x202, 0x4000), (0x1B8, 0x60), (0x1BA, 0x40)]:
        label = 'ready-' + str(register)
        program.constant(9, value); program.label(label)
        program.emit(0x96080000 | register, 0, 0x01095026)
        program.branch(5, 10, 0, label)
    program.emit(0x8E080200, 0, 0xAE280004, 0x8E0801B8, 0, 0xAE280008)
    for register, value in [(0, 0xA000), (2, 0xA000), (0x180, 0xA02C), (0x182, 0xA02C)]:
        program.constant(8, value); program.emit(0xA6080000 | register)
    program.label('fade')
    program.emit(0x96080200, 0x96090202, 0, 0x01094025,
                 0x960901B8, 0, 0x01094025, 0x960901BA, 0, 0x01094025)
    program.branch(5, 8, 0, 'fade')
    program.emit(0x8E080200, 0, 0xAE28000C, 0x8E0801B8, 0, 0xAE280010)
    program.constant(8, RESULT_MAGIC); program.emit(0xAE280000)
    program.label('done'); program.emit(0x8E280004, 0, 0xAE280018)
    program.branch(4, 0, 0, 'done')
    return program.finish()
