# Install Task Spooler

First, clone the repository

```
git clone https://github.com/justanhduc/task-spooler
```

To setup Task Spooler with GPU support, one needs to set a `CUDA_HOME` environment variable.
Then, simple run the provided script

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
* Cannot find CUDA: Did you set a `CUDA_HOME` flag?
* ~~`/usr/bin/ld: cannot find -lnvidia-ml`: This lib lies in `$CUDA_HOME/lib64/stubs`.
  Please append this path to `LD_LIBRARY_PATH`.
  Sometimes, this problem persists even after adding the lib path.
  Then one can add `-L$(CUDA_HOME)/lib64/stubs` to [this line](./Makefile#L29) in the Makefile.~~
* `list.c:22:5: error: implicitly declaring library function 'snprintf' with type 'int (char *, unsigned long, const char *, ...)'`: 
Please remove `-D_XOPEN_SOURCE=500 -D__STRICT_ANSI__` in the Makefile as reported [here](https://github.com/justanhduc/task-spooler/issues/4).
* `Error calling recv_msg in c_check_version` and/or `Wrong server version`: This is because another version of `ts`
is still running. If you want to upgrade, please use the `reinstall` script as instructed above.
If you installed `ts` via `apt` before, you have to uninstall it and remove the server 
file in `/tmp` first before installing this `ts`. 
For more info, see [here](https://github.com/justanhduc/task-spooler/issues/6) and [here](https://github.com/justanhduc/task-spooler/issues/21).

# Uinstall Task Spooler

```
./uninstall
```
Why would you want to do that anyway?
