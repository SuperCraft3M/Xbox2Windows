"""Build a larger source-authored BC1 stream by reinterleaving the existing synthetic lane fragments."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from _bcpack_rans import BC1_ALPHABET_BITS, decode_lanes

vector = json.loads((ROOT / "tools/_bcpack_rans/vectors/synthetic_11.json").read_text())
packed = bytes.fromhex(vector["chunk_hex"])
lanes = [bytearray() for _ in range(12)]
position = 0

def split(lane):
    global position
    value = packed[position]
    position += 1
    lanes[lane].append(value)
    return value

decode_lanes(b"", BC1_ALPHABET_BITS, [64] * 12, source=split)
assert position == len(packed)
streams = [bytes(lane) * 256 for lane in lanes]
positions = [0] * 12
output = bytearray()

def feed(lane):
    value = streams[lane][positions[lane]]
    positions[lane] += 1
    output.append(value)
    return value

decode_lanes(b"", BC1_ALPHABET_BITS, [16384] * 12, source=feed)
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / "synthetic.bcpack"
    path.write_bytes(output)
    zpath = Path(directory) / "synthetic.zlib"
    zpath.write_bytes(zlib.compress(bytes((i * 17 + i // 31) & 255 for i in range(4 << 20))))
    command = [sys.argv[1], str(path), str(zpath), sys.argv[2]]
    subprocess.run(command, check=True)
    subprocess.run([*command, "disabled"], check=True)
