"""Package the Windows build without publishing a release."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import zipfile

root = Path.cwd()
out = root / "dist/windows"
package = out / "package"
package.mkdir(parents=True, exist_ok=True)
core = root / "build-libretro/shadps4_libretro.dll"
commit = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
shutil.copy2(core, package / core.name)
for name in ("LICENSE", "LIBRETRO.md", ".gitmodules"):
    shutil.copy2(root / name, package / name)
shutil.copytree(root / "LICENSES", package / "LICENSES", dirs_exist_ok=True)
for directory in (root / "externals", Path(os.environ["LLVM_ROOT"])):
    for source in directory.rglob("*"):
        if source.is_file() and source.name.upper().startswith(("LICENSE", "COPYING", "COPYRIGHT", "NOTICE")):
            target = package / "licenses" / directory.name / source.relative_to(directory)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
sha = hashlib.sha256(core.read_bytes()).hexdigest()
metadata = {"source_repository": "https://github.com/LeandroGowes/shadPS4-libretro",
            "source_commit": commit, "platform": "windows", "architecture": "x86_64",
            "filename": core.name, "sha256": sha, "configuration": "Release",
            "compiler": "LLVM-MinGW 20260922-ucrt-x86_64",
            "dependency_patch": "cmake/libretro-openal-clang.patch",
            "verification": "Compilation only; no game execution on CI",
            "runtime_requirements": ["Windows system libraries", "Vulkan driver"]}
for directory in (out, package):
    (directory / "build-info-windows.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
(package / "source-submodules.txt").write_bytes(subprocess.check_output(["git", "submodule", "status", "--recursive"]))
archive = out / "shadps4-libretro-windows-x86_64.zip"
with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as bundle:
    for source in package.rglob("*"):
        if source.is_file():
            bundle.write(source, source.relative_to(package))
(out / "SHA256SUMS-windows.txt").write_text(
    f"{sha}  {core.name}\n{hashlib.sha256(archive.read_bytes()).hexdigest()}  {archive.name}\n", encoding="utf-8")
