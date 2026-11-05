"""Small bit and number helpers shared by the translator modules."""

MASK32 = 0xFFFFFFFF


def sign_extend(value, bits):
    """Reinterpret the low `bits` of value as a signed number."""
    sign_bit = 1 << (bits - 1)
    return ((value & ((1 << bits) - 1)) ^ sign_bit) - sign_bit


def rotate_right(value, amount):
    amount &= 31
    if not amount:
        return value
    return ((value >> amount) | (value << (32 - amount))) & MASK32


def c_hex(value):
    """A 32-bit constant as it is written in the generated C."""
    return f"0x{value & MASK32:08X}u"


def func_symbol(addr, thumb, prefix=""):
    """Name of the generated C function for the code at addr."""
    return f"{'T' if thumb else 'A'}{prefix}_{addr:08X}"
