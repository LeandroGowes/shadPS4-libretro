# shadPS4 Libretro para Promus Play

Base de emulação: https://github.com/shadps4-emu/shadPS4

Revisão usada: `0fe263a4760dfbfa973366890061749b4af0de97` (main, 0.19.1 WIP).
Emulação desenvolvida pelo projeto shadPS4. Integração libretro e adaptações
para o Promus Play, incluindo vídeo, áudio, controles e execução pelo frontend.
Esta DLL é uma compilação deste fork, não um lançamento oficial do shadPS4.

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
- Falhas na alocação da memória de backing são devolvidas ao frontend pelo log
  Libretro, com o código original do Windows, tamanho solicitado e memória
  disponível. A sessão exige outro processo para uma nova tentativa.
- Savestates, cheats e troca de discos não são implementados nesta integração.

O Promus hospeda a DLL em `promus_core_worker.exe`. Os testes usam esse mesmo
host, com saves temporários, sem modificar os saves do usuário. Um teste de
callbacks não comprova sozinho que um jogo funciona; verificar também a imagem
e o log `shadPS4-libretro/log/shad_log.txt`.

## Linux

Compile com Git, CMake 3.24+, Ninja e Clang, usando as dependências de build
descritas em `documents/building-linux.md`:

```bash
./scripts/build-libretro-linux.sh
# Para ajustar o paralelismo:
JOBS=4 ./scripts/build-libretro-linux.sh
```

O resultado fica em `build-libretro-linux/dist/shadps4_libretro.so`. O core
Linux pode ser distribuído como um único `.so`; não precisa de `cores/lib`.
SDL, OpenAL, FFmpeg, compressão, fontes de interface, shaders e runtime C++
ficam incorporados ao core. `LIBRETRO_STATIC_RUNTIME=OFF` permite usar o runtime
C++ compartilhado. A configuração usa as dependências fixadas no repositório,
sem substituir por versões de desenvolvimento instaladas no sistema.

O vínculo Linux resolve os símbolos dentro do core e oculta os símbolos das
bibliotecas estáticas. Isso permite incorporar os arquivos FFmpeg fornecidos
pelo upstream e evita misturar o runtime C++ do core com o do frontend.

Ainda são necessários glibc, o driver/loader Vulkan do sistema e, quando um jogo
exigir, módulos e fontes extraídos do PS4 no diretório de BIOS do frontend.
Esses arquivos do console não são incluídos na distribuição. O binário produzido
usa a glibc da máquina de build; para suportar distribuições mais antigas,
compile no sistema mais antigo que deseja suportar. O alvo x86-64 requer AVX2.

Validação local: Clang 22.1.8, Linux x86-64, Burnout Paradise Remastered
(`CUSA10866`), host Linux do Promus em processo isolado. O pacote final passou
6.000 iterações em 100 segundos com vídeo 1920x1080, áudio 48 kHz e controles.
O encerramento e a reabertura são verificados separadamente em dois processos.
Esse teste verifica inicialização e execução inicial, não uma partida completa.
Savestates continuam indisponíveis. Use sempre um processo novo para cada sessão.

### Pacote com base antiga

Para distribuir entre distros, prefira a compilação isolada Ubuntu 22.04:

```bash
./scripts/build-libretro-portable.sh
```

O host precisa de Bubblewrap (`bwrap`), curl, tar, Git, ripgrep e binutils.
O script prepara Ubuntu Base 22.04.5 e GCC 14 dentro de
`build-libretro-portable/rootfs`, sem sudo ou alterar os pacotes do host.
O ambiente de compilação precisa de rede; o pacote final não precisa dele.
A imagem base vem de `https://cdimage.ubuntu.com/ubuntu-base/releases/22.04/release/`
e seu SHA-256 fica fixado no script.

Distribuição: `build-libretro-portable/shadps4-libretro-linux-x86_64.tar.gz`.
O core fica em `build-libretro-portable/build/dist/shadps4_libretro.so`, com
`release-metadata.json` e `shadps4_libretro.so.sha256` na mesma pasta. O script
também cria um checksum `.sha256` ao lado do arquivo tar.gz.
O core Linux e distribuído como um único `.so`: UUID v4 usa `getrandom()` e os
backends SDL/libusb não dependem de libudev. O script verifica as dependências
ELF e falha se houver uma biblioteca de runtime fora da glibc do sistema ou se
o pacote exigir glibc acima de 2.35. O runtime C++ continua incorporado.

Essa base permite uso em distribuições x86-64 com glibc 2.35 ou posterior e CPU
x86-64-v3/AVX2, com frontend e Vulkan já instalados. Sistemas com musl, glibc
mais antiga ou outra arquitetura precisam de uma compilação diferente.
O carregamento e a API Libretro foram verificados no ambiente Ubuntu 22.04.
No host Linux do Promus, Burnout Paradise Remastered passou 6.000 iterações
em 100 segundos, com vídeo 1920x1080, áudio e controles. O teste cobre a execução
inicial; não comprova uma partida completa nem todas as distribuições.
