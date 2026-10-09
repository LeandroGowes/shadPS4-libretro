# shadPS4 official: núcleo Libretro local para Promus Play

Base de emulação: https://github.com/shadps4-emu/shadPS4

Revisão usada: `0fe263a4760dfbfa973366890061749b4af0de97` (main, 0.19.1 WIP).
O código de integração Libretro foi reaproveitado e portado da integração local
anterior; o emulador, kernel, decodificação e renderizador são os desta revisão
oficial. Esta DLL é uma compilação local, não um lançamento oficial do projeto.

## Compilação Windows

Dependências: Git, CMake 3.24+, Ninja e LLVM-MinGW
`20260922-ucrt-x86_64` (Clang 23.1.2). Esta é a versão utilizada na DLL publicada.
O script aplica o patch OpenAL incluído neste repositório e configura os runtimes
estáticos. Execute no PowerShell, ajustando os caminhos das ferramentas:

```powershell
git clone --recursive https://github.com/LeandroGowes/shadPS4-libretro.git
cd shadPS4-libretro
./scripts/build-libretro-windows.ps1 `
  -CompilerBin 'C:\tools\llvm-mingw-20260922-ucrt-x86_64\bin' `
  -CMake 'C:\tools\cmake\bin\cmake.exe' -Ninja 'C:\tools\ninja.exe'
```

Configuração manual equivalente, após inicializar os submódulos e aplicar o patch
`cmake/libretro-openal-clang.patch` ao submódulo `externals/openal-soft`:

```powershell
cmake -S . -B build-libretro -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER=clang.exe -DCMAKE_CXX_COMPILER=clang++.exe `
  -DENABLE_LIBRETRO=ON -DENABLE_TESTS=OFF `
  -DENABLE_DISCORD_RPC=OFF -DENABLE_UPDATER=OFF `
  -DSDL_SHARED=OFF -DSDL_STATIC=ON -DHAVE_ENDIAN_H=OFF
cmake --build build-libretro --target shadps4_libretro --parallel 4
```

No Windows, `LIBRETRO_STATIC_RUNTIME=ON` (padrão) incorpora libc++, libunwind e
winpthreads. Distribuir apenas `build-libretro/shadps4_libretro.dll` para este
núcleo. Ele ainda utiliza as bibliotecas de sistema do Windows e o driver Vulkan.
Com `LIBRETRO_STATIC_RUNTIME=OFF`, distribuir também `libc++.dll`, `libunwind.dll`
e `libwinpthread-1.dll` da mesma instalação LLVM-MinGW.
O submódulo OpenAL tem uma alteração local em `alc/alc.cpp`: o transform da
coleção EffectSlotCluster usa uma lambda para desambiguar `operator*` no Clang.
O patch preserva o commit oficial do submódulo e está incluído no fork.

## Integração

- API real `retro_*`, com vídeo XRGB8888, áudio estéreo 48 kHz e controles.
- Publica os descritores DualShock 4, incluindo os dois sticks, para que o
  frontend mantenha os analógicos separados do D-pad no modo automático.
- RetroPad B/A/Y/X correspondem a Cross/Circle/Square/Triangle.
- Renderização Vulkan do emulador em imagens fora da tela, com leitura para o
  callback de vídeo Libretro. Não abre uma janela SDL nem outro emulador.
- Aceita `eboot.bin`, ELF, ZAR ou diretório de jogo contendo `eboot.bin`.
- Dados do emulador: diretório de saves do frontend, em `shadPS4-libretro`.
- Módulos de sistema: diretório de BIOS do frontend, em `shadPS4/sys_modules`.
- Cada sessão precisa de um processo novo. O upstream ainda possui threads e
  estado global que não permitem reinicializar com segurança na mesma instância.
- A inicialização Libretro não exibe as caixas SDL de migração do standalone.
  Configurações TOML locais são convertidas pelo migrador oficial, quando possível.
- Savestates, cheats e troca de discos não são implementados nesta integração.

O Promus hospeda a DLL em `promus_core_worker.exe`. Os testes usam esse mesmo
host, com saves temporários, sem modificar os saves do usuário. Um teste de
callbacks não comprova sozinho que um jogo funciona; verificar também a imagem
e o log `shadPS4-libretro/log/shad_log.txt`.

## Linux

O upstream suporta Linux. Este alvo possui caminhos Linux, mas esta integração
local ainda não foi compilada ou testada nesse sistema. É necessário gerar uma
compilação separada, `shadps4_libretro.so`, com as dependências de build descritas
em `documents/building-linux.md`. A DLL de Windows não é um núcleo Linux.
