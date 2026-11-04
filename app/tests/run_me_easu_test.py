"""Offline EASU shader/constant regression using DXC and software Vulkan.

Run under Linux/WSL with the project's DXC wrapper, Vulkan headers/library and
Lavapipe. Does not launch the game, build an NRO or write production caches.
"""
from pathlib import Path
import argparse
import os
import re
import shutil
import subprocess

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--dxc', default='/root/me2deko/out/nx/dxc-linux.sh')
parser.add_argument('--cxx', default='clang++')
parser.add_argument('--icd', default='/usr/share/vulkan/icd.d/lvp_icd.json')
args = parser.parse_args()
out = root / 'out/fixes/easu_constants_20261029/test'
out.mkdir(parents=True, exist_ok=True)
host = root / 'app/src/renut_engine/gpu/shaders/host'
sources = {'before': root / 'app/tests/fixtures/fsr_easu_before_cpu_constants.hlsl',
           'after': host / 'fsr_easu_ps.hlsl'}

for label, source in sources.items():
    directory = out / label
    directory.mkdir(exist_ok=True)
    # Production fragment shader, including its bindless declarations and exact push layout.
    spv = directory / 'production.spv'
    subprocess.run([args.dxc, '-T', 'ps_6_0', '-E', 'main', '-HV', '2021', '-all-resources-bound',
                    '-Wno-ignored-attributes', '-spirv', '-fvk-use-dx-layout', '-I', str(host),
                    str(source), '-Fo', str(spv)], check=True)
    subprocess.run(['spirv-val', str(spv)], check=True)
    assembly = subprocess.check_output(['spirv-dis', str(spv)], text=True)
    (directory / 'production.spvasm').write_text(assembly)
    queries = len(re.findall(r'= OpImageQuerySize(?:Lod)?\b', assembly))
    assert queries == (1 if label == 'before' else 0), (label, queries)
    assert len(re.findall(r'= OpImageGather\b', assembly)) == 12
    # The production HLSL compiler retains the utility prefix and adds four aligned vectors.
    if label == 'after':
        offsets = re.findall(r'OpMemberDecorate %type_ConstantBuffer_UtilityPush (\d+) Offset (\d+)', assembly)
        assert [int(n) for _, n in offsets] == [0, 8, 16, 20, 24, 28, 32, 48, 64, 80], offsets
        assert 'FsrEasuCon(' not in source.read_text().split('float4 main(', 1)[1]

    # Execute the exact pixel function from a compute wrapper, replacing only
    # descriptor-array indexing with one fixed image/sampler for a portable test.
    # Pixel coordinates, UVs, EASU body, constants and alpha sampling are retained.
    for include in host.iterdir():
        if include.suffix in ('.h', '.hlsli'):
            shutil.copy2(include, directory / include.name)
    utility = (host / 'utility.hlsli').read_text().replace('g_Textures[]', 'g_Textures[1]').replace('g_Samplers[]', 'g_Samplers[1]')
    utility = re.sub(r'NonUniformResourceIndex\([^)]*\)', '0', utility)
    (directory / 'utility.hlsli').write_text(utility)
    shader = source.read_text().replace('float4 main(', 'float4 PixelMain(', 1)
    shader = re.sub(r'NonUniformResourceIndex\([^)]*\)', '0', shader)
    shader += r'''
RWStructuredBuffer<float4> outputPixels : register(u0, space2);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint2 size = uint2(g_Push.param0, g_Push.param1);
    if (id.x >= size.x || id.y >= size.y) return;
    float2 position = float2(id.xy) + 0.5;
    outputPixels[id.y * size.x + id.x] = PixelMain(float4(position, 0, 1), position / float2(size));
}
'''
    wrapper = directory / 'wrapper.hlsl'
    wrapper.write_text(shader)
    subprocess.run([args.dxc, '-T', 'cs_6_0', '-E', 'main', '-HV', '2021', '-all-resources-bound',
                    '-Wno-ignored-attributes', '-spirv', '-fvk-use-dx-layout', str(wrapper),
                    '-Fo', str(directory / 'wrapper.spv')], check=True)
    subprocess.run(['spirv-val', str(directory / 'wrapper.spv')], check=True)
print('PASS: production SPIR-V, push offsets, unchanged 12 gathers; image-size query removed', flush=True)

exe = out / 'gpu_test'
subprocess.run([args.cxx, '-std=c++20', '-O2', '-ffp-contract=off', '-I' + str(root / 'app/src'),
                str(root / 'app/tests/me_easu_gpu_test.cpp'),
                str(root / 'app/src/renut_engine/gpu/render/easu_constants.cpp'),
                '-lvulkan', '-o', str(exe)], check=True)
environment = os.environ | {'VK_ICD_FILENAMES': args.icd}
result = subprocess.run([str(exe), str(out / 'before/wrapper.spv'), str(out / 'after/wrapper.spv')],
                        env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
(out.parent / 'image_tests.log').write_text(result.stdout)
print(result.stdout, end='')
result.check_returncode()
