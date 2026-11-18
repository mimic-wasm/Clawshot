"""The software HMMA path avoids AMD proprietary subgroup shuffles on devices without cooperative matrices."""

import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "g++"]
ROOT = Path(__file__).resolve().parents[1]


def check_source():
    profile = (ROOT / "gpu/vk/eden/shader_recompiler/profile.h").read_text(encoding="utf-8")
    shader = (ROOT / "gpu/vk/vk_shader.cpp").read_text(encoding="utf-8")
    warp = (ROOT / "gpu/vk/eden/shader_recompiler/backend/spirv/emit_spirv_warp.cpp").read_text(encoding="utf-8")
    assert "has_broken_spirv_software_mma_shuffle" in profile
    assert '#include "amd_software_mma.h"' in shader
    assert "dev.driver_id == VK_DRIVER_ID_AMD_PROPRIETARY" in shader
    assert "Shader::Backend::SPIRV::mma_software" in shader
    assert "profile().has_broken_spirv_software_mma_shuffle" in shader
    body = warp[warp.index("void DefineSoftwareMatrixFunction(EmitContext& ctx) {"):
                warp.index("// (hookshot) HMMA_nx2:", warp.index("void DefineSoftwareMatrixFunction(EmitContext& ctx) {"))]
    assert "broadcast_shuffle" in body
    assert "ShuffleAbsolute(ctx, b," in body
    assert "ShuffleAbsolute(ctx, a," in body
    assert "OpGroupNonUniformShuffle(" not in body


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    fixture = r'''
#include <cassert>
#include "amd_software_mma.h"

int main() {
    assert(!amd_software_mma::use_broadcast_fallback(false, false));
    assert(!amd_software_mma::use_broadcast_fallback(false, true));
    assert(!amd_software_mma::use_broadcast_fallback(true, false));
    assert(amd_software_mma::use_broadcast_fallback(true, true));
}
'''
    with tempfile.TemporaryDirectory(prefix="amd-software-mma-") as tmp:
        source = Path(tmp) / "policy.cpp"
        exe = Path(tmp) / "policy.exe"
        source.write_text(fixture, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "gpu/vk"), str(source), "-o", str(exe)], check=True, timeout=60)
        subprocess.run([str(exe)], check=True, timeout=10)
    print("PASS: AMD proprietary software HMMA selects the broadcast fallback; other profiles keep native shuffles")


if __name__ == "__main__":
    main()
