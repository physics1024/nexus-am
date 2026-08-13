  .attribute arch, "rv64gcv_xsaivfncvtmxfp0p1"

  .text
  .globl convert
  .p2align 2
convert:
  vsetvli zero, a0, e32, mf2, tu, ma
  vfncvtmxfp8.f.f.w v8, v9, v10
  vfncvtmxfp4.f.f.w v8, v9, v10
  vfncvtmxfp8.f.f.w v8, v9, v10, v0.t
  ret
