"""Grab the Flipper screen over USB serial and save it as a PNG.

    uvx --with pyserial --with pillow python scripts/screenshot.py /dev/cu.usbmodemflip_XXX docs/screenshot.png

Switches the CLI into an RPC session and requests one screen-stream frame.
The protobuf messages are tiny, so they are encoded by hand instead of
pulling in the generated flipperzero-protobuf bindings.

The frame is the raw 128x64 display buffer (u8g2 pages: one byte = 8
vertical pixels). Pimpkin Remote runs in vertical orientation, so the image
is rotated to 64x128 portrait and scaled up for the README.
"""

import sys
import time

import serial
from PIL import Image

WIDTH, HEIGHT = 128, 64
SCALE = 4

# PB.Main field numbers (flipperzero-protobuf flipper.proto).
FIELD_COMMAND_ID = 1
FIELD_START_SCREEN_STREAM = 20
FIELD_SCREEN_FRAME = 22
FIELD_FRAME_DATA = 1


def varint(value: int) -> bytes:
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def read_varint(data: bytes, pos: int) -> tuple[int, int]:
    value = shift = 0
    while True:
        byte = data[pos]
        pos += 1
        value |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return value, pos
        shift += 7


def fields(data: bytes):
    """Yield (field number, wire type, value) for one protobuf message."""
    pos = 0
    while pos < len(data):
        key, pos = read_varint(data, pos)
        number, wire = key >> 3, key & 7
        if wire == 0:
            value, pos = read_varint(data, pos)
        elif wire == 2:
            size, pos = read_varint(data, pos)
            value, pos = data[pos : pos + size], pos + size
        else:
            raise ValueError(f"unexpected wire type {wire}")
        yield number, wire, value


def start_rpc(port: serial.Serial) -> None:
    port.write(b"\r")
    time.sleep(0.3)
    port.reset_input_buffer()
    port.write(b"start_rpc_session\r")
    port.read_until(b"\n")  # command echo


def request_frame(port: serial.Serial) -> bytes:
    message = (
        varint(FIELD_COMMAND_ID << 3) + varint(1)
        + varint(FIELD_START_SCREEN_STREAM << 3 | 2) + varint(0)
    )
    port.write(varint(len(message)) + message)

    buffer = b""
    deadline = time.time() + 5
    while time.time() < deadline:
        buffer += port.read(4096)
        while buffer:
            try:
                size, start = read_varint(buffer, 0)
            except IndexError:
                break
            if len(buffer) < start + size:
                break
            body, buffer = buffer[start : start + size], buffer[start + size :]
            for number, _, value in fields(body):
                if number == FIELD_SCREEN_FRAME:
                    for inner, _, data in fields(value):
                        if inner == FIELD_FRAME_DATA:
                            return data
    raise TimeoutError("no screen frame received")


def frame_to_image(data: bytes) -> Image.Image:
    image = Image.new("1", (WIDTH, HEIGHT), 1)
    for y in range(HEIGHT):
        for x in range(WIDTH):
            if data[(y // 8) * WIDTH + x] >> (y % 8) & 1:
                image.putpixel((x, y), 0)
    return image


def main() -> None:
    device, out = sys.argv[1], sys.argv[2]
    with serial.Serial(device, 230400, timeout=0.2) as port:
        start_rpc(port)
        data = request_frame(port)
    image = frame_to_image(data).transpose(Image.Transpose.ROTATE_270)
    image = image.resize((image.width * SCALE, image.height * SCALE), Image.Resampling.NEAREST)
    image.save(out)
    print(f"saved {out} ({image.width}x{image.height})")


if __name__ == "__main__":
    main()
