"""Original generated firmware for guest SIO0 card transactions."""
import struct

SECTOR = 0x123
CONTENT = bytes((i * 29 + 7) & 255 for i in range(128))
RESULT_ADDRESS = 0x30000
RESULT_MAGIC = 0x4D435458


class Program:
    def __init__(self):
        self.words, self.labels, self.fixups = [], {}, []

    def emit(self, *words):
        self.words.extend(words)

    def constant(self, register, value):
        self.emit(0x3C000000 | register << 16 | (value >> 16),
                  0x34000000 | register << 21 | register << 16 | (value & 0xFFFF))

    def label(self, name):
        self.labels[name] = len(self.words)

    def branch(self, opcode, rs, rt, name):
        self.fixups.append((len(self.words), 'branch', name))
        self.emit(opcode << 26 | rs << 21 | rt << 16, 0)

    def call(self, name):
        self.fixups.append((len(self.words), 'call', name))
        self.emit(0x0C000000, 0)

    def finish(self):
        for position, kind, name in self.fixups:
            target = self.labels[name]
            if kind == 'call': self.words[position] |= ((0xBFC00000 + target * 4) >> 2) & 0x3FFFFFF
            else:
                offset = target - position - 1
                assert -32768 <= offset <= 32767
                self.words[position] |= offset & 0xFFFF
        return self.words


def checksum(address, data):
    value = (address >> 8) ^ (address & 255)
    for byte in data: value ^= byte
    return value


def write_packet(flag, data=CONTENT, address=SECTOR, *, bad=False, partial=False):
    sent = [0x81, 0x57, 0, 0, address >> 8, address & 255]
    replies = [0xFF, flag, 0x5A, 0x5D, 0, address >> 8]
    data = data[:65] if partial else data
    sent.extend(data)
    replies.extend([address & 255, *data[:-1]])
    if not partial:
        sent.extend([checksum(address, data) ^ int(bad), 0, 0, 0])
        replies.extend([data[-1], 0x5C, 0x5D, 0x4E if bad else (0x47 if address < 1024 else 0xFF)])
    return list(zip(sent, replies, [True] * (len(sent) - 1) + [partial]))


def read_packet(flag, address=SECTOR):
    sent = [0x81, 0x52, 0, 0, address >> 8, address & 255, 0, 0, 0, 0]
    replies = [0xFF, flag, 0x5A, 0x5D, 0, address >> 8, 0x5C, 0x5D]
    if address >= 1024: replies.extend([0xFF, 0xFF])
    else:
        sent.extend([0] * 130)
        replies.extend([address >> 8, address & 255, *CONTENT, checksum(address, CONTENT), 0x47])
    return list(zip(sent, replies, [True] * (len(sent) - 1) + [False]))


def build(writer=True):
    packets = [read_packet(8)]
    if writer:
        alternate = bytes(value ^ 0x5A for value in CONTENT)
        packets = [write_packet(8), read_packet(0), write_packet(0, alternate, bad=True), read_packet(0),
                   write_packet(0, alternate, SECTOR + 1024), read_packet(0),
                   write_packet(0, alternate, partial=True), read_packet(0), read_packet(0, SECTOR + 1024)]
        packets.append(list(zip([0x81, 0x53] + [0] * 8,
                                [0xFF, 0, 0x5A, 0x5D, 0x5C, 0x5D, 4, 0, 0, 0x80],
                                [True] * 9 + [False])))
    program = Program()
    # s0=SIO0, s1=response output; t7=CTRL with IRQ acknowledge.
    program.constant(16, 0x1F801040)
    program.constant(17, 0xA0000000 + RESULT_ADDRESS + 16)
    program.emit(0x24080040, 0xA608000A, 0x2408000D, 0xA6080008,
                 0x24080088, 0xA608000E, 0x240F1013)
    expected = bytearray()
    for packet in packets:
        program.emit(0xA600000A, 0x24081003, 0xA608000A)
        for sent, reply, ack in packet:
            program.emit(0x24040000 | sent, 0x24050000 | int(ack))
            program.call('transfer')
            expected.extend([reply, int(ack)])
    program.emit(0xA600000A)
    program.constant(8, RESULT_MAGIC)
    program.constant(9, 0xA0000000 + RESULT_ADDRESS)
    program.emit(0xAD280000)
    program.constant(8, len(expected))
    program.emit(0xAD280004)
    program.label('done'); program.branch(4, 0, 0, 'done')

    program.label('transfer')
    program.emit(0xA60F000A, 0xA2040000)  # Clear prior IRQ and send a0.
    program.label('receive')
    program.emit(0x8E080004, 0, 0x31090002)
    program.branch(4, 9, 0, 'receive')
    program.emit(0x92020000, 0, 0xA2220000)
    program.branch(4, 5, 0, 'no_ack')
    program.label('ack')
    program.emit(0x8E080004, 0, 0x31090200)
    program.branch(4, 9, 0, 'ack')
    program.branch(4, 0, 0, 'record')
    program.label('no_ack')
    # Observe beyond the normal ACK delay; this also catches a spurious final IRQ.
    program.emit(0x24090080)
    program.label('delay'); program.emit(0x2529FFFF)
    program.branch(5, 9, 0, 'delay')
    program.emit(0x8E080004, 0)
    program.label('record')
    program.emit(0x00084242, 0x31080001, 0xA2280001, 0x26310002, 0x03E00008, 0)
    return program.finish(), bytes(expected)


def write_inputs(bios, executable, words):
    firmware = bytearray(512 * 1024)
    struct.pack_into('<' + 'I' * len(words), firmware, 0, *words)
    bios.write_bytes(firmware)
    image = bytearray(2048 + len(words) * 4)
    image[:8] = b'PS-X EXE'
    struct.pack_into('<4I', image, 16, 0x80010000, 0, 0x80010000, len(words) * 4)
    struct.pack_into('<' + 'I' * len(words), image, 2048, *words)
    executable.write_bytes(image)
