#include <klib.h>
#include <stdint.h>
#include <stdbool.h>

#define F_EX_NX 0x01u
#define F_EX_UF 0x02u
#define F_EX_OF 0x04u
#define F_EX_NV 0x10u

#define RM_RNE 0
#define RM_RTZ 1
#define RM_RDN 2
#define RM_RUP 3
#define RM_RMM 4

#define MXFP8_INSTR 0x3aa2245b
#define MXFP4_INSTR 0x3aa2145b
#define MXFP8_MASKED_INSTR 0x38a2245b

typedef struct {
  unsigned fraction_bits;
  int bias;
  int max_exponent;
  uint32_t max_significand;
  uint8_t max_finite;
  uint8_t nan;
} format_t;

static const format_t FORMAT_MXFP4 = {1, 1, 2, 3, 0x7, 0x7};
static const format_t FORMAT_MXFP8 = {3, 7, 8, 14, 0x7e, 0x7f};

static uint32_t random_state = 0x6d2b79f5u;

static uint32_t next_random(void) {
  random_state = random_state * 1664525u + 1013904223u;
  return random_state;
}

static int floor_log2_u32(uint32_t value) {
  int result = -1;
  while (value != 0) {
    value >>= 1;
    result++;
  }
  return result;
}

static bool exceeds(uint32_t significand, int power, uint32_t limit,
                    int limit_power) {
  int top = floor_log2_u32(significand) + power;
  int limit_top = floor_log2_u32(limit) + limit_power;
  if (top != limit_top)
    return top > limit_top;
  if (power >= limit_power)
    return ((uint64_t)significand << (power - limit_power)) > limit;
  return significand > ((uint64_t)limit << (limit_power - power));
}

static uint64_t round_binary(uint32_t significand, int shift, bool sign,
                             unsigned rm, bool *inexact) {
  if (shift <= 0) {
    *inexact = false;
    return (uint64_t)significand << -shift;
  }

  uint64_t truncated = shift < 64 ? (uint64_t)significand >> shift : 0;
  uint64_t remainder = shift < 64
      ? (uint64_t)significand & ((UINT64_C(1) << shift) - 1)
      : significand;
  *inexact = remainder != 0;
  if (!*inexact)
    return truncated;

  bool increment = false;
  switch (rm) {
    case RM_RNE:
      if (shift < 64) {
        uint64_t half = UINT64_C(1) << (shift - 1);
        increment = remainder > half || (remainder == half && (truncated & 1));
      }
      break;
    case RM_RDN: increment = sign; break;
    case RM_RUP: increment = !sign; break;
    case RM_RMM:
      if (shift < 64)
        increment = remainder >= (UINT64_C(1) << (shift - 1));
      break;
    case RM_RTZ: break;
  }
  return truncated + increment;
}

static uint8_t oracle_mxfp(uint32_t source, uint8_t scale_code,
                           unsigned width, unsigned rm, uint32_t *flags) {
  const format_t *format = width == 4 ? &FORMAT_MXFP4 : &FORMAT_MXFP8;
  uint32_t sign = source >> 31;
  uint32_t exponent = (source >> 23) & 0xff;
  uint32_t fraction = source & 0x7fffff;
  bool is_nan = exponent == 0xff && fraction != 0;
  bool is_snan = is_nan && (fraction & 0x400000) == 0;
  uint8_t encoded_sign = sign << (width - 1);
  *flags = 0;

  if (scale_code == 0xff) {
    if (width == 4 || is_snan)
      *flags = F_EX_NV;
    return format->nan;
  }
  if (is_nan) {
    if (width == 4 || is_snan)
      *flags = F_EX_NV;
    return format->nan;
  }
  if (exponent == 0xff) {
    *flags = F_EX_OF | F_EX_NX;
    return encoded_sign | format->max_finite;
  }
  if (exponent == 0 && fraction == 0)
    return encoded_sign;

  uint32_t significand;
  int power;
  if (exponent == 0) {
    significand = fraction;
    power = -22 - scale_code;
  } else {
    significand = 0x800000 | fraction;
    power = (int)exponent - scale_code - 23;
  }

  int max_power = format->max_exponent - format->fraction_bits;
  if (exceeds(significand, power, format->max_significand, max_power)) {
    *flags = F_EX_OF | F_EX_NX;
    return encoded_sign | format->max_finite;
  }

  int top_exponent = floor_log2_u32(significand) + power;
  int min_exponent = 1 - format->bias;
  bool inexact;
  uint64_t rounded;
  uint8_t result;
  if (top_exponent < min_exponent) {
    int quantum_power = min_exponent - format->fraction_bits;
    rounded = round_binary(significand, quantum_power - power, sign, rm,
                           &inexact);
    uint64_t min_normal = UINT64_C(1) << format->fraction_bits;
    if (rounded >= min_normal) {
      result = min_normal;
    } else {
      result = rounded;
      if (inexact)
        *flags |= F_EX_UF;
    }
  } else {
    int result_exponent = top_exponent;
    int shift = floor_log2_u32(significand) - format->fraction_bits;
    rounded = round_binary(significand, shift, sign, rm, &inexact);
    if (rounded == (UINT64_C(1) << (format->fraction_bits + 1))) {
      rounded >>= 1;
      result_exponent++;
    }
    result = ((result_exponent + format->bias) << format->fraction_bits)
           | (rounded - (UINT64_C(1) << format->fraction_bits));
  }
  if (inexact)
    *flags |= F_EX_NX;
  return encoded_sign | result;
}

static uint8_t packed_element(const uint8_t *output, unsigned width,
                              unsigned index) {
  if (width == 8)
    return output[index];
  return (output[index / 2] >> ((index & 1) * 4)) & 0xf;
}

static uint32_t run_conversion(const uint32_t *input, uint8_t scale,
                               unsigned width, unsigned rm, uint8_t *output) {
  uint32_t flags;
  uint64_t scale_value = scale;
  for (int i = 0; i < 16; i++)
    output[i] = 0xa5;

  if (width == 8) {
    asm volatile("csrw frm, %4\n"
                 "csrwi fflags, 0\n"
                 "vsetivli zero, 4, e32, m1, ta, ma\n"
                 "vle32.v v4, (%1)\n"
                 "vmv.s.x v10, %3\n"
                 ".word 0x3aa2245b\n"
                 "vsetivli zero, 16, e8, m1, ta, ma\n"
                 "vse8.v v8, (%2)\n"
                 "csrr %0, fflags\n"
                 : "=r"(flags)
                 : "r"(input), "r"(output), "r"(scale_value), "r"(rm)
                 : "v4", "v8", "v10", "memory");
  } else {
    asm volatile("csrw frm, %4\n"
                 "csrwi fflags, 0\n"
                 "vsetivli zero, 4, e32, m1, ta, ma\n"
                 "vle32.v v4, (%1)\n"
                 "vmv.s.x v10, %3\n"
                 ".word 0x3aa2145b\n"
                 "vsetivli zero, 16, e8, m1, ta, ma\n"
                 "vse8.v v8, (%2)\n"
                 "csrr %0, fflags\n"
                 : "=r"(flags)
                 : "r"(input), "r"(output), "r"(scale_value), "r"(rm)
                 : "v4", "v8", "v10", "memory");
  }
  return flags;
}

static uint32_t run_masked(const uint32_t *input, uint8_t scale,
                           unsigned width, unsigned rm, uint8_t *mask,
                           uint8_t *output) {
  uint32_t flags;
  uint64_t scale_value = scale;
  for (int i = 0; i < 16; i++)
    output[i] = 0xa5;

  if (width == 8) {
    asm volatile("csrw frm, %4\n"
                 "csrwi fflags, 0\n"
                 "vsetivli zero, 4, e8, m1, tu, mu\n"
                 "vlm.v v0, (%3)\n"
                 "vle8.v v8, (%2)\n"
                 "vsetivli zero, 4, e32, m1, tu, mu\n"
                 "vle32.v v4, (%1)\n"
                 "vmv.s.x v10, %5\n"
                 ".word 0x38a2245b\n"
                 "vsetivli zero, 16, e8, m1, tu, mu\n"
                 "vse8.v v8, (%2)\n"
                 "csrr %0, fflags\n"
                 : "=r"(flags)
                 : "r"(input), "r"(output), "r"(mask), "r"(rm),
                   "r"(scale_value)
                 : "v0", "v4", "v8", "v10", "memory");
  } else {
    asm volatile("csrw frm, %4\n"
                 "csrwi fflags, 0\n"
                 "vsetivli zero, 4, e8, m1, tu, mu\n"
                 "vlm.v v0, (%3)\n"
                 "vle8.v v8, (%2)\n"
                 "vsetivli zero, 4, e32, m1, tu, mu\n"
                 "vle32.v v4, (%1)\n"
                 "vmv.s.x v10, %5\n"
                 ".word 0x38a2145b\n"
                 "vsetivli zero, 16, e8, m1, tu, mu\n"
                 "vse8.v v8, (%2)\n"
                 "csrr %0, fflags\n"
                 : "=r"(flags)
                 : "r"(input), "r"(output), "r"(mask), "r"(rm),
                   "r"(scale_value)
                 : "v0", "v4", "v8", "v10", "memory");
  }
  return flags;
}

static int compare_batch(const uint32_t *input, uint8_t scale, unsigned width,
                         unsigned rm, const char *name) {
  uint8_t output[16];
  uint32_t actual_flags = run_conversion(input, scale, width, rm, output);
  uint32_t expected_flags = 0;
  int failures = 0;
  for (unsigned i = 0; i < 4; i++) {
    uint32_t element_flags;
    uint8_t expected = oracle_mxfp(input[i], scale, width, rm, &element_flags);
    expected_flags |= element_flags;
    uint8_t actual = packed_element(output, width, i);
    if (actual != expected) {
      printf("FAIL %s lane%u input=0x%08x scale=%u rm=%u got=0x%x expected=0x%x\n",
             name, i, input[i], scale, rm, actual, expected);
      failures++;
    }
  }
  if (actual_flags != expected_flags) {
    printf("FAIL %s flags scale=%u rm=%u got=0x%x expected=0x%x\n",
           name, scale, rm, actual_flags, expected_flags);
    failures++;
  }
  return failures;
}

static int test_boundaries(void) {
  static const uint32_t values[] = {
    0x00000000, 0x80000000, 0x3f800000, 0x40000000,
    0x3a800000, 0x3b000000, 0x3c700000,
    0x3fa00000, 0x3fe00000, 0x40c00000,
    0x41000000, 0x43e00000, 0x43e08000,
    0x7f800000, 0xff800000, 0x7fc00000, 0x7f800001,
  };
  static const uint8_t scales[] = {0, 126, 127, 128, 254, 255};
  uint32_t input[4];
  int failures = 0;
  for (unsigned width = 4; width <= 8; width += 4) {
    for (unsigned scale_i = 0; scale_i < sizeof(scales); scale_i++) {
      for (unsigned rm = 0; rm < 5; rm++) {
        for (unsigned base = 0; base < sizeof(values) / sizeof(values[0]);
             base += 4) {
          for (unsigned lane = 0; lane < 4; lane++)
            input[lane] = values[base + lane];
          failures += compare_batch(input, scales[scale_i], width, rm,
                                     "boundary");
        }
      }
    }
  }
  return failures;
}

static int test_random(void) {
  int failures = 0;
  uint32_t input[4];
  for (unsigned width = 4; width <= 8; width += 4) {
    for (unsigned rm = 0; rm < 5; rm++) {
      for (unsigned scale = 0; scale < 256; scale += 17) {
        for (unsigned iteration = 0; iteration < 32; iteration++) {
          for (unsigned lane = 0; lane < 4; lane++)
            input[lane] = next_random();
          failures += compare_batch(input, scale, width, rm, "random");
        }
      }
    }
  }
  return failures;
}

static int test_masked(void) {
  uint32_t input[4] = {0x3f800000, 0x40000000, 0x3f800000, 0x40000000};
  uint8_t mask[1] = {0x5};
  uint8_t output[16];
  int failures = 0;
  for (unsigned width = 4; width <= 8; width += 4) {
    uint32_t flags = run_masked(input, 127, width, RM_RNE, mask, output);
    if (flags != 0) {
      printf("FAIL masked flags width=%u got=0x%x\n", width, flags);
      failures++;
    }
    for (unsigned lane = 0; lane < 4; lane++) {
      uint8_t sentinel = width == 8 ? 0xa5 : (lane & 1 ? 0xa : 0x5);
      uint8_t expected = lane & 1 ? sentinel :
          (width == 8 ? 0x38 : 0x2);
      uint8_t actual = packed_element(output, width, lane);
      if (actual != expected) {
        printf("FAIL masked width=%u lane=%u got=0x%x expected=0x%x\n",
               width, lane, actual, expected);
        failures++;
      }
    }
  }
  return failures;
}

static int test_tail_and_zero_vl(void) {
  uint32_t input[4] = {0x3f800000, 0x40000000, 0, 0};
  uint8_t output[16];
  uint32_t flags;
  for (int i = 0; i < 16; i++)
    output[i] = 0xa5;

  asm volatile("csrwi frm, 0\n"
               "csrwi fflags, 0\n"
               "vsetivli zero, 2, e32, m1, ta, ma\n"
               "vle32.v v4, (%1)\n"
               "li t0, 127\n"
               "vmv.s.x v10, t0\n"
               ".word 0x3aa2245b\n"
               "vsetivli zero, 16, e8, m1, ta, ma\n"
               "vse8.v v8, (%2)\n"
               "csrr %0, fflags\n"
               : "=r"(flags)
               : "r"(input), "r"(output)
               : "v4", "v8", "v10", "t0", "memory");
  int failures = 0;
  if (output[0] != 0x38 || output[1] != 0x40 || output[2] != 0xff
      || output[3] != 0xff || flags != 0) {
    printf("FAIL tail output=%02x%02x%02x%02x flags=%x\n",
           output[3], output[2], output[1], output[0], flags);
    failures++;
  }

  for (int i = 0; i < 16; i++)
    output[i] = 0x5a;
  asm volatile("csrwi fflags, 0\n"
               "vsetivli zero, 16, e8, m1, tu, mu\n"
               "vle8.v v8, (%1)\n"
               "vsetivli zero, 0, e32, m1, ta, ma\n"
               ".word 0x3aa2245b\n"
               "vsetivli zero, 16, e8, m1, tu, mu\n"
               "vse8.v v8, (%1)\n"
               "csrr %0, fflags\n"
               : "=r"(flags)
               : "r"(output)
               : "v8", "v4", "v10", "memory");
  for (int i = 0; i < 16; i++) {
    if (output[i] != 0x5a)
      failures++;
  }
  if (flags != 0) {
    printf("FAIL vl=0 flags=%x\n", flags);
    failures++;
  }
  return failures;
}

int main(void) {
  asm volatile("lui t0, 0x2\n"
               "addiw t0, t0, 512\n"
               "csrs mstatus, t0\n"
               "csrwi vcsr, 0" ::: "t0", "memory");
  int failures = 0;
  printf("Testing vfncvtmxfp boundary and stability cases\n");
  failures += test_boundaries();
  failures += test_masked();
  failures += test_tail_and_zero_vl();
  failures += test_random();
  if (failures == 0) {
    printf("PASS: vfncvtmxfp boundary/stability tests\n");
    return 0;
  }
  printf("FAIL: vfncvtmxfp had %d failures\n", failures);
  return 1;
}
