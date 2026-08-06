# Install Task Spooler

First, clone the repository

```
git clone https://github.com/justanhduc/task-spooler
```

The GPU features only require **NVML**, not the full CUDA compute toolkit. The
library itself (`libnvidia-ml.so.1`) ships with the NVIDIA driver, so nothing
extra is needed to *run* `ts`. To *build* it you also need the `nvml.h` header,
which no driver ships -- on Debian/Ubuntu it comes from `libnvidia-ml-dev`
(~83 KB, and the only file it installs is `/usr/include/nvml.h`):

```
sudo apt install libnvidia-ml-dev
```

A CUDA Toolkit install works too, since it carries `nvml.h` alongside a link
stub. If either lives somewhere custom, point the build at it with a `CUDA_HOME`
(or `CUDA_PATH`) environment variable, which is used only as a search hint.
Then simply run the provided script

```
./install_cmake
```
to use CMake, or
```
./install_make
```
to use Makefile. If Task Spooler has already been installed, 
and you want to reinstall or upgrade, you can run

```
./reinstall
```

## CPU version

If you would like to install only the CPU version, use the following commands (recommended)
```
make cpu
sudo make install
```
or via CMake
```
mkdir build && cd build
cmake .. -DTASK_SPOOLER_COMPILE_CUDA=OFF -DCMAKE_BUILD_TYPE=Release
make
sudo make install
```
This version is more faithful to the original `ts` but with many new exciting features, 
and is still being actively developed. 

## Local installation
To install without sudo privilege, one can replace the installation command by
```
make install-local
```

Note that, the installation will create a `bin` folder in `$HOME` if it does not exist.
To use `ts` anywhere, `$HOME/bin` needs to be added to `$PATH` if it hasn't been done already.
To use `man`, you may also need to add `$HOME/.local/share/man` to `$MANPATH`.

## Common problems
* Could NOT find NVML (missing `nvml.h`): install `libnvidia-ml-dev` (Debian /
Ubuntu) or a CUDA Toolkit, and set `CUDA_HOME`/`CUDA_PATH` if it lives somewhere
non-standard. A working `nvidia-smi` does *not* imply the header is present:
drivers ship runtime libraries only, never development headers. Beware that
`find / -name nvml.h` may turn up `/usr/include/hwloc/nvml.h`, which belongs to
hwloc and is a different file altogether.
* `ld: cannot find -lnvidia-ml` while the driver is clearly installed: some
hosts ship only the versioned `libnvidia-ml.so.1` without the unversioned
`libnvidia-ml.so` symlink that `-lnvidia-ml` needs -- WSL2 is one such case, as
it exposes the Windows driver under `/usr/lib/wsl/lib`. The CMake build handles
this by linking the versioned file directly; the Makefile build needs the
symlink, which `libnvidia-ml-dev` or a CUDA Toolkit provides.
* If you do not need GPU scheduling at all, build the CPU version with
`-DTASK_SPOOLER_COMPILE_CUDA=OFF`.
* `list.c:22:5: error: implicitly declaring library function 'snprintf' with type 'int (char *, unsigned long, const char *, ...)'`: 
Please remove `-D_XOPEN_SOURCE=500` in the Makefile as reported [here](https://github.com/justanhduc/task-spooler/issues/4).
* `Error calling recv_msg in c_check_version` and/or `Wrong server version`: This is because another version of `ts`
is still running. If you want to upgrade, please use the `reinstall` script as instructed above.
If you installed `ts` via `apt` before, you have to uninstall it and remove the server 
file in `/tmp` first before installing this `ts`. 
For more info, see [here](https://github.com/justanhduc/task-spooler/issues/6) and [here](https://github.com/justanhduc/task-spooler/issues/21).

# Uninstall Task Spooler

```
./uninstall
```
Or, to remove a local installation:
```
make uninstall-local
```
Why would you want to do that anyway?
