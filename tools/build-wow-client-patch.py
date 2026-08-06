#!/usr/bin/env python3
"""Build the unified WotLK client patch and matching AzerothCore DBC files.

The patch combines mod-easy-gathering's Spell.dbc requirement changes with
mod-tertiary-stats' spells, authored item, and approved icons. Input DBCs must be from WoW
3.3.5a build 12340.
"""

import argparse
import configparser
import hashlib
import shutil
import struct
from pathlib import Path

from PIL import Image

SPELL_FIELDS = 234
ICON_FIELDS = 2
ITEM_FIELDS = 8
SERVER_PRISTINE_DBC = (
    "SpellItemEnchantment.dbc", "ItemRandomProperties.dbc", "ItemRandomSuffix.dbc"
)

SPELL_BASE = 82001
SPELL_COUNT = 22
ICON_BASE = 4501
SPELL_INTERRUPT_FLAG_MOVEMENT = 0x1
AURA_INTERRUPT_FLAG_MOVEMENT = 0x8
SPELL_EFFECT_CREATE_RANDOM_ITEM = 59
SPELL_EFFECT_CREATE_ITEM_2 = 157
ENABLE_CLIENT_WARCASTER_TRANSFORM = True
POWER_NAMES = (
    "Avoidance", "Fleetfoot", "Siphon", "Tempo", "Echo", "Unbroken", "Opportunity"
)
AUTHORED_ITEM_ENTRY = 90000
AUTHORED_ITEM_SOURCE = 7507
INVTYPE_TRINKET = 12

DEFAULT_TOOL_CATEGORIES = "14,15,121,162,165,166"
ITEM_CLASS_WEAPON = 2
SUBCLASS_MASK_FISHING_POLE = 1 << 20

MPQ_FILE_EXISTS = 0x80000000
MPQ_FILE_SINGLE_UNIT = 0x01000000

try:
    LANCZOS = Image.Resampling.LANCZOS
    MEDIANCUT = Image.Quantize.MEDIANCUT
except AttributeError:  # Pillow < 9.1
    LANCZOS = Image.LANCZOS
    MEDIANCUT = Image.MEDIANCUT


def fail(message):
    raise SystemExit(message)


def validate_dbc(raw, name, expected_fields=None):
    if len(raw) < 20:
        fail(f"{name}: truncated DBC header")
    magic, count, fields, record_size, string_size = struct.unpack_from("<4sIIII", raw)
    if magic != b"WDBC":
        fail(f"{name}: unexpected DBC magic {magic!r}")
    if not fields or record_size != fields * 4:
        fail(f"{name}: inconsistent field count or record size ({fields} fields, {record_size} bytes)")
    if expected_fields is not None and fields != expected_fields:
        fail(f"{name}: unexpected field count {fields} (expected {expected_fields})")
    records_end = 20 + count * record_size
    if records_end + string_size != len(raw):
        fail(f"{name}: inconsistent record or string-block size")
    strings = raw[records_end:]
    if not strings or strings[0] != 0 or strings[-1] != 0:
        fail(f"{name}: string block must be nonempty and start and end with NUL")
    return count, fields, record_size, records_end


def parse_dbc(raw, expected_fields, name):
    count, fields, record_size, records_end = validate_dbc(raw, name, expected_fields)
    rows = [list(struct.unpack_from(f"<{fields}I", raw, 20 + i * record_size)) for i in range(count)]
    return rows, bytearray(raw[records_end:])


def build_dbc(rows, strings, fields):
    records = b"".join(struct.pack(f"<{fields}I", *row) for row in rows)
    return struct.pack("<4sIIII", b"WDBC", len(rows), fields, fields * 4, len(strings)) + records + strings


class StringBlock:
    def __init__(self, initial):
        self.data = bytearray(initial)
        if not self.data:
            self.data.append(0)
        self.known = {b"": 0}

    def add(self, text):
        encoded = text.encode("utf-8")
        if encoded in self.known:
            return self.known[encoded]
        offset = len(self.data)
        self.data += encoded + b"\0"
        self.known[encoded] = offset
        return offset


def float_bits(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]




def read_settings(path):
    parser = configparser.ConfigParser(interpolation=None)
    parser.optionxform = str
    try:
        loaded = parser.read(path)
    except configparser.Error as error:
        fail(f"{path}: invalid config: {error}")
    if not loaded:
        fail(f"cannot read config: {path}")
    if not parser.has_section("worldserver"):
        fail(f"{path}: missing required [worldserver] section")
    section = parser["worldserver"]

    def text(key, default):
        return section.get(key, default).strip()

    def number(key, default):
        try:
            return float(text(key, str(default)))
        except ValueError:
            fail(f"{path}: {key} must be numeric")

    def integer(key, default):
        value = number(key, default)
        if value < 0 or value != int(value):
            fail(f"{path}: {key} must be a non-negative integer")
        return int(value)

    return {
        "tempo_duration": integer("TertiaryStats.Tempo.DurationMs", 8000),
        "tempo_min_cooldown": integer("TertiaryStats.Tempo.MinCooldownMs", 3000),
        "tempo_max_cooldown": integer("TertiaryStats.Tempo.MaxCooldownMs", 30000),
        "momentum_window": integer("TertiaryStats.Fabled.Momentum.WindowMs", 15000),
        "echo_duration": integer("TertiaryStats.Echo.DurationMs", 6000),
        "echo_damage_radius": number("TertiaryStats.Echo.DamageRadius", 10),
        "echo_healing_radius": number("TertiaryStats.Echo.HealingRadius", 30),
        "unbroken_threshold": number("TertiaryStats.Unbroken.ThresholdPct", 35),
        "unbroken_duration": integer("TertiaryStats.Unbroken.DurationMs", 6000),
        "unbroken_cooldown": integer("TertiaryStats.Unbroken.CooldownMs", 45000),
        "opportunity_duration": integer("TertiaryStats.Opportunity.DurationMs", 8000),
    }


def fmt(value):
    return f"{value:g}"


def seconds(milliseconds):
    return fmt(milliseconds / 1000)




def spell_row(spell_id, name, icon_id, effects=(), auras=(), targets=(), duration=0,
              attributes=0, attributes_ex2=0, amplitudes=(), misc_values=(),
              school=1, damage_class=0, visual=0, aura_tooltip="", range_index=0,
              stack_amount=0, dispel=0, aura_interrupt_flags=0, attributes_ex3=0):
    row = [0] * SPELL_FIELDS
    row[0] = spell_id
    row[2] = dispel
    row[4] = attributes
    row[6] = attributes_ex2
    row[7] = attributes_ex3
    row[32] = aura_interrupt_flags
    row[40] = duration
    row[46] = range_index
    row[49] = stack_amount
    row[68] = 0xFFFFFFFF
    for index, effect in enumerate(effects):
        row[71 + index] = effect
    for index, target in enumerate(targets):
        row[86 + index] = target
    for index, aura in enumerate(auras):
        row[95 + index] = aura
    for index, amplitude in enumerate(amplitudes):
        row[98 + index] = amplitude
    for index, misc in enumerate(misc_values):
        row[110 + index] = misc & 0xFFFFFFFF
    row[131] = visual
    row[133] = icon_id
    row[213] = damage_class
    row[216] = float_bits(1.0)
    row[217] = float_bits(1.0)
    row[218] = float_bits(1.0)
    row[225] = school
    return row, name, aura_tooltip


def custom_spells(settings):
    hidden_passive = 0x000000C0
    aura_is_debuff = 0x04000000
    cannot_cancel = 0x80000000
    cannot_crit = 0x20000000
    ignore_line_of_sight = 0x00000004
    suppress_damage_procs = 0x20030000
    return [
        spell_row(82001, "Avoidance", 4501, (6,), (229,), (1,), 21, hidden_passive),
        spell_row(82002, "Fleetfoot", 4502, (6, 6, 6), (129, 130, 58), (1, 1, 1), 21, hidden_passive),
        spell_row(82003, "Fleetfoot", 4502, (6, 6), (210, 209), (1, 1), 21, hidden_passive),
        spell_row(82004, "Siphon", 4503, (10,), (), (1,), attributes_ex2=cannot_crit,
                  school=2, damage_class=1),
        spell_row(82005, "Tempo", 4504, (6, 6, 6), (31, 192, 65), (1, 1, 1), 32,
                  cannot_cancel,
                  aura_tooltip="Movement, attack, casting, and damaging-area tick speed increased by $s1%."),
        spell_row(82006, "Echo", 4505, (2,), (), (6,),
                  attributes_ex2=cannot_crit | ignore_line_of_sight,
                  school=64, damage_class=1, visual=965, range_index=1),
        spell_row(82007, "Echo", 4505, (10,), (), (21,),
                  attributes_ex2=cannot_crit | ignore_line_of_sight,
                  school=2, damage_class=1, visual=8253, range_index=1),
        spell_row(82008, "Unbroken", 4506, (6, 6, 6), (69, 8, 31), (1, 1, 1), 32,
                  cannot_cancel, cannot_crit, (0, 1000, 0), (127, 0, 0), school=1, visual=784,
                  aura_tooltip=("Absorbs up to $s1 damage, restores $s2 health every sec, and increases "
                  "movement speed by $s3%.")),
        spell_row(82009, "Opportunity", 4507, (6,), (4,), (1,), 31, cannot_cancel,
                  aura_tooltip=("Your next eligible ability critically strikes every immediate target and "
                  "every tick of an active channel.")),
        spell_row(82010, "Echo", 4505, (6,), (4,), (1,), 31, cannot_cancel,
                  aura_tooltip=("Storing $s1% of your effective damage and healing for "
                  f"{seconds(settings['echo_duration'])} sec.")),
        spell_row(82011, "Premonition", 212, (6,), (72,), (1,), 35, cannot_cancel,
                  misc_values=(127,), aura_tooltip="Your abilities are free."),
        spell_row(82012, "Momentum", 516, (6, 6, 6), (31, 138, 65), (1, 1, 1), 8,
                  cannot_cancel, stack_amount=255,
                  aura_tooltip=("Kills build movement, melee attack, and casting speed. "
                  f"All stacks expire together after {seconds(settings['momentum_window'])} sec.")),
        spell_row(82013, "Ghostwalk", 67, (6, 6), (31, 16), (1, 1), 21, cannot_cancel,
                  aura_interrupt_flags=0,
                  aura_tooltip="Out of combat, you fade into a swift, stealthy spirit."),
        spell_row(82014, "Vengeful Ghost", 1654, (6,), (4,), (1,), 8, aura_is_debuff,
                  aura_tooltip=("Death leaves you in a vengeful spirit phase. "
                  "Kill before it ends to return.")),
        spell_row(82015, "Toxicity", 513, (6,), (89,), (1,), 9, aura_is_debuff,
                  amplitudes=(3000,), school=8, stack_amount=255, dispel=0,
                  aura_tooltip="Each stack deals $s1% of your maximum health every 3 sec."),
        spell_row(82016, "Leviathan's Gift", 545, (6, 6), (82, 58), (1, 1), 21,
                  cannot_cancel,
                  aura_tooltip="You can breathe underwater and swim $s2% faster."),
        spell_row(82017, "Blood Debt", 1109, (6, 6), (4, 3), (1, 1), 0,
                  aura_is_debuff | cannot_cancel, amplitudes=(0, 1000),
                  attributes_ex3=suppress_damage_procs, school=32,
                  aura_tooltip="$s1 damage remains and is dealt over the debuff's duration."),
        spell_row(82018, "Impact", 4501, (2,), (), (6,),
                  attributes_ex2=cannot_crit | ignore_line_of_sight,
                  attributes_ex3=suppress_damage_procs,
                  school=1, damage_class=1, visual=784, range_index=1),
        spell_row(82019, "Crossfire", 4505, (2,), (), (6,),
                  attributes_ex2=cannot_crit | ignore_line_of_sight,
                  attributes_ex3=suppress_damage_procs,
                  school=1, damage_class=1, visual=0, range_index=1),
        spell_row(82020, "Vengeful Ghost Exhaustion", 1654),
        spell_row(82021, "Tempo: Expedite", 4504, (6,), (4,), (1,), 31, cannot_cancel,
                  aura_tooltip=(f"Your next eligible {seconds(settings['tempo_min_cooldown'])}-"
                                f"{seconds(settings['tempo_max_cooldown'])} sec class ability "
                                "has its cooldown reduced.")),
        spell_row(82022, "Overkill", 4505, (2,), (), (6,),
                  attributes_ex2=cannot_crit | ignore_line_of_sight,
                  attributes_ex3=suppress_damage_procs,
                  school=1, damage_class=1, visual=0, range_index=1),
        spell_row(82023, "Vengeful Ghost", 1654, (6,), (69,), (1,), 21,
                  hidden_passive, misc_values=(127,)),
    ]


def strip_client_movement_interrupts(rows):
    """Remove movement cancellation from standard client spell records only."""
    interrupted_casts = 0
    interrupted_channels = 0
    for row in rows:
        if row[31] & SPELL_INTERRUPT_FLAG_MOVEMENT:
            row[31] &= ~SPELL_INTERRUPT_FLAG_MOVEMENT
            interrupted_casts += 1
        if row[33] & AURA_INTERRUPT_FLAG_MOVEMENT:
            row[33] &= ~AURA_INTERRUPT_FLAG_MOVEMENT
            interrupted_channels += 1
    return interrupted_casts, interrupted_channels


def is_loot_crafting(row):
    return (row[71] == SPELL_EFFECT_CREATE_RANDOM_ITEM
            or (row[71] == SPELL_EFFECT_CREATE_ITEM_2
                and (row[222] != 0
                     or (row[50] != 0 and row[133] == 1)
                     or row[107] == 0)))


def patch_spell_dbc(raw, categories, settings, strip_easy_gathering,
                    strip_movement_interrupts=False):
    rows, strings_raw = parse_dbc(raw, SPELL_FIELDS, "Spell.dbc")
    custom_ids = set(range(SPELL_BASE, SPELL_BASE + SPELL_COUNT))
    rows = [row for row in rows if row[0] not in custom_ids]
    fishing = []
    tools = 0

    if strip_easy_gathering:
        for row in rows:
            if ((row[5] & 0x44) and row[68] == ITEM_CLASS_WEAPON
                    and row[69] == SUBCLASS_MASK_FISHING_POLE):
                row[68] = 0xFFFFFFFF
                row[69] = 0
                row[70] = 0
                fishing.append(row[0])
            original_categories = (row[222], row[223])
            loot_before = is_loot_crafting(row)
            stripped = False
            for column in (222, 223):
                if row[column] in categories:
                    row[column] = 0
                    stripped = True
            if stripped and is_loot_crafting(row) != loot_before:
                row[222], row[223] = original_categories
                stripped = False
            tools += int(stripped)

    movement_counts = (0, 0)
    if strip_movement_interrupts:
        movement_counts = strip_client_movement_interrupts(rows)

    strings = StringBlock(strings_raw)
    for row, name, tooltip in custom_spells(settings):
        row[136] = strings.add(name)
        if tooltip:
            row[187] = strings.add(tooltip)
        rows.append(row)
    return build_dbc(rows, strings.data, SPELL_FIELDS), fishing, tools, movement_counts




def patch_icon_dbc(raw):
    rows, strings_raw = parse_dbc(raw, ICON_FIELDS, "SpellIcon.dbc")
    custom_ids = set(range(ICON_BASE, ICON_BASE + len(POWER_NAMES)))
    rows = [row for row in rows if row[0] not in custom_ids]
    strings = StringBlock(strings_raw)
    for index, name in enumerate(POWER_NAMES):
        rows.append([ICON_BASE + index, strings.add(f"Interface\\Icons\\Tertiary_{name}")])
    return build_dbc(rows, strings.data, ICON_FIELDS)


def patch_item_dbc(raw):
    rows, strings = parse_dbc(raw, ITEM_FIELDS, "Item.dbc")
    source = next((row for row in rows if row[0] == AUTHORED_ITEM_SOURCE), None)
    if source is None:
        fail(f"Item.dbc: missing authored item source {AUTHORED_ITEM_SOURCE}")

    authored = source.copy()
    authored[0] = AUTHORED_ITEM_ENTRY
    authored[6] = INVTYPE_TRINKET
    authored[7] = 0
    rows = [row for row in rows if row[0] != AUTHORED_ITEM_ENTRY]
    rows.append(authored)
    rows.sort(key=lambda row: row[0])
    return build_dbc(rows, strings, ITEM_FIELDS)


def blp_palette_icon(source):
    image = Image.open(source).convert("RGBA").resize((64, 64), LANCZOS)
    rgb = Image.new("RGB", image.size)
    rgb.paste(image, mask=image.getchannel("A"))
    quantized = rgb.quantize(colors=256, method=MEDIANCUT)
    palette_raw = quantized.getpalette()[:768]
    palette = bytearray()
    for index in range(256):
        red, green, blue = palette_raw[index * 3:index * 3 + 3]
        palette += bytes((blue, green, red, 0))

    mip_data = []
    width = height = 64
    current = image
    while width and height:
        current_rgb = Image.new("RGB", current.size)
        current_rgb.paste(current, mask=current.getchannel("A"))
        indexed = current_rgb.quantize(palette=quantized)
        mip_data.append(bytes(indexed.getdata()) + bytes(current.getchannel("A").getdata()))
        if width == 1 and height == 1:
            break
        width = max(1, width // 2)
        height = max(1, height // 2)
        current = image.resize((width, height), LANCZOS)

    header_size = 148
    data_offset = header_size + len(palette)
    offsets = [0] * 16
    lengths = [0] * 16
    cursor = data_offset
    for index, data in enumerate(mip_data):
        offsets[index] = cursor
        lengths[index] = len(data)
        cursor += len(data)

    # WotLK's paletted BLP2 textures use alpha encoding 8 with an 8-bit alpha plane.
    header = struct.pack("<4sIBBBBII", b"BLP2", 1, 1, 8, 8, 1, 64, 64)
    header += struct.pack("<16I", *offsets) + struct.pack("<16I", *lengths)
    result = header + palette + b"".join(mip_data)
    if (result[:4] != b"BLP2" or result[8:12] != b"\x01\x08\x08\x01"
            or offsets[0] != data_offset or lengths[0] != 64 * 64 * 2):
        fail(f"failed to encode WotLK-compatible BLP icon {source}")
    return result


def build_crypt_table():
    table = [0] * 0x500
    seed = 0x00100001
    for index in range(0x100):
        slot = index
        for _ in range(5):
            seed = (seed * 125 + 3) % 0x2AAAAB
            high = (seed & 0xFFFF) << 16
            seed = (seed * 125 + 3) % 0x2AAAAB
            table[slot] = high | (seed & 0xFFFF)
            slot += 0x100
    return table


CRYPT = build_crypt_table()


def hash_string(text, hash_type):
    seed1, seed2 = 0x7FED7FED, 0xEEEEEEEE
    for char in text.upper().replace("/", "\\"):
        code = ord(char)
        seed1 = (CRYPT[(hash_type << 8) + code] ^ (seed1 + seed2)) & 0xFFFFFFFF
        seed2 = (code + seed1 + seed2 + (seed2 << 5) + 3) & 0xFFFFFFFF
    return seed1


def encrypt_block(data, key):
    output = bytearray()
    seed = 0xEEEEEEEE
    for (value,) in struct.iter_unpack("<I", data):
        seed = (seed + CRYPT[0x400 + (key & 0xFF)]) & 0xFFFFFFFF
        output += struct.pack("<I", value ^ ((key + seed) & 0xFFFFFFFF))
        key = ((((~key & 0xFFFFFFFF) << 0x15) + 0x11111111) | (key >> 0x0B)) & 0xFFFFFFFF
        seed = (value + seed + (seed << 5) + 3) & 0xFFFFFFFF
    return bytes(output)


def decrypt_block(data, key):
    output = bytearray()
    seed = 0xEEEEEEEE
    for (encrypted,) in struct.iter_unpack("<I", data):
        seed = (seed + CRYPT[0x400 + (key & 0xFF)]) & 0xFFFFFFFF
        value = encrypted ^ ((key + seed) & 0xFFFFFFFF)
        output += struct.pack("<I", value)
        key = ((((~key & 0xFFFFFFFF) << 0x15) + 0x11111111) | (key >> 0x0B)) & 0xFFFFFFFF
        seed = (value + seed + (seed << 5) + 3) & 0xFFFFFFFF
    return bytes(output)


def build_mpq(files):
    file_items = list(files.items())
    hash_entries = 1
    while hash_entries < len(file_items) * 2:
        hash_entries *= 2

    file_blob = bytearray()
    blocks = []
    for _, data in file_items:
        offset = 32 + len(file_blob)
        file_blob += data
        blocks.append((offset, len(data), len(data), MPQ_FILE_EXISTS | MPQ_FILE_SINGLE_UNIT))

    hash_offset = 32 + len(file_blob)
    block_offset = hash_offset + hash_entries * 16
    archive_size = block_offset + len(blocks) * 16
    hashes = [(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFF, 0xFFFF, 0xFFFFFFFF)] * hash_entries

    for block_index, (name, _) in enumerate(file_items):
        slot = hash_string(name, 0) & (hash_entries - 1)
        while hashes[slot][4] != 0xFFFFFFFF:
            slot = (slot + 1) & (hash_entries - 1)
        hashes[slot] = (hash_string(name, 1), hash_string(name, 2), 0, 0, block_index)

    hash_raw = b"".join(struct.pack("<IIHHI", *entry) for entry in hashes)
    block_raw = b"".join(struct.pack("<4I", *entry) for entry in blocks)
    header = struct.pack("<4sIIHHIIII", b"MPQ\x1a", 32, archive_size, 0, 3,
                         hash_offset, block_offset, hash_entries, len(blocks))
    archive = (header + file_blob
               + encrypt_block(hash_raw, hash_string("(hash table)", 3))
               + encrypt_block(block_raw, hash_string("(block table)", 3)))

    decoded_hashes = decrypt_block(archive[hash_offset:block_offset], hash_string("(hash table)", 3))
    decoded_blocks = decrypt_block(archive[block_offset:], hash_string("(block table)", 3))
    for name, expected in file_items:
        slot = hash_string(name, 0) & (hash_entries - 1)
        while True:
            name_a, name_b, _, _, block_index = struct.unpack_from("<IIHHI", decoded_hashes, slot * 16)
            if (name_a, name_b) == (hash_string(name, 1), hash_string(name, 2)):
                break
            slot = (slot + 1) & (hash_entries - 1)
        offset, _, size, flags = struct.unpack_from("<4I", decoded_blocks, block_index * 16)
        if flags != MPQ_FILE_EXISTS | MPQ_FILE_SINGLE_UNIT or archive[offset:offset + size] != expected:
            fail(f"MPQ self-verification failed for {name}")
    return archive


def main():
    repo = Path(__file__).resolve().parents[1]
    module = repo / "modules" / "mod-tertiary-stats"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client-dbc-dir", required=True, type=Path,
                        help="directory containing pristine 3.3.5a DBC files")
    parser.add_argument("--out", required=True, type=Path,
                        help="output MPQ path, normally patch-4.MPQ")
    parser.add_argument("--server-dbc-dir", type=Path,
                        help="also write matching patched DBC files here")
    parser.add_argument("--client-addon-dir", type=Path,
                        help="install the companion addon in this Interface/AddOns directory")
    parser.add_argument("--config", type=Path,
                        default=module / "conf" / "mod-tertiary-stats.conf.dist")
    parser.add_argument("--icons", type=Path, default=module / "assets" / "icons")
    parser.add_argument("--tool-categories", default=DEFAULT_TOOL_CATEGORIES,
                        help="EasyGathering totem category IDs to strip")
    args = parser.parse_args()

    try:
        categories = {int(token.strip()) for token in args.tool_categories.split(",") if token.strip()}
    except ValueError:
        fail("--tool-categories must be a comma-separated list of integers")
    settings = read_settings(args.config)

    required = ("Spell.dbc", "SpellIcon.dbc", "Item.dbc", *SERVER_PRISTINE_DBC)
    source = {name: (args.client_dbc_dir / name).read_bytes() for name in required}
    for name in SERVER_PRISTINE_DBC:
        validate_dbc(source[name], name)
    client_spell, fishing, tools, movement_counts = patch_spell_dbc(
        source["Spell.dbc"], categories, settings, strip_easy_gathering=True,
        strip_movement_interrupts=ENABLE_CLIENT_WARCASTER_TRANSFORM)
    server_spell, _, _, server_movement_counts = patch_spell_dbc(
        source["Spell.dbc"], categories, settings, strip_easy_gathering=False)
    if server_movement_counts != (0, 0):
        fail("server Spell.dbc unexpectedly received the client movement transform")
    client_rows, _ = parse_dbc(client_spell, SPELL_FIELDS, "client Spell.dbc")
    server_rows, _ = parse_dbc(server_spell, SPELL_FIELDS, "server Spell.dbc")
    client_by_id = {row[0]: row for row in client_rows}
    server_by_id = {row[0]: row for row in server_rows}
    cast_divergence = sum(
        bool(server_by_id[spell_id][31] & SPELL_INTERRUPT_FLAG_MOVEMENT)
        and not bool(client_by_id[spell_id][31] & SPELL_INTERRUPT_FLAG_MOVEMENT)
        for spell_id in server_by_id
    )
    channel_divergence = sum(
        bool(server_by_id[spell_id][33] & AURA_INTERRUPT_FLAG_MOVEMENT)
        and not bool(client_by_id[spell_id][33] & AURA_INTERRUPT_FLAG_MOVEMENT)
        for spell_id in server_by_id
    )
    if (cast_divergence, channel_divergence) != movement_counts:
        fail("client/server movement-interrupt divergence verification failed")
    frostbolt_client = client_by_id[116]
    frostbolt_server = server_by_id[116]
    if (frostbolt_client[31] & SPELL_INTERRUPT_FLAG_MOVEMENT
            or not frostbolt_server[31] & SPELL_INTERRUPT_FLAG_MOVEMENT):
        fail("Frostbolt 116 movement-interrupt spot check failed")
    common = {
        "SpellIcon.dbc": patch_icon_dbc(source["SpellIcon.dbc"]),
        "Item.dbc": patch_item_dbc(source["Item.dbc"]),
    }
    patched = {
        "Spell.dbc": client_spell,
        **common,
        **{name: source[name] for name in SERVER_PRISTINE_DBC},
    }
    server_patched = {
        "Spell.dbc": server_spell,
        **common,
        **{name: source[name] for name in SERVER_PRISTINE_DBC},
    }

    files = {f"DBFilesClient\\{name}": data for name, data in patched.items()}
    for name in POWER_NAMES:
        icon_source = args.icons / f"{name.lower()}.webp"
        if not icon_source.is_file():
            fail(f"missing approved icon master: {icon_source}")
        files[f"Interface\\Icons\\Tertiary_{name}.blp"] = blp_palette_icon(icon_source)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    archive = build_mpq(files)
    args.out.write_bytes(archive)
    (args.out.parent / f"{args.out.name}.sha256").write_text(
        f"{hashlib.sha256(archive).hexdigest()}  {args.out.name}\n", encoding="ascii")

    if args.server_dbc_dir:
        args.server_dbc_dir.mkdir(parents=True, exist_ok=True)
        for name, data in server_patched.items():
            (args.server_dbc_dir / name).write_bytes(data)

    if args.client_addon_dir:
        addon_source = module / "TertiaryStatsUI"
        addon_destination = args.client_addon_dir / addon_source.name
        args.client_addon_dir.mkdir(parents=True, exist_ok=True)
        shutil.copytree(addon_source, addon_destination, dirs_exist_ok=True)

    print(f"fishing spells stripped: {len(fishing)} {fishing}")
    print(f"physical-tool spells stripped: {tools}")
    print(f"client movement InterruptFlags cleared: {cast_divergence}")
    print(f"client movement ChannelInterruptFlags cleared: {channel_divergence}")
    print("Frostbolt 116 InterruptFlags: "
          f"client=0x{frostbolt_client[31]:08x} server=0x{frostbolt_server[31]:08x}")
    print(f"tertiary rows: {len(custom_spells(settings))} spells, 1 item, {len(POWER_NAMES)} custom icons")
    print(f"server DBC files: {len(server_patched)}")
    print(f"MPQ files: {len(files)}")
    print(f"wrote {args.out} ({len(archive)} bytes)")
    print(f"sha256 {hashlib.sha256(archive).hexdigest()}")
    if args.client_addon_dir:
        print(f"installed addon {addon_destination}")


if __name__ == "__main__":
    main()
