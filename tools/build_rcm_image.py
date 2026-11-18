#!/usr/bin/env python3
"""Genera/inspecciona la imagen RCM (fusee gelee) que se envia a una Nintendo
Switch en modo RCM.

Es la misma construccion que hace fusee-launcher (y la que implementa
main/rcm.c en el firmware), util para depurar o para verificar un payload
antes de flashear el ESP32-S3.

Formato de la imagen (offsets sobre el flujo que se envia por USB):

    0x0000 .. 0x02A7  comando RCM (680 B): u32 0x30298 + ceros
    0x02A8 .. 0x0323  intermezzo (124 B)  -> 0x40010000
    0x0324 .. 0x10E7  ceros
    0x10E8 .. 0x50E7  payload[0x0000:0x4000] -> 0x40010E40
    0x50E8 .. 0x72A7  2160 x u32(0x40010000)  (stack spray)
    0x72A8 ..         payload[0x4000:]        -> 0x40017000
    ...relleno de ceros hasta multiplo de 0x1000

Uso:
    python3 build_rcm_image.py --payload hekate.bin --output main/rcm_message.bin
    python3 build_rcm_image.py --payload hekate.bin --info
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

RCM_MAX_IMAGE_SIZE = 0x30298
RCM_CMD_SIZE = 680
RCM_PAYLOAD_ADDR = 0x40010000
PAYLOAD_START_ADDR = 0x40010E40
STACK_SPRAY_START = 0x40014E40
STACK_SPRAY_END = 0x40017000
CHUNK = 0x1000

HERE = Path(__file__).resolve().parent
DEFAULT_INTERMEZZO = HERE.parent / "main" / "intermezzo.bin"


def build(payload: bytes, intermezzo: bytes) -> bytes:
    if len(payload) < 0x400:
        raise ValueError(f"payload demasiado pequeno ({len(payload)} bytes)")

    out = bytearray()
    out += struct.pack("<I", RCM_MAX_IMAGE_SIZE)
    out += bytes(RCM_CMD_SIZE - len(out))
    assert len(out) == RCM_CMD_SIZE

    out += intermezzo
    out += bytes(PAYLOAD_START_ADDR - (RCM_PAYLOAD_ADDR + len(intermezzo)))

    split = STACK_SPRAY_START - PAYLOAD_START_ADDR
    part1 = payload[:split]
    out += part1
    # Si el payload es mas corto que la primera mitad, el hueco se rellena con
    # ceros para que el spray caiga siempre en 0x40014E40.
    out += bytes(split - len(part1))
    out += struct.pack("<I", RCM_PAYLOAD_ADDR) * ((STACK_SPRAY_END - STACK_SPRAY_START) // 4)
    out += payload[split:]

    # Igual que fusee-launcher: siempre anade entre 1 y 0x1000 bytes.
    out += bytes(CHUNK - (len(out) % CHUNK))

    if len(out) > RCM_MAX_IMAGE_SIZE:
        raise ValueError(
            f"payload demasiado grande: imagen de {len(out)} bytes, "
            f"maximo {RCM_MAX_IMAGE_SIZE}"
        )
    return bytes(out)


def describe(image: bytes) -> str:
    return "\n".join(
        [
            f"imagen total      : {len(image)} bytes ({len(image)//CHUNK} bloques de 0x1000)",
            f"comando RCM       : {struct.unpack_from('<I', image, 0)[0]:#x}",
            f"intermezzo @0x{RCM_CMD_SIZE:04X} : {image[RCM_CMD_SIZE:RCM_CMD_SIZE+4].hex()}...",
            f"payload parte 1 @0x{PAYLOAD_START_ADDR - RCM_PAYLOAD_ADDR + RCM_CMD_SIZE:04X}"
            f" -> 0x{PAYLOAD_START_ADDR:08X}",
            f"stack spray @0x{PAYLOAD_START_ADDR - RCM_PAYLOAD_ADDR + RCM_CMD_SIZE + 0x4000:04X}"
            f" -> 0x{STACK_SPRAY_START:08X}..0x{STACK_SPRAY_END:08X}",
            f"payload parte 2 @0x{PAYLOAD_START_ADDR - RCM_PAYLOAD_ADDR + RCM_CMD_SIZE + 0x4000 + 0x21C0:04X}"
            f" -> 0x{STACK_SPRAY_END:08X}",
        ]
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--payload", type=Path, required=True, help="payload (hekate, fusee...)")
    parser.add_argument("--intermezzo", type=Path, default=DEFAULT_INTERMEZZO, help="stub relocador")
    parser.add_argument("--output", type=Path, help="fichero de salida con la imagen RCM")
    parser.add_argument("--info", action="store_true", help="solo mostrar informacion")
    args = parser.parse_args()

    if not args.payload.is_file():
        parser.error(f"no existe el payload: {args.payload}")
    if not args.intermezzo.is_file():
        parser.error(f"no existe el intermezzo: {args.intermezzo}")

    payload = args.payload.read_bytes()
    intermezzo = args.intermezzo.read_bytes()

    try:
        image = build(payload, intermezzo)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    print(f"payload           : {args.payload} ({len(payload)} bytes)")
    print(describe(image))

    if args.output:
        args.output.write_bytes(image)
        print(f"escrito           : {args.output}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
