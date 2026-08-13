# vfncvtmxfp tests

The directory contains two layers of tests:

* The default target is an LLVM assembler, disassembler, and RISC-V ELF arch
  attribute smoke test for `vfncvtmxfp4.f.f.w` and `vfncvtmxfp8.f.f.w`.
* `runtime` is a software/hardware golden test. It executes the instructions
  and compares packed MXFP4/MXFP8 results and `fflags` against an independent
  integer FP32 conversion oracle.

The test expects an LLVM build with `xsaivfncvtmxfp` support. By default it
uses `$HOME/llvm/build/bin` when present, then falls back to `$HOME/llvm/bin`;
override the path when needed:

```sh
cd $AM_HOME/tests/vfncvtmxfp
make LLVM_BIN=$HOME/llvm/build/bin
```

It checks both unmasked instructions and the `v0.t` masked form, as well as
the `xsaivfncvtmxfp0p1` value emitted by `llvm-readelf -A`.

## Runtime golden test

Build the bare-metal image with the nexus-am toolchain:

```sh
cd $AM_HOME
AM_HOME=$PWD make -C tests/vfncvtmxfp runtime \
  ARCH=riscv64-xs CROSS_COMPILE=riscv64-linux-gnu-
```

Run the generated image on a NEMU configured for the `riscv64-xs` bare-metal
target:

```sh
$NEMU_HOME/build/riscv64-nemu-interpreter -b \
  tests/vfncvtmxfp/runtime/build/vfncvtmxfp-runtime-riscv64-xs.bin
```

The runtime cases cover all five rounding modes, scale codes at both UE8M0
endpoints and around unity, FP32 zeros/subnormals/normals/overflow values,
positive and negative infinities, quiet and signaling NaNs, deterministic
random inputs, masked-off lanes, tail-agnostic lanes, and `vl=0` destination
preservation. The oracle also checks the accumulated `NV`, `OF`, `UF`, and
`NX` flags.

The runtime source uses fixed `.word` encodings because the GNU cross
assembler does not yet know the custom mnemonics; the LLVM test above checks
the corresponding textual instruction forms.
