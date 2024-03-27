// Copyright 2024 Google LLC
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "hwy/base.h"

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "tests/fast_float_to_int_conv_test.cc"
#include "hwy/foreach_target.h"  // IWYU pragma: keep
#include "hwy/highway.h"
#include "hwy/nanobenchmark.h"
#include "hwy/tests/test_util-inl.h"

HWY_BEFORE_NAMESPACE();
namespace hwy {
namespace HWY_NAMESPACE {

// HWY_FAST_F2I_CONV_TEST_CONST_ASSERT(condition, msg) checks that condition is
// true using static_assert if constexpr BitCastScalar is available and Highway
// is being compiled in C++11 mode.
//
// Otherwise, if constexpr BitCastScalar is not available or Highway is being
// compiled in C++11 mode, HWY_FAST_F2I_CONV_TEST_CONST_ASSERT(condition, msg)
// checks that condition is true using a run-time assertion.
#if (HWY_HAS_BUILTIN(__builtin_bit_cast) || HWY_COMPILER_MSVC >= 1926) && \
    __cpp_constexpr >= 201304L
#define HWY_FAST_F2I_CONV_TEST_CONST_ASSERT(condition, msg) \
  static_assert((condition), msg)
#else
#define HWY_FAST_F2I_CONV_TEST_CONST_ASSERT(condition, msg) \
  do {                                                      \
    if (HWY_UNLIKELY(!(condition))) {                       \
      HWY_ABORT("Assert %s failed:\n%s", #condition, msg);  \
    }                                                       \
  } while (0)
#endif

template <class TTo>
class TestFastConvertFloatToInt {
  static_assert(!IsFloat<TTo>() && !IsSpecialFloat<TTo>(),
                "TTo must be an integer type");

 private:
  // LargestLt1FloatVal<T>() returns the largest value of T that is less than 1
  template <class T, HWY_IF_FLOAT_OR_SPECIAL(T)>
  static HWY_INLINE HWY_BITCASTSCALAR_CONSTEXPR T LargestLt1FloatVal() {
    using TU = MakeUnsigned<T>;
    return BitCastScalar<T>(
        static_cast<TU>(BitCastScalar<TU>(ConvertScalarTo<T>(1)) - 1u));
  }

  // RoundedDownFloatSum(hi, lo) returns the rounded down value of hi + lo

  // RoundedDownFloatSum(hi, lo) should only be called if
  // (ScalarAbs(hi) >= ScalarAbs(lo) || hi == 0) is true
  template <class T, HWY_IF_FLOAT(T)>
  static HWY_INLINE HWY_BITCASTSCALAR_CXX14_CONSTEXPR RemoveCvRef<T>
  RoundedDownFloatSum(T hi, T lo) {
    using NonCvRefT = RemoveCvRef<T>;
    using TU = MakeUnsigned<NonCvRefT>;

    const NonCvRefT sum = static_cast<NonCvRefT>(hi + lo);
    const NonCvRefT carry = static_cast<NonCvRefT>((hi - sum) + lo);

    const TU sum_bits = BitCastScalar<TU>(sum);
    const TU carry_bits = BitCastScalar<TU>(carry);

    return BitCastScalar<NonCvRefT>(static_cast<TU>(
        sum_bits - (((sum_bits ^ carry_bits) >> (sizeof(TU) * 8 - 1)) &
                    static_cast<TU>(carry != 0))));
  }

  // ConvIntToRoundedDownF64 converts val to a F64, with inexact conversion
  // rounded down
  template <class T, HWY_IF_T_SIZE_LE(T, 4)>
  static HWY_INLINE constexpr double ConvIntToRoundedDownF64(T val) {
    return static_cast<double>(val);
  }

  template <class T, HWY_IF_T_SIZE(T, 8)>
  static HWY_INLINE HWY_BITCASTSCALAR_CXX14_CONSTEXPR double
  ConvIntToRoundedDownF64(T val) {
    using NonCvRefT = RemoveCvRef<T>;
    return RoundedDownFloatSum(
        static_cast<double>(static_cast<NonCvRefT>(static_cast<uint64_t>(val) &
                                                   0xFFE0000000000000ULL)),
        static_cast<double>(static_cast<NonCvRefT>(static_cast<uint64_t>(val) &
                                                   0x001FFFFFFFFFFFFFULL)));
  }

  // RoundFloatDownToPrecision rounds val down to a floating-point value with a
  // mantissa that has at most kBitPrecision bits of precision
  template <int kBitPrecision, class T, HWY_IF_FLOAT_OR_SPECIAL(T)>
  static HWY_INLINE HWY_BITCASTSCALAR_CXX14_CONSTEXPR RemoveCvRef<T>
  RoundFloatDownToPrecision(T val) {
    static_assert(kBitPrecision > 0, "kBitPrecision > 0 must be true");

    using NonCvRefT = RemoveCvRef<T>;
    using TU = MakeUnsigned<NonCvRefT>;

    // kTMantBitPrecision is the number of bits in the mantissa of val,
    // including the implied one bit
    constexpr int kTMantBitPrecision = MantissaBits<NonCvRefT>() + 1;
    static_assert(kTMantBitPrecision > 0,
                  "kTMantBitPrecision > 0 must be true");

    constexpr int kNumOfBitsToZeroOut =
        HWY_MAX(kTMantBitPrecision - kBitPrecision, 0);
    constexpr TU kZeroOutMask =
        static_cast<TU>((1ULL << kNumOfBitsToZeroOut) - 1ULL);

    return BitCastScalar<NonCvRefT>(
        static_cast<TU>(BitCastScalar<TU>(val) & (~kZeroOutMask)));
  }

  // LowestInRangeValForF2IConv<TFrom> returns the lowest finite value of TFrom
  // that is greater than LimitsMin<TTo>() - 1
  template <class TFrom>
  static HWY_INLINE HWY_BITCASTSCALAR_CXX14_CONSTEXPR TFrom
  LowestInRangeValForF2IConv() {
    using TFArith = If<(sizeof(TFrom) <= sizeof(float)), float, double>;

    // kTFromMantBitPrecision is equal to the number of bits in the mantissa of
    // TFrom, including the implied one bit
    constexpr int kTFromMantBitPrecision = MantissaBits<TFrom>() + 1;

    HWY_BITCASTSCALAR_CXX14_CONSTEXPR const TFArith kLowestTFromVal =
        ConvertScalarTo<TFArith>(LowestValue<TFrom>());
    HWY_BITCASTSCALAR_CXX14_CONSTEXPR const TFArith kLowestTToVal =
        RoundFloatDownToPrecision<kTFromMantBitPrecision>(RoundedDownFloatSum(
            static_cast<TFArith>(LimitsMin<TTo>()),
            static_cast<TFArith>(-LargestLt1FloatVal<TFArith>())));
    HWY_BITCASTSCALAR_CXX14_CONSTEXPR const TFArith kLowestInRangeVal =
        HWY_MAX(kLowestTFromVal, kLowestTToVal);

    HWY_FAST_F2I_CONV_TEST_CONST_ASSERT(
        ScalarIsFinite(kLowestInRangeVal) &&
            kLowestInRangeVal >= static_cast<TFArith>(LimitsMin<int64_t>()),
        "kLowestInRangeVal must be a finite value that is greater than or "
        "equal to LimitsMin<int64_t>()");
    HWY_FAST_F2I_CONV_TEST_CONST_ASSERT(
        kLowestInRangeVal < static_cast<TFArith>(0.0),
        "kLowestInRangeVal must be less than zero");

    // Disable the C4056 (overflow in floating-point constant arithmetic)
    // warning that MSVC generates when kLowestInRangeVal is cast to an
    // int64_t in the HWY_FAST_F2I_CONV_TEST_CONST_ASSERT statements below as
    // kLowestInRangeVal is known to be within the range of an int64_t due to
    // the HWY_FAST_F2I_CONV_TEST_CONST_ASSERT checks above

#if HWY_COMPILER_MSVC
    HWY_DIAGNOSTICS(push)
    HWY_DIAGNOSTICS_OFF(disable : 4056, ignored "-Woverflow")
#endif

    HWY_FAST_F2I_CONV_TEST_CONST_ASSERT(
        static_cast<int64_t>(kLowestInRangeVal) <= 0,
        "static_cast<int64_t>(kLowestInRangeVal) must be less than "
        "or equal to 0");
    HWY_FAST_F2I_CONV_TEST_CONST_ASSERT(
        static_cast<int64_t>(kLowestInRangeVal) >=
            static_cast<int64_t>(LimitsMin<TTo>()),
        "static_cast<int64_t>(kLowestInRangeVal) must be greater "
        "than or equal to LimitsMin<TTo>()");

#if HWY_COMPILER_MSVC
    HWY_DIAGNOSTICS(pop)
#endif

    return ConvertScalarTo<TFrom>(kLowestInRangeVal);
  }

  // HighestInRangeValForF2IConv<TFrom> returns the largest finite value of
  // TFrom that is less than LimitsMax<TTo>() + 1
  template <class TFrom>
  static HWY_INLINE HWY_BITCASTSCALAR_CXX14_CONSTEXPR TFrom
  HighestInRangeValForF2IConv() {
    using TFArith = If<(sizeof(TFrom) <= sizeof(float)), float, double>;

    // kTFromMantBitPrecision is equal to the number of bits in the mantissa of
    // TFrom, including the implied one bit
    constexpr int kTFromMantBitPrecision = MantissaBits<TFrom>() + 1;

    HWY_BITCASTSCALAR_CXX14_CONSTEXPR const TFArith kHighestTFromVal =
        ConvertScalarTo<TFArith>(HighestValue<TFrom>());
    HWY_BITCASTSCALAR_CXX14_CONSTEXPR const TFArith kHighestTToVal =
        static_cast<TFArith>(RoundFloatDownToPrecision<kTFromMantBitPrecision>(
            RoundedDownFloatSum(ConvIntToRoundedDownF64(LimitsMax<TTo>()),
                                LargestLt1FloatVal<double>())));
    HWY_BITCASTSCALAR_CXX14_CONSTEXPR const TFArith kHighestInRangeVal =
        HWY_MIN(kHighestTFromVal, kHighestTToVal);

    HWY_FAST_F2I_CONV_TEST_CONST_ASSERT(
        ScalarIsFinite(kHighestInRangeVal) &&
            kHighestInRangeVal < static_cast<TFArith>(18446744073709551616.0),
        "kHighestInRangeVal must be a finite value that is less than or "
        "equal to LimitsMax<uint64_t>()");
    HWY_FAST_F2I_CONV_TEST_CONST_ASSERT(
        kHighestInRangeVal > 0, "kHighestInRangeVal must be greater than 0");
    HWY_FAST_F2I_CONV_TEST_CONST_ASSERT(
        static_cast<uint64_t>(kHighestInRangeVal) > 0,
        "static_cast<uint64_t>(kHighestInRangeVal) must be greater than 0");
    HWY_FAST_F2I_CONV_TEST_CONST_ASSERT(
        static_cast<uint64_t>(kHighestInRangeVal) <=
            static_cast<uint64_t>(LimitsMax<TTo>()),
        "static_cast<uint64_t>(kHighestInRangeVal) must be less "
        "than or equal to LimitsMax<TTo>()");

    return ConvertScalarTo<TFrom>(kHighestInRangeVal);
  }

  template <class DTo, class VFrom,
            HWY_IF_T_SIZE_LE_D(DTo, sizeof(TFromV<VFrom>) - 1)>
  static HWY_INLINE Vec<DTo> DoConvVector(DTo d_to, VFrom v_from) {
    return DemoteTo(d_to, v_from);
  }
  template <class DTo, class VFrom, HWY_IF_T_SIZE_D(DTo, sizeof(TFromV<VFrom>)),
            hwy::EnableIf<IsFloat<TFromD<DTo>>() == IsFloat<TFromV<VFrom>>()>* =
                nullptr>
  static HWY_INLINE Vec<DTo> DoConvVector(DTo d_to, VFrom v_from) {
    return BitCast(d_to, v_from);
  }
  template <class DTo, class VFrom, HWY_IF_T_SIZE_D(DTo, sizeof(TFromV<VFrom>)),
            hwy::EnableIf<IsFloat<TFromD<DTo>>() != IsFloat<TFromV<VFrom>>()>* =
                nullptr>
  static HWY_INLINE Vec<DTo> DoConvVector(DTo d_to, VFrom v_from) {
    return ConvertTo(d_to, v_from);
  }
  template <class DTo, class VFrom,
            HWY_IF_T_SIZE_GT_D(DTo, sizeof(TFromV<VFrom>))>
  static HWY_INLINE Vec<DTo> DoConvVector(DTo d_to, VFrom v_from) {
    return PromoteTo(d_to, v_from);
  }
  template <class DTo, class VFrom,
            HWY_IF_T_SIZE_LE_D(DTo, sizeof(TFromV<VFrom>) - 1)>
  static HWY_INLINE Vec<DTo> DoFastF2IConvVector(DTo d_to, VFrom v_from) {
    return FastDemoteTo(d_to, v_from);
  }
  template <class DTo, class VFrom, HWY_IF_T_SIZE_D(DTo, sizeof(TFromV<VFrom>))>
  static HWY_INLINE Vec<DTo> DoFastF2IConvVector(DTo d_to, VFrom v_from) {
    return FastConvertTo(d_to, v_from);
  }
  template <class DTo, class VFrom,
            HWY_IF_T_SIZE_GT_D(DTo, sizeof(TFromV<VFrom>))>
  static HWY_INLINE Vec<DTo> DoFastF2IConvVector(DTo d_to, VFrom v_from) {
    return FastPromoteTo(d_to, v_from);
  }

 public:
  template <typename TFrom, class DFrom>
  HWY_NOINLINE void operator()(TFrom /*unused*/, DFrom d_from) {
    static_assert(IsFloat<TFrom>(), "TFrom must be a floating-point type");

    using TIFrom = MakeSigned<TFrom>;
    using TUFrom = MakeUnsigned<TFrom>;
    const RebindToSigned<decltype(d_from)> di_from;
    const RebindToUnsigned<decltype(d_from)> du_from;

    HWY_BITCASTSCALAR_CXX14_CONSTEXPR const TFrom kLowestInRangeFltVal =
        LowestInRangeValForF2IConv<TFrom>();
    HWY_BITCASTSCALAR_CXX14_CONSTEXPR const TFrom kHighestInRangeFltVal =
        HighestInRangeValForF2IConv<TFrom>();

    HWY_BITCASTSCALAR_CXX14_CONSTEXPR const TUFrom kLowestInRangeFltValBits =
        BitCastScalar<TUFrom>(kLowestInRangeFltVal);
    HWY_BITCASTSCALAR_CXX14_CONSTEXPR const TUFrom kHighestInRangeFltValBits =
        BitCastScalar<TUFrom>(kHighestInRangeFltVal);

    HWY_BITCASTSCALAR_CXX14_CONSTEXPR const TUFrom kMinOutOfRangeRandFltBits =
        static_cast<TUFrom>(
            HWY_MAX(kLowestInRangeFltValBits &
                        static_cast<TUFrom>(LimitsMax<TIFrom>()),
                    kHighestInRangeFltValBits) +
            1u);

    HWY_FAST_F2I_CONV_TEST_CONST_ASSERT(
        kMinOutOfRangeRandFltBits > kHighestInRangeFltValBits,
        "kMinOutOfRangeRandFltBits > kHighestInRangeFltValBits must be true");
    HWY_FAST_F2I_CONV_TEST_CONST_ASSERT(
        kMinOutOfRangeRandFltBits <= static_cast<TUFrom>(LimitsMax<TIFrom>()),
        "kMinOutOfRangeRandFltBits <= LimitsMax<TIFrom>() must be true");

    HWY_BITCASTSCALAR_CXX14_CONSTEXPR const TTo kLowestInRangeIntVal =
        ConvertScalarTo<TTo>(kLowestInRangeFltVal);
    HWY_BITCASTSCALAR_CXX14_CONSTEXPR const TTo kHighestInRangeIntVal =
        ConvertScalarTo<TTo>(kHighestInRangeFltVal);

    const Rebind<TTo, decltype(d_from)> d_to;

    HWY_ASSERT_VEC_EQ(
        d_to, Set(d_to, static_cast<TTo>(0)),
        DoFastF2IConvVector(d_to, Set(d_from, ConvertScalarTo<TFrom>(0))));
    HWY_ASSERT_VEC_EQ(
        d_to, Set(d_to, static_cast<TTo>(1)),
        DoFastF2IConvVector(d_to, Set(d_from, ConvertScalarTo<TFrom>(1))));

    if (IsSigned<TTo>()) {
      HWY_ASSERT_VEC_EQ(
          d_to, Set(d_to, static_cast<TTo>(-1)),
          DoFastF2IConvVector(d_to, Set(d_from, ConvertScalarTo<TFrom>(-1))));
    }

    HWY_ASSERT_VEC_EQ(
        d_to, Set(d_to, kLowestInRangeIntVal),
        DoFastF2IConvVector(d_to, Set(d_from, kLowestInRangeFltVal)));
    HWY_ASSERT_VEC_EQ(
        d_to, Set(d_to, kHighestInRangeIntVal),
        DoFastF2IConvVector(d_to, Set(d_from, kHighestInRangeFltVal)));

    constexpr TIFrom kIotaMask =
        static_cast<TIFrom>(static_cast<uint64_t>(MantissaMask<TFrom>()) &
                            static_cast<uint64_t>(LimitsMax<TTo>() / 2));

    const auto flt_iota = DoConvVector(
        d_from, Add(And(Iota(di_from, TIFrom{0}), Set(di_from, kIotaMask)),
                    Set(di_from, TIFrom{1})));
    const auto expected_f2i_iota =
        Add(And(Iota(d_to, TTo{0}), Set(d_to, static_cast<TTo>(kIotaMask))),
            Set(d_to, TTo{1}));
    HWY_ASSERT_VEC_EQ(d_to, expected_f2i_iota, DoConvVector(d_to, flt_iota));

    const size_t N = Lanes(d_from);
    auto from_lanes = AllocateAligned<TFrom>(N);
    auto expected = AllocateAligned<TTo>(N);
    HWY_ASSERT(from_lanes && expected);

    constexpr int kMaxBiasedExp = static_cast<int>(MaxExponentField<TFrom>());
    static_assert(kMaxBiasedExp > 0, "kMaxBiasedExp > 0 must be true");

    constexpr int kExpBias = kMaxBiasedExp >> 1;
    static_assert(kExpBias > 0, "kExpBias > 0 must be true");

    constexpr int kMinOutOfRangeBiasedExp =
        static_cast<int>(HWY_MIN(static_cast<unsigned>(kExpBias) +
                                     static_cast<unsigned>(sizeof(TTo) * 8) -
                                     static_cast<unsigned>(IsSigned<TTo>()),
                                 static_cast<unsigned>(kMaxBiasedExp)));
    static_assert(kMinOutOfRangeBiasedExp > 0,
                  "kMinOutOfRangeBiasedExp > 0 must be true");
    static_assert(
        (kMaxBiasedExp - kMinOutOfRangeBiasedExp + 1) > 0,
        "kMaxBiasedExp - kMinOutOfRangeBiasedExp + 1 must be greater than 0");

    constexpr int kNumOfMantBits = MantissaBits<TFrom>();
    static_assert(kNumOfMantBits > 0, "kNumOfMantBits > 0 must be true");

    constexpr TUFrom kExpMask = ExponentMask<TFrom>();
    constexpr TUFrom kMantAndSignMask = static_cast<TUFrom>(
        (~kExpMask) &
        (IsSigned<TTo>() ? LimitsMax<TUFrom>()
                         : static_cast<TUFrom>(LimitsMax<TIFrom>())));

    const int non_elided_one = Unpredictable1();

    const auto pos_inf = BitCast(
        d_from,
        Set(du_from,
            static_cast<TUFrom>(
                kExpMask | static_cast<TUFrom>(
                               static_cast<unsigned>(non_elided_one) - 1u))));
    const auto neg_inf = BitCast(
        d_from,
        Set(du_from, static_cast<TUFrom>(
                         kExpMask | static_cast<TUFrom>(LimitsMin<TIFrom>()) |
                         static_cast<TUFrom>(
                             static_cast<unsigned>(non_elided_one) - 1u))));
    const auto pos_nan = BitCast(
        d_from,
        Set(di_from, static_cast<TIFrom>(static_cast<TIFrom>(-non_elided_one) &
                                         LimitsMax<TIFrom>())));
    const auto neg_nan =
        BitCast(d_from, Set(di_from, static_cast<TIFrom>(-non_elided_one)));

    RandomState rng;
    for (size_t rep = 0; rep < AdjustedReps(200); ++rep) {
      for (size_t i = 0; i < N; i++) {
        uint64_t rand_bits = rng();

        const TUFrom exp_bits =
            static_cast<TUFrom>(((rand_bits >> kNumOfMantBits) %
                                 static_cast<uint64_t>(kMinOutOfRangeBiasedExp))
                                << kNumOfMantBits);
        const TFrom rand_in_range_val = BitCastScalar<TFrom>(
            static_cast<TUFrom>((rand_bits & kMantAndSignMask) | exp_bits));

        HWY_ASSERT(ScalarIsFinite(rand_in_range_val));
        HWY_ASSERT(rand_in_range_val >= kLowestInRangeFltVal);
        HWY_ASSERT(rand_in_range_val <= kHighestInRangeFltVal);

        from_lanes[i] = rand_in_range_val;
        expected[i] = ConvertScalarTo<TTo>(rand_in_range_val);
      }

      const auto from = Load(d_from, from_lanes.get());

      HWY_ASSERT_VEC_EQ(d_to, expected.get(), DoFastF2IConvVector(d_to, from));
      HWY_ASSERT_VEC_EQ(
          d_to, expected.get(),
          OddEven(DoFastF2IConvVector(d_to, OddEven(from, pos_nan)),
                  DoFastF2IConvVector(d_to, OddEven(pos_nan, from))));
      HWY_ASSERT_VEC_EQ(
          d_to, expected.get(),
          OddEven(DoFastF2IConvVector(d_to, OddEven(from, neg_nan)),
                  DoFastF2IConvVector(d_to, OddEven(neg_nan, from))));
      HWY_ASSERT_VEC_EQ(
          d_to, expected.get(),
          OddEven(DoFastF2IConvVector(d_to, OddEven(from, pos_inf)),
                  DoFastF2IConvVector(d_to, OddEven(pos_inf, from))));
      HWY_ASSERT_VEC_EQ(
          d_to, expected.get(),
          OddEven(DoFastF2IConvVector(d_to, OddEven(from, neg_inf)),
                  DoFastF2IConvVector(d_to, OddEven(neg_inf, from))));
    }

    HWY_BITCASTSCALAR_CXX14_CONSTEXPR const uint64_t
        kOutOfRangeRandBitsModulus =
            static_cast<uint64_t>(static_cast<TUFrom>(LimitsMax<TIFrom>()) -
                                  kMinOutOfRangeRandFltBits + 1);
    HWY_FAST_F2I_CONV_TEST_CONST_ASSERT(
        kOutOfRangeRandBitsModulus != 0,
        "kOutOfRangeRandBitsModulus != 0 must be true");

    for (size_t rep = 0; rep < AdjustedReps(200); ++rep) {
      ZeroBytes(expected.get(), sizeof(TTo) * N);

      for (size_t i = 0; i < N; i++) {
        uint64_t rand_bits = rng();

        const uint64_t rand_mag_bits = static_cast<uint64_t>(
            (rand_bits % kOutOfRangeRandBitsModulus) +
            static_cast<uint64_t>(kMinOutOfRangeRandFltBits));

        const TFrom rand_out_of_range_val =
            BitCastScalar<TFrom>(static_cast<TUFrom>(
                rand_mag_bits |
                (rand_bits & (1ULL << (sizeof(TFrom) * 8 - 1)))));

        HWY_ASSERT(!(ScalarIsFinite(rand_out_of_range_val) &&
                     rand_out_of_range_val >= kLowestInRangeFltVal &&
                     rand_out_of_range_val <= kHighestInRangeFltVal));

        from_lanes[i] = rand_out_of_range_val;
      }

      const auto from = Load(d_from, from_lanes.get());
      const auto actual = DoFastF2IConvVector(d_to, from);
      HWY_ASSERT_VEC_EQ(
          d_to, expected.get(),
          And(actual,
              Set(d_to, static_cast<TTo>(static_cast<unsigned>(non_elided_one) -
                                         1u))));

      for (size_t i = 0; i < N; i++) {
        expected[i] = static_cast<TTo>(-1);
      }

      HWY_ASSERT_VEC_EQ(
          d_to, expected.get(),
          Or(actual,
             Set(d_to,
                 static_cast<TTo>(TTo{0} - static_cast<TTo>(non_elided_one)))));
    }
  }
};

HWY_NOINLINE void TestAllFastConvertFloatToInt() {
#if HWY_HAVE_FLOAT16
  ForPartialVectors<TestFastConvertFloatToInt<int16_t>>()(hwy::float16_t());
  ForPartialVectors<TestFastConvertFloatToInt<uint16_t>>()(hwy::float16_t());
#endif

  ForPartialVectors<TestFastConvertFloatToInt<int32_t>>()(float());
  ForPartialVectors<TestFastConvertFloatToInt<uint32_t>>()(float());

#if HWY_HAVE_INTEGER64
  ForPromoteVectors<TestFastConvertFloatToInt<int64_t>>()(float());
  ForPromoteVectors<TestFastConvertFloatToInt<uint64_t>>()(float());
#endif

#if HWY_HAVE_FLOAT64
  ForDemoteVectors<TestFastConvertFloatToInt<int32_t>>()(double());
  ForDemoteVectors<TestFastConvertFloatToInt<uint32_t>>()(double());
  ForPartialVectors<TestFastConvertFloatToInt<int64_t>>()(double());
  ForPartialVectors<TestFastConvertFloatToInt<uint64_t>>()(double());
#endif
}

template <class TTo>
struct TestFastPromoteUpperLowerFloatToInt {
  template <typename TFrom, class DFrom>
  HWY_NOINLINE void operator()(TFrom /*unused*/, DFrom d_from) {
    static_assert(IsFloat<TFrom>(), "TFrom must be a floating-point type");
    static_assert(!IsFloat<TTo>() && !IsSpecialFloat<TTo>(),
                  "TTo must be an integer type");

    const size_t N = Lanes(d_from);
    HWY_ASSERT(N >= 2);

    auto from_lanes = AllocateAligned<TFrom>(N);
    auto expected = AllocateAligned<TTo>(N / 2);
    HWY_ASSERT(from_lanes && expected);

    constexpr uint64_t kIotaMask =
        static_cast<uint64_t>(MantissaMask<TFrom>()) &
        static_cast<uint64_t>(LimitsMax<TTo>() / 2);

    const Repartition<TTo, decltype(d_from)> d_to;

    for (size_t i = 0; i < N; ++i) {
      const uint64_t from_val = static_cast<uint64_t>((i & kIotaMask) + 1u);
      from_lanes[i] = ConvertScalarTo<TFrom>(from_val);
    }

    for (size_t i = 0; i < N / 2; ++i) {
      const uint64_t from_val = static_cast<uint64_t>((i & kIotaMask) + 1u);
      expected[i] = static_cast<TTo>(from_val);
    }

    const auto from = Load(d_from, from_lanes.get());
    HWY_ASSERT_VEC_EQ(d_to, expected.get(), FastPromoteLowerTo(d_to, from));

#if HWY_TARGET != HWY_SCALAR
    for (size_t i = 0; i < N / 2; ++i) {
      const uint64_t from_val =
          static_cast<uint64_t>(((i + (N / 2)) & kIotaMask) + 1u);
      expected[i] = static_cast<TTo>(from_val);
    }

    HWY_ASSERT_VEC_EQ(d_to, expected.get(), FastPromoteUpperTo(d_to, from));
#endif
  }
};

HWY_NOINLINE void TestAllFastPromoteUpperLowerFloatToInt() {
#if HWY_HAVE_INTEGER64
  ForShrinkableVectors<TestFastPromoteUpperLowerFloatToInt<int64_t>, 1>()(
      float());
  ForShrinkableVectors<TestFastPromoteUpperLowerFloatToInt<uint64_t>, 1>()(
      float());
#endif
}

template <class TTo>
struct TestFastPromoteOddEvenFloatToInt {
  template <typename TFrom, class DFrom>
  HWY_NOINLINE void operator()(TFrom /*unused*/, DFrom d_from) {
    static_assert(IsFloat<TFrom>(), "TFrom must be a floating-point type");
    static_assert(!IsFloat<TTo>() && !IsSpecialFloat<TTo>(),
                  "TTo must be an integer type");

    const size_t N = Lanes(d_from);
    HWY_ASSERT(N >= 2);

    auto from_lanes = AllocateAligned<TFrom>(N);
    auto expected = AllocateAligned<TTo>(N / 2);
    HWY_ASSERT(from_lanes && expected);

    constexpr uint64_t kIotaMask =
        static_cast<uint64_t>(MantissaMask<TFrom>()) &
        static_cast<uint64_t>(LimitsMax<TTo>() / 2);

    const Repartition<TTo, decltype(d_from)> d_to;

    for (size_t i = 0; i < N; ++i) {
      const uint64_t from_val = static_cast<uint64_t>((i & kIotaMask) + 1u);
      from_lanes[i] = ConvertScalarTo<TFrom>(from_val);
    }

    for (size_t i = 0; i < N / 2; ++i) {
      const uint64_t from_val =
          static_cast<uint64_t>(((2 * i) & kIotaMask) + 1u);
      expected[i] = static_cast<TTo>(from_val);
    }

    const auto from = Load(d_from, from_lanes.get());
    HWY_ASSERT_VEC_EQ(d_to, expected.get(), FastPromoteEvenTo(d_to, from));

#if HWY_TARGET != HWY_SCALAR
    for (size_t i = 0; i < N / 2; ++i) {
      const uint64_t from_val =
          static_cast<uint64_t>(((2 * i + 1) & kIotaMask) + 1u);
      expected[i] = static_cast<TTo>(from_val);
    }

    HWY_ASSERT_VEC_EQ(d_to, expected.get(), FastPromoteOddTo(d_to, from));
#endif
  }
};

HWY_NOINLINE void TestAllFastPromoteOddEvenFloatToInt() {
#if HWY_HAVE_INTEGER64
  ForShrinkableVectors<TestFastPromoteOddEvenFloatToInt<int64_t>, 1>()(float());
  ForShrinkableVectors<TestFastPromoteOddEvenFloatToInt<uint64_t>, 1>()(
      float());
#endif
}

#undef HWY_FAST_F2I_CONV_TEST_CONST_ASSERT

// NOLINTNEXTLINE(google-readability-namespace-comments)
}  // namespace HWY_NAMESPACE
}  // namespace hwy
HWY_AFTER_NAMESPACE();

#if HWY_ONCE

namespace hwy {
HWY_BEFORE_TEST(HwyFastFloatToIntConvTest);
HWY_EXPORT_AND_TEST_P(HwyFastFloatToIntConvTest, TestAllFastConvertFloatToInt);
HWY_EXPORT_AND_TEST_P(HwyFastFloatToIntConvTest,
                      TestAllFastPromoteUpperLowerFloatToInt);
HWY_EXPORT_AND_TEST_P(HwyFastFloatToIntConvTest,
                      TestAllFastPromoteOddEvenFloatToInt);
HWY_AFTER_TEST();
}  // namespace hwy

#endif
