# Contributing to Ilang

Bug reports, fixes and improvements are welcome. Thanks for your interest.

## Building

You need a C++17 compiler for `Ilangc` and a C11 compiler for `IlangVM` and `Ilangdis`. The build commands are in the
[README](README.md#building); `build.sh` / `build.bat` build all three tools.


## Things to keep in step

- **Bytecode.** `Ilangc` emits it, `IlangVM` runs it and `Ilangdis` decodes it. The compiler has its own opcode list in
  `Ilangc/ilang.hpp`, while `Ilangdis` uses the VM's headers. If you add or change an instruction, change all three,
  and keep `kBytecodeVersion` in `Ilangc/ilang.hpp` equal to `SUPPORTED_VERSION` in `IlangVM/VM/IlangConf.h`.
- **The language.** [docs/SPEC.md](docs/SPEC.md) says every code example in it was compiled with `Ilangc`. If you
  change the language, update the specification, and check any example you touch.
- **Comments.** The source is documented with Doxygen comments. Match the density, naming and style of the code around
  your change. `doxygen Doxyfile` should not produce new warnings.

## Reporting bugs

Please open an issue with:

- the smallest Ilang program (`.i`) that shows the problem,
- what you expected and what happened, including the compiler or VM output,
- your operating system, the compiler you built with, and whether you used `build.sh` / `build.bat` or your own flags.

## AI-assisted contributions

Parts of this project were written with AI tools, so AI assistance is welcome in
contributions too. You are responsible for what you submit: read and understand the code, build it.
Please mention in the pull request if a substantial part was AI-generated.

## License

By contributing you agree that your contribution is licensed under the [Apache License 2.0](LICENSE.md).