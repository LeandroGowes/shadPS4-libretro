param(
    [Parameter(Mandatory = $true)] [string] $CompilerBin,
    [string] $CMake = 'cmake.exe',
    [string] $Ninja = 'ninja.exe',
    [ValidateRange(1, 64)] [int] $Jobs = 4
)

$ErrorActionPreference = 'Stop'
$sourceRoot = Split-Path $PSScriptRoot -Parent
$compilerPath = (Resolve-Path -LiteralPath $CompilerBin).Path
$cmakePath = (Get-Command $CMake -ErrorAction Stop).Source
$ninjaPath = (Get-Command $Ninja -ErrorAction Stop).Source
$env:PATH = "$compilerPath;$env:PATH"
$openAlRoot = Join-Path $sourceRoot 'externals/openal-soft'
$patchPath = Join-Path $sourceRoot 'cmake/libretro-openal-clang.patch'

& git -C $sourceRoot submodule update --init --recursive
if ($LASTEXITCODE -ne 0) { throw 'Submodule checkout failed.' }

$openAlSource = Get-Content -LiteralPath (Join-Path $openAlRoot 'alc/alc.cpp') -Raw
if ($openAlSource.Contains('&ContextBase::EffectSlotCluster::operator*')) {
    & git -C $openAlRoot apply $patchPath
    if ($LASTEXITCODE -ne 0) { throw 'OpenAL compiler patch failed.' }
} elseif (-not $openAlSource.Contains('[](auto &cluster) -> decltype(auto) { return *cluster; }')) {
    throw 'OpenAL source does not match the pinned dependency.'
}

& $cmakePath -S $sourceRoot -B (Join-Path $sourceRoot 'build-libretro') -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$ninjaPath" `
    "-DCMAKE_C_COMPILER=$(Join-Path $compilerPath 'clang.exe')" `
    "-DCMAKE_CXX_COMPILER=$(Join-Path $compilerPath 'clang++.exe')" `
    -DCMAKE_BUILD_TYPE=Release -DENABLE_LIBRETRO=ON -DLIBRETRO_STATIC_RUNTIME=ON `
    -DENABLE_TESTS=OFF -DENABLE_DISCORD_RPC=OFF -DENABLE_UPDATER=OFF `
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DHAVE_ENDIAN_H=OFF
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }

& $cmakePath --build (Join-Path $sourceRoot 'build-libretro') --target shadps4_libretro --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw 'Libretro build failed.' }

Write-Output (Join-Path $sourceRoot 'build-libretro/shadps4_libretro.dll')
