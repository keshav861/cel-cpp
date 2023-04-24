// Copyright 2021 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "eval/public/builtin_func_registrar.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "absl/types/optional.h"
#include "base/function_adapter.h"
#include "base/handle.h"
#include "base/type_factory.h"
#include "base/value.h"
#include "base/value_factory.h"
#include "base/values/bytes_value.h"
#include "base/values/list_value.h"
#include "base/values/map_value.h"
#include "base/values/string_value.h"
#include "eval/eval/mutable_list_impl.h"
#include "eval/internal/interop.h"
#include "eval/public/cel_builtins.h"
#include "eval/public/cel_function_registry.h"
#include "eval/public/cel_number.h"
#include "eval/public/cel_options.h"
#include "eval/public/cel_value.h"
#include "eval/public/comparison_functions.h"
#include "eval/public/containers/container_backed_list_impl.h"
#include "eval/public/equality_function_registrar.h"
#include "eval/public/logical_function_registrar.h"
#include "eval/public/portable_cel_function_adapter.h"
#include "extensions/protobuf/memory_manager.h"
#include "internal/casts.h"
#include "internal/overflow.h"
#include "internal/proto_time_encoding.h"
#include "internal/status_macros.h"
#include "internal/time.h"
#include "internal/utf8.h"
#include "re2/re2.h"

namespace google::api::expr::runtime {

namespace {

using ::cel::BinaryFunctionAdapter;
using ::cel::BytesValue;
using ::cel::Handle;
using ::cel::ListValue;
using ::cel::MapValue;
using ::cel::StringValue;
using ::cel::UnaryFunctionAdapter;
using ::cel::Value;
using ::cel::ValueFactory;
using ::cel::internal::EncodeDurationToString;
using ::cel::internal::EncodeTimeToString;
using ::cel::internal::MaxTimestamp;
using ::google::protobuf::Arena;

// Time representing `9999-12-31T23:59:59.999999999Z`.
const absl::Time kMaxTime = MaxTimestamp();

// Template functions providing arithmetic operations
template <class Type>
Handle<Value> Add(ValueFactory&, Type v0, Type v1);

template <>
Handle<Value> Add<int64_t>(ValueFactory& value_factory, int64_t v0,
                           int64_t v1) {
  auto sum = cel::internal::CheckedAdd(v0, v1);
  if (!sum.ok()) {
    return value_factory.CreateErrorValue(sum.status());
  }
  return value_factory.CreateIntValue(*sum);
}

template <>
Handle<Value> Add<uint64_t>(ValueFactory& value_factory, uint64_t v0,
                            uint64_t v1) {
  auto sum = cel::internal::CheckedAdd(v0, v1);
  if (!sum.ok()) {
    return value_factory.CreateErrorValue(sum.status());
  }
  return value_factory.CreateUintValue(*sum);
}

template <>
Handle<Value> Add<double>(ValueFactory& value_factory, double v0, double v1) {
  return value_factory.CreateDoubleValue(v0 + v1);
}

template <class Type>
Handle<Value> Sub(ValueFactory&, Type v0, Type v1);

template <>
Handle<Value> Sub<int64_t>(ValueFactory& value_factory, int64_t v0,
                           int64_t v1) {
  auto diff = cel::internal::CheckedSub(v0, v1);
  if (!diff.ok()) {
    return value_factory.CreateErrorValue(diff.status());
  }
  return value_factory.CreateIntValue(*diff);
}

template <>
Handle<Value> Sub<uint64_t>(ValueFactory& value_factory, uint64_t v0,
                            uint64_t v1) {
  auto diff = cel::internal::CheckedSub(v0, v1);
  if (!diff.ok()) {
    return value_factory.CreateErrorValue(diff.status());
  }
  return value_factory.CreateUintValue(*diff);
}

template <>
Handle<Value> Sub<double>(ValueFactory& value_factory, double v0, double v1) {
  return value_factory.CreateDoubleValue(v0 - v1);
}

template <class Type>
Handle<Value> Mul(ValueFactory&, Type v0, Type v1);

template <>
Handle<Value> Mul<int64_t>(ValueFactory& value_factory, int64_t v0,
                           int64_t v1) {
  auto prod = cel::internal::CheckedMul(v0, v1);
  if (!prod.ok()) {
    return value_factory.CreateErrorValue(prod.status());
  }
  return value_factory.CreateIntValue(*prod);
}

template <>
Handle<Value> Mul<uint64_t>(ValueFactory& value_factory, uint64_t v0,
                            uint64_t v1) {
  auto prod = cel::internal::CheckedMul(v0, v1);
  if (!prod.ok()) {
    return value_factory.CreateErrorValue(prod.status());
  }
  return value_factory.CreateUintValue(*prod);
}

template <>
Handle<Value> Mul<double>(ValueFactory& value_factory, double v0, double v1) {
  return value_factory.CreateDoubleValue(v0 * v1);
}

template <class Type>
Handle<Value> Div(ValueFactory&, Type v0, Type v1);

// Division operations for integer types should check for
// division by 0
template <>
Handle<Value> Div<int64_t>(ValueFactory& value_factory, int64_t v0,
                           int64_t v1) {
  auto quot = cel::internal::CheckedDiv(v0, v1);
  if (!quot.ok()) {
    return value_factory.CreateErrorValue(quot.status());
  }
  return value_factory.CreateIntValue(*quot);
}

// Division operations for integer types should check for
// division by 0
template <>
Handle<Value> Div<uint64_t>(ValueFactory& value_factory, uint64_t v0,
                            uint64_t v1) {
  auto quot = cel::internal::CheckedDiv(v0, v1);
  if (!quot.ok()) {
    return value_factory.CreateErrorValue(quot.status());
  }
  return value_factory.CreateUintValue(*quot);
}

template <>
Handle<Value> Div<double>(ValueFactory& value_factory, double v0, double v1) {
  static_assert(std::numeric_limits<double>::is_iec559,
                "Division by zero for doubles must be supported");

  // For double, division will result in +/- inf
  return value_factory.CreateDoubleValue(v0 / v1);
}

// Modulo operation
template <class Type>
Handle<Value> Modulo(ValueFactory& value_factory, Type v0, Type v1);

// Modulo operations for integer types should check for
// division by 0
template <>
Handle<Value> Modulo<int64_t>(ValueFactory& value_factory, int64_t v0,
                              int64_t v1) {
  auto mod = cel::internal::CheckedMod(v0, v1);
  if (!mod.ok()) {
    return value_factory.CreateErrorValue(mod.status());
  }
  return value_factory.CreateIntValue(*mod);
}

template <>
Handle<Value> Modulo<uint64_t>(ValueFactory& value_factory, uint64_t v0,
                               uint64_t v1) {
  auto mod = cel::internal::CheckedMod(v0, v1);
  if (!mod.ok()) {
    return value_factory.CreateErrorValue(mod.status());
  }
  return value_factory.CreateUintValue(*mod);
}

// Helper method
// Registers all arithmetic functions for template parameter type.
template <class Type>
absl::Status RegisterArithmeticFunctionsForType(CelFunctionRegistry* registry) {
  using FunctionAdapter = cel::BinaryFunctionAdapter<Handle<Value>, Type, Type>;
  CEL_RETURN_IF_ERROR(registry->Register(
      FunctionAdapter::CreateDescriptor(builtin::kAdd, false),
      FunctionAdapter::WrapFunction(&Add<Type>)));

  CEL_RETURN_IF_ERROR(registry->Register(
      FunctionAdapter::CreateDescriptor(builtin::kSubtract, false),
      FunctionAdapter::WrapFunction(&Sub<Type>)));

  CEL_RETURN_IF_ERROR(registry->Register(
      FunctionAdapter::CreateDescriptor(builtin::kMultiply, false),
      FunctionAdapter::WrapFunction(&Mul<Type>)));

  return registry->Register(
      FunctionAdapter::CreateDescriptor(builtin::kDivide, false),
      FunctionAdapter::WrapFunction(&Div<Type>));
}

// Register basic Arithmetic functions for numeric types.
absl::Status RegisterNumericArithmeticFunctions(
    CelFunctionRegistry* registry, const InterpreterOptions& options) {
  CEL_RETURN_IF_ERROR(RegisterArithmeticFunctionsForType<int64_t>(registry));
  CEL_RETURN_IF_ERROR(RegisterArithmeticFunctionsForType<uint64_t>(registry));
  CEL_RETURN_IF_ERROR(RegisterArithmeticFunctionsForType<double>(registry));

  // Modulo
  CEL_RETURN_IF_ERROR(registry->Register(
      BinaryFunctionAdapter<Handle<Value>, int64_t, int64_t>::CreateDescriptor(
          builtin::kModulo, false),
      BinaryFunctionAdapter<Handle<Value>, int64_t, int64_t>::WrapFunction(
          &Modulo<int64_t>)));

  CEL_RETURN_IF_ERROR(registry->Register(
      BinaryFunctionAdapter<Handle<Value>, uint64_t,
                            uint64_t>::CreateDescriptor(builtin::kModulo,
                                                        false),
      BinaryFunctionAdapter<Handle<Value>, uint64_t, uint64_t>::WrapFunction(
          &Modulo<uint64_t>)));

  // Negation group
  CEL_RETURN_IF_ERROR(registry->Register(
      UnaryFunctionAdapter<Handle<Value>, int64_t>::CreateDescriptor(
          builtin::kNeg, false),
      UnaryFunctionAdapter<Handle<Value>, int64_t>::WrapFunction(
          [](ValueFactory& value_factory, int64_t value) -> Handle<Value> {
            auto inv = cel::internal::CheckedNegation(value);
            if (!inv.ok()) {
              return value_factory.CreateErrorValue(inv.status());
            }
            return value_factory.CreateIntValue(*inv);
          })));

  return registry->Register(
      UnaryFunctionAdapter<double, double>::CreateDescriptor(builtin::kNeg,
                                                             false),
      UnaryFunctionAdapter<double, double>::WrapFunction(
          [](ValueFactory&, double value) -> double { return -value; }));
}

template <class T>
bool ValueEquals(const CelValue& value, T other);

template <>
bool ValueEquals(const CelValue& value, bool other) {
  return value.IsBool() && (value.BoolOrDie() == other);
}

template <>
bool ValueEquals(const CelValue& value, int64_t other) {
  return value.IsInt64() && (value.Int64OrDie() == other);
}

template <>
bool ValueEquals(const CelValue& value, uint64_t other) {
  return value.IsUint64() && (value.Uint64OrDie() == other);
}

template <>
bool ValueEquals(const CelValue& value, double other) {
  return value.IsDouble() && (value.DoubleOrDie() == other);
}

template <>
bool ValueEquals(const CelValue& value, CelValue::StringHolder other) {
  return value.IsString() && (value.StringOrDie() == other);
}

template <>
bool ValueEquals(const CelValue& value, CelValue::BytesHolder other) {
  return value.IsBytes() && (value.BytesOrDie() == other);
}

// Template function implementing CEL in() function
template <typename T>
bool In(Arena* arena, T value, const CelList* list) {
  int index_size = list->size();

  for (int i = 0; i < index_size; i++) {
    CelValue element = (*list).Get(arena, i);

    if (ValueEquals<T>(element, value)) {
      return true;
    }
  }

  return false;
}

// Implementation for @in operator using heterogeneous equality.
CelValue HeterogeneousEqualityIn(Arena* arena, CelValue value,
                                 const CelList* list) {
  int index_size = list->size();

  for (int i = 0; i < index_size; i++) {
    CelValue element = (*list).Get(arena, i);
    absl::optional<bool> element_equals = CelValueEqualImpl(element, value);

    // If equality is undefined (e.g. duration == double), just treat as false.
    if (element_equals.has_value() && *element_equals) {
      return CelValue::CreateBool(true);
    }
  }

  return CelValue::CreateBool(false);
}

// AppendList will append the elements in value2 to value1.
//
// This call will only be invoked within comprehensions where `value1` is an
// intermediate result which cannot be directly assigned or co-mingled with a
// user-provided list.
const CelList* AppendList(Arena* arena, const CelList* value1,
                          const CelList* value2) {
  // The `value1` object cannot be directly addressed and is an intermediate
  // variable. Once the comprehension completes this value will in effect be
  // treated as immutable.
  MutableListImpl* mutable_list = const_cast<MutableListImpl*>(
      cel::internal::down_cast<const MutableListImpl*>(value1));
  for (int i = 0; i < value2->size(); i++) {
    mutable_list->Append((*value2).Get(arena, i));
  }
  return mutable_list;
}

// Concatenation for string type.
absl::StatusOr<Handle<StringValue>> ConcatString(ValueFactory& factory,
                                                 const StringValue& value1,
                                                 const StringValue& value2) {
  return factory.CreateUncheckedStringValue(
      absl::StrCat(value1.ToString(), value2.ToString()));
}

// Concatenation for bytes type.
absl::StatusOr<Handle<BytesValue>> ConcatBytes(ValueFactory& factory,
                                               const BytesValue& value1,
                                               const BytesValue& value2) {
  return factory.CreateBytesValue(
      absl::StrCat(value1.ToString(), value2.ToString()));
}

// Concatenation for CelList type.
absl::StatusOr<Handle<ListValue>> ConcatList(ValueFactory& factory,
                                             const ListValue& value1,
                                             const ListValue& value2) {
  std::vector<CelValue> joined_values;

  int size1 = value1.size();
  int size2 = value2.size();
  joined_values.reserve(size1 + size2);

  Arena* arena = cel::extensions::ProtoMemoryManager::CastToProtoArena(
      factory.memory_manager());

  ListValue::GetContext context(factory);
  for (int i = 0; i < size1; i++) {
    CEL_ASSIGN_OR_RETURN(Handle<Value> elem, value1.Get(context, i));
    joined_values.push_back(
        cel::interop_internal::ModernValueToLegacyValueOrDie(arena, elem));
  }
  for (int i = 0; i < size2; i++) {
    CEL_ASSIGN_OR_RETURN(Handle<Value> elem, value2.Get(context, i));
    joined_values.push_back(
        cel::interop_internal::ModernValueToLegacyValueOrDie(arena, elem));
  }

  auto concatenated =
      Arena::Create<ContainerBackedListImpl>(arena, joined_values);

  return cel::interop_internal::CreateLegacyListValue(concatenated);
}

// Timestamp
absl::Status FindTimeBreakdown(absl::Time timestamp, absl::string_view tz,
                               absl::TimeZone::CivilInfo* breakdown) {
  absl::TimeZone time_zone;

  // Early return if there is no timezone.
  if (tz.empty()) {
    *breakdown = time_zone.At(timestamp);
    return absl::OkStatus();
  }

  // Check to see whether the timezone is an IANA timezone.
  if (absl::LoadTimeZone(tz, &time_zone)) {
    *breakdown = time_zone.At(timestamp);
    return absl::OkStatus();
  }

  // Check for times of the format: [+-]HH:MM and convert them into durations
  // specified as [+-]HHhMMm.
  if (absl::StrContains(tz, ":")) {
    std::string dur = absl::StrCat(tz, "m");
    absl::StrReplaceAll({{":", "h"}}, &dur);
    absl::Duration d;
    if (absl::ParseDuration(dur, &d)) {
      timestamp += d;
      *breakdown = time_zone.At(timestamp);
      return absl::OkStatus();
    }
  }

  // Otherwise, error.
  return absl::InvalidArgumentError("Invalid timezone");
}

Handle<Value> GetTimeBreakdownPart(
    ValueFactory& value_factory, absl::Time timestamp, absl::string_view tz,
    const std::function<int64_t(const absl::TimeZone::CivilInfo&)>&
        extractor_func) {
  absl::TimeZone::CivilInfo breakdown;
  auto status = FindTimeBreakdown(timestamp, tz, &breakdown);

  if (!status.ok()) {
    return value_factory.CreateErrorValue(status);
  }

  return value_factory.CreateIntValue(extractor_func(breakdown));
}

Handle<Value> GetFullYear(ValueFactory& value_factory, absl::Time timestamp,
                          absl::string_view tz) {
  return GetTimeBreakdownPart(value_factory, timestamp, tz,
                              [](const absl::TimeZone::CivilInfo& breakdown) {
                                return breakdown.cs.year();
                              });
}

Handle<Value> GetMonth(ValueFactory& value_factory, absl::Time timestamp,
                       absl::string_view tz) {
  return GetTimeBreakdownPart(value_factory, timestamp, tz,
                              [](const absl::TimeZone::CivilInfo& breakdown) {
                                return breakdown.cs.month() - 1;
                              });
}

Handle<Value> GetDayOfYear(ValueFactory& value_factory, absl::Time timestamp,
                           absl::string_view tz) {
  return GetTimeBreakdownPart(
      value_factory, timestamp, tz,
      [](const absl::TimeZone::CivilInfo& breakdown) {
        return absl::GetYearDay(absl::CivilDay(breakdown.cs)) - 1;
      });
}

Handle<Value> GetDayOfMonth(ValueFactory& value_factory, absl::Time timestamp,
                            absl::string_view tz) {
  return GetTimeBreakdownPart(value_factory, timestamp, tz,
                              [](const absl::TimeZone::CivilInfo& breakdown) {
                                return breakdown.cs.day() - 1;
                              });
}

Handle<Value> GetDate(ValueFactory& value_factory, absl::Time timestamp,
                      absl::string_view tz) {
  return GetTimeBreakdownPart(value_factory, timestamp, tz,
                              [](const absl::TimeZone::CivilInfo& breakdown) {
                                return breakdown.cs.day();
                              });
}

Handle<Value> GetDayOfWeek(ValueFactory& value_factory, absl::Time timestamp,
                           absl::string_view tz) {
  return GetTimeBreakdownPart(
      value_factory, timestamp, tz,
      [](const absl::TimeZone::CivilInfo& breakdown) {
        absl::Weekday weekday = absl::GetWeekday(breakdown.cs);

        // get day of week from the date in UTC, zero-based, zero for Sunday,
        // based on GetDayOfWeek CEL function definition.
        int weekday_num = static_cast<int>(weekday);
        weekday_num = (weekday_num == 6) ? 0 : weekday_num + 1;
        return weekday_num;
      });
}

Handle<Value> GetHours(ValueFactory& value_factory, absl::Time timestamp,
                       absl::string_view tz) {
  return GetTimeBreakdownPart(value_factory, timestamp, tz,
                              [](const absl::TimeZone::CivilInfo& breakdown) {
                                return breakdown.cs.hour();
                              });
}

Handle<Value> GetMinutes(ValueFactory& value_factory, absl::Time timestamp,
                         absl::string_view tz) {
  return GetTimeBreakdownPart(value_factory, timestamp, tz,
                              [](const absl::TimeZone::CivilInfo& breakdown) {
                                return breakdown.cs.minute();
                              });
}

Handle<Value> GetSeconds(ValueFactory& value_factory, absl::Time timestamp,
                         absl::string_view tz) {
  return GetTimeBreakdownPart(value_factory, timestamp, tz,
                              [](const absl::TimeZone::CivilInfo& breakdown) {
                                return breakdown.cs.second();
                              });
}

Handle<Value> GetMilliseconds(ValueFactory& value_factory, absl::Time timestamp,
                              absl::string_view tz) {
  return GetTimeBreakdownPart(
      value_factory, timestamp, tz,
      [](const absl::TimeZone::CivilInfo& breakdown) {
        return absl::ToInt64Milliseconds(breakdown.subsecond);
      });
}

Handle<Value> CreateDurationFromString(ValueFactory& value_factory,
                                       const StringValue& dur_str) {
  absl::Duration d;
  if (!absl::ParseDuration(dur_str.ToString(), &d)) {
    return value_factory.CreateErrorValue(
        absl::InvalidArgumentError("String to Duration conversion failed"));
  }

  auto duration = value_factory.CreateDurationValue(d);

  if (!duration.ok()) {
    return value_factory.CreateErrorValue(duration.status());
  }

  return *duration;
}

bool StringContains(ValueFactory&, const StringValue& value,
                    const StringValue& substr) {
  return absl::StrContains(value.ToString(), substr.ToString());
}

bool StringEndsWith(ValueFactory&, const StringValue& value,
                    const StringValue& suffix) {
  return absl::EndsWith(value.ToString(), suffix.ToString());
}

bool StringStartsWith(ValueFactory&, const StringValue& value,
                      const StringValue& prefix) {
  return absl::StartsWith(value.ToString(), prefix.ToString());
}

absl::Status RegisterSetMembershipFunctions(CelFunctionRegistry* registry,
                                            const InterpreterOptions& options) {
  constexpr std::array<absl::string_view, 3> in_operators = {
      builtin::kIn,            // @in for map and list types.
      builtin::kInFunction,    // deprecated in() -- for backwards compat
      builtin::kInDeprecated,  // deprecated _in_ -- for backwards compat
  };

  if (options.enable_list_contains) {
    for (absl::string_view op : in_operators) {
      if (options.enable_heterogeneous_equality) {
        CEL_RETURN_IF_ERROR(registry->Register(
            (PortableBinaryFunctionAdapter<CelValue, CelValue, const CelList*>::
                 Create(op, false, &HeterogeneousEqualityIn))));
      } else {
        CEL_RETURN_IF_ERROR(registry->Register(
            (PortableBinaryFunctionAdapter<bool, bool, const CelList*>::Create(
                op, false, In<bool>))));
        CEL_RETURN_IF_ERROR(registry->Register(
            (PortableBinaryFunctionAdapter<
                bool, int64_t, const CelList*>::Create(op, false,
                                                       In<int64_t>))));
        CEL_RETURN_IF_ERROR(registry->Register(
            PortableBinaryFunctionAdapter<
                bool, uint64_t, const CelList*>::Create(op, false,
                                                        In<uint64_t>)));
        CEL_RETURN_IF_ERROR(registry->Register(
            PortableBinaryFunctionAdapter<bool, double, const CelList*>::Create(
                op, false, In<double>)));
        CEL_RETURN_IF_ERROR(registry->Register(
            PortableBinaryFunctionAdapter<
                bool, CelValue::StringHolder,
                const CelList*>::Create(op, false,
                                        In<CelValue::StringHolder>)));
        CEL_RETURN_IF_ERROR(registry->Register(
            PortableBinaryFunctionAdapter<
                bool, CelValue::BytesHolder,
                const CelList*>::Create(op, false, In<CelValue::BytesHolder>)));
      }
    }
  }

  auto boolKeyInSet = [options](Arena* arena, bool key,
                                const CelMap* cel_map) -> CelValue {
    const auto& result = cel_map->Has(CelValue::CreateBool(key));
    if (result.ok()) {
      return CelValue::CreateBool(*result);
    }
    if (options.enable_heterogeneous_equality) {
      return CelValue::CreateBool(false);
    }
    return CreateErrorValue(arena, result.status());
  };

  auto intKeyInSet = [options](Arena* arena, int64_t key,
                               const CelMap* cel_map) -> CelValue {
    CelValue int_key = CelValue::CreateInt64(key);
    const auto& result = cel_map->Has(int_key);
    if (options.enable_heterogeneous_equality) {
      if (result.ok() && *result) {
        return CelValue::CreateBool(*result);
      }
      absl::optional<CelNumber> number = GetNumberFromCelValue(int_key);
      if (number->LosslessConvertibleToUint()) {
        const auto& result =
            cel_map->Has(CelValue::CreateUint64(number->AsUint()));
        if (result.ok() && *result) {
          return CelValue::CreateBool(*result);
        }
      }
      return CelValue::CreateBool(false);
    }
    if (!result.ok()) {
      return CreateErrorValue(arena, result.status());
    }
    return CelValue::CreateBool(*result);
  };

  auto stringKeyInSet = [options](Arena* arena, CelValue::StringHolder key,
                                  const CelMap* cel_map) -> CelValue {
    const auto& result = cel_map->Has(CelValue::CreateString(key));
    if (result.ok()) {
      return CelValue::CreateBool(*result);
    }
    if (options.enable_heterogeneous_equality) {
      return CelValue::CreateBool(false);
    }
    return CreateErrorValue(arena, result.status());
  };

  auto uintKeyInSet = [options](Arena* arena, uint64_t key,
                                const CelMap* cel_map) -> CelValue {
    CelValue uint_key = CelValue::CreateUint64(key);
    const auto& result = cel_map->Has(uint_key);
    if (options.enable_heterogeneous_equality) {
      if (result.ok() && *result) {
        return CelValue::CreateBool(*result);
      }
      absl::optional<CelNumber> number = GetNumberFromCelValue(uint_key);
      if (number->LosslessConvertibleToInt()) {
        const auto& result =
            cel_map->Has(CelValue::CreateInt64(number->AsInt()));
        if (result.ok() && *result) {
          return CelValue::CreateBool(*result);
        }
      }
      return CelValue::CreateBool(false);
    }
    if (!result.ok()) {
      return CreateErrorValue(arena, result.status());
    }
    return CelValue::CreateBool(*result);
  };

  auto doubleKeyInSet = [](Arena* arena, double key,
                           const CelMap* cel_map) -> CelValue {
    absl::optional<CelNumber> number =
        GetNumberFromCelValue(CelValue::CreateDouble(key));
    if (number->LosslessConvertibleToInt()) {
      const auto& result = cel_map->Has(CelValue::CreateInt64(number->AsInt()));
      if (result.ok() && *result) {
        return CelValue::CreateBool(*result);
      }
    }
    if (number->LosslessConvertibleToUint()) {
      const auto& result =
          cel_map->Has(CelValue::CreateUint64(number->AsUint()));
      if (result.ok() && *result) {
        return CelValue::CreateBool(*result);
      }
    }
    return CelValue::CreateBool(false);
  };

  for (auto op : in_operators) {
    auto status = registry->Register(
        PortableBinaryFunctionAdapter<CelValue, CelValue::StringHolder,
                                      const CelMap*>::Create(op, false,
                                                             stringKeyInSet));
    if (!status.ok()) return status;

    status = registry->Register(
        PortableBinaryFunctionAdapter<CelValue, bool, const CelMap*>::Create(
            op, false, boolKeyInSet));
    if (!status.ok()) return status;

    status = registry->Register(
        PortableBinaryFunctionAdapter<CelValue, int64_t, const CelMap*>::Create(
            op, false, intKeyInSet));
    if (!status.ok()) return status;

    status = registry->Register(
        PortableBinaryFunctionAdapter<CelValue, uint64_t,
                                      const CelMap*>::Create(op, false,
                                                             uintKeyInSet));
    if (!status.ok()) return status;

    if (options.enable_heterogeneous_equality) {
      status = registry->Register(
          PortableBinaryFunctionAdapter<CelValue, double,
                                        const CelMap*>::Create(op, false,
                                                               doubleKeyInSet));
      if (!status.ok()) return status;
    }
  }
  return absl::OkStatus();
}

// TODO(issues/5): after refactors for the new value type are done, move this
// to a separate build target to enable subset environments to not depend on
// RE2.
absl::Status RegisterRegexFunctions(CelFunctionRegistry* registry,
                                    const InterpreterOptions& options) {
  if (options.enable_regex) {
    auto regex_matches = [max_size = options.regex_max_program_size](
                             ValueFactory& value_factory,
                             const StringValue& target,
                             const StringValue& regex) -> Handle<Value> {
      RE2 re2(regex.ToString());
      if (max_size > 0 && re2.ProgramSize() > max_size) {
        return value_factory.CreateErrorValue(
            absl::InvalidArgumentError("exceeded RE2 max program size"));
      }
      if (!re2.ok()) {
        return value_factory.CreateErrorValue(
            absl::InvalidArgumentError("invalid regex for match"));
      }
      return value_factory.CreateBoolValue(
          RE2::PartialMatch(target.ToString(), re2));
    };

    // bind str.matches(re) and matches(str, re)
    for (bool receiver_style : {true, false}) {
      using MatchFnAdapter =
          BinaryFunctionAdapter<Handle<Value>, const StringValue&,
                                const StringValue&>;
      CEL_RETURN_IF_ERROR(
          registry->Register(MatchFnAdapter::CreateDescriptor(
                                 builtin::kRegexMatch, receiver_style),
                             MatchFnAdapter::WrapFunction(regex_matches)));
    }
  }  // if options.enable_regex

  return absl::OkStatus();
}

absl::Status RegisterStringFunctions(CelFunctionRegistry* registry,
                                     const InterpreterOptions& options) {
  // Basic substring tests (contains, startsWith, endsWith)
  for (bool receiver_style : {true, false}) {
    CEL_RETURN_IF_ERROR(registry->Register(
        BinaryFunctionAdapter<bool, const StringValue&, const StringValue&>::
            CreateDescriptor(builtin::kStringContains, receiver_style),
        BinaryFunctionAdapter<bool, const StringValue&, const StringValue&>::
            WrapFunction(StringContains)));

    CEL_RETURN_IF_ERROR(registry->Register(
        BinaryFunctionAdapter<bool, const StringValue&, const StringValue&>::
            CreateDescriptor(builtin::kStringEndsWith, receiver_style),
        BinaryFunctionAdapter<bool, const StringValue&, const StringValue&>::
            WrapFunction(StringEndsWith)));

    CEL_RETURN_IF_ERROR(registry->Register(
        BinaryFunctionAdapter<bool, const StringValue&, const StringValue&>::
            CreateDescriptor(builtin::kStringStartsWith, receiver_style),
        BinaryFunctionAdapter<bool, const StringValue&, const StringValue&>::
            WrapFunction(StringStartsWith)));
  }

  // string concatenation if enabled
  if (options.enable_string_concat) {
    using StrCatFnAdapter =
        BinaryFunctionAdapter<absl::StatusOr<Handle<StringValue>>,
                              const StringValue&, const StringValue&>;
    CEL_RETURN_IF_ERROR(registry->Register(
        StrCatFnAdapter::CreateDescriptor(builtin::kAdd, false),
        StrCatFnAdapter::WrapFunction(&ConcatString)));

    using BytesCatFnAdapter =
        BinaryFunctionAdapter<absl::StatusOr<Handle<BytesValue>>,
                              const BytesValue&, const BytesValue&>;
    CEL_RETURN_IF_ERROR(registry->Register(
        BytesCatFnAdapter::CreateDescriptor(builtin::kAdd, false),
        BytesCatFnAdapter::WrapFunction(&ConcatBytes)));
  }

  // String size
  auto size_func = [](ValueFactory& value_factory,
                      const StringValue& value) -> Handle<Value> {
    auto [count, valid] = ::cel::internal::Utf8Validate(value.ToString());
    if (!valid) {
      return value_factory.CreateErrorValue(
          absl::InvalidArgumentError("invalid utf-8 string"));
    }
    return value_factory.CreateIntValue(count);
  };

  // receiver style = true/false
  // Support global and receiver style size() operations on strings.
  using StrSizeFnAdapter =
      UnaryFunctionAdapter<Handle<Value>, const StringValue&>;
  CEL_RETURN_IF_ERROR(
      registry->Register(StrSizeFnAdapter::CreateDescriptor(
                             builtin::kSize, /*receiver_style=*/true),
                         StrSizeFnAdapter::WrapFunction(size_func)));
  CEL_RETURN_IF_ERROR(
      registry->Register(StrSizeFnAdapter::CreateDescriptor(
                             builtin::kSize, /*receiver_style=*/false),
                         StrSizeFnAdapter::WrapFunction(size_func)));

  // Bytes size
  auto bytes_size_func = [](ValueFactory&, const BytesValue& value) -> int64_t {
    return value.size();
  };
  // receiver style = true/false
  // Support global and receiver style size() operations on bytes.
  using BytesSizeFnAdapter = UnaryFunctionAdapter<int64_t, const BytesValue&>;
  CEL_RETURN_IF_ERROR(
      registry->Register(BytesSizeFnAdapter::CreateDescriptor(
                             builtin::kSize, /*receiver_style=*/true),
                         BytesSizeFnAdapter::WrapFunction(bytes_size_func)));
  CEL_RETURN_IF_ERROR(
      registry->Register(BytesSizeFnAdapter::CreateDescriptor(
                             builtin::kSize, /*receiver_style=*/false),
                         BytesSizeFnAdapter::WrapFunction(bytes_size_func)));

  return absl::OkStatus();
}

absl::Status RegisterTimestampFunctions(CelFunctionRegistry* registry,
                                        const InterpreterOptions& options) {
  CEL_RETURN_IF_ERROR(registry->Register(
      BinaryFunctionAdapter<Handle<Value>, absl::Time, const StringValue&>::
          CreateDescriptor(builtin::kFullYear, true),
      BinaryFunctionAdapter<Handle<Value>, absl::Time, const StringValue&>::
          WrapFunction([](ValueFactory& value_factory, absl::Time ts,
                          const StringValue& tz) -> Handle<Value> {
            return GetFullYear(value_factory, ts, tz.ToString());
          })));

  CEL_RETURN_IF_ERROR(registry->Register(
      UnaryFunctionAdapter<Handle<Value>, absl::Time>::CreateDescriptor(
          builtin::kFullYear, true),
      UnaryFunctionAdapter<Handle<Value>, absl::Time>::WrapFunction(
          [](ValueFactory& value_factory, absl::Time ts) -> Handle<Value> {
            return GetFullYear(value_factory, ts, "");
          })));

  CEL_RETURN_IF_ERROR(registry->Register(
      BinaryFunctionAdapter<Handle<Value>, absl::Time, const StringValue&>::
          CreateDescriptor(builtin::kMonth, true),
      BinaryFunctionAdapter<Handle<Value>, absl::Time, const StringValue&>::
          WrapFunction([](ValueFactory& value_factory, absl::Time ts,
                          const StringValue& tz) -> Handle<Value> {
            return GetMonth(value_factory, ts, tz.ToString());
          })));

  CEL_RETURN_IF_ERROR(registry->Register(
      UnaryFunctionAdapter<Handle<Value>, absl::Time>::CreateDescriptor(
          builtin::kMonth, true),
      UnaryFunctionAdapter<Handle<Value>, absl::Time>::WrapFunction(
          [](ValueFactory& value_factory, absl::Time ts) -> Handle<Value> {
            return GetMonth(value_factory, ts, "");
          })));

  CEL_RETURN_IF_ERROR(registry->Register(
      BinaryFunctionAdapter<Handle<Value>, absl::Time, const StringValue&>::
          CreateDescriptor(builtin::kDayOfYear, true),
      BinaryFunctionAdapter<Handle<Value>, absl::Time, const StringValue&>::
          WrapFunction([](ValueFactory& value_factory, absl::Time ts,
                          const StringValue& tz) -> Handle<Value> {
            return GetDayOfYear(value_factory, ts, tz.ToString());
          })));

  CEL_RETURN_IF_ERROR(registry->Register(
      UnaryFunctionAdapter<Handle<Value>, absl::Time>::CreateDescriptor(
          builtin::kDayOfYear, true),
      UnaryFunctionAdapter<Handle<Value>, absl::Time>::WrapFunction(
          [](ValueFactory& value_factory, absl::Time ts) -> Handle<Value> {
            return GetDayOfYear(value_factory, ts, "");
          })));

  CEL_RETURN_IF_ERROR(registry->Register(
      BinaryFunctionAdapter<Handle<Value>, absl::Time, const StringValue&>::
          CreateDescriptor(builtin::kDayOfMonth, true),
      BinaryFunctionAdapter<Handle<Value>, absl::Time, const StringValue&>::
          WrapFunction([](ValueFactory& value_factory, absl::Time ts,
                          const StringValue& tz) -> Handle<Value> {
            return GetDayOfMonth(value_factory, ts, tz.ToString());
          })));

  CEL_RETURN_IF_ERROR(registry->Register(
      UnaryFunctionAdapter<Handle<Value>, absl::Time>::CreateDescriptor(
          builtin::kDayOfMonth, true),
      UnaryFunctionAdapter<Handle<Value>, absl::Time>::WrapFunction(
          [](ValueFactory& value_factory, absl::Time ts) -> Handle<Value> {
            return GetDayOfMonth(value_factory, ts, "");
          })));

  CEL_RETURN_IF_ERROR(registry->Register(
      BinaryFunctionAdapter<Handle<Value>, absl::Time, const StringValue&>::
          CreateDescriptor(builtin::kDate, true),
      BinaryFunctionAdapter<Handle<Value>, absl::Time, const StringValue&>::
          WrapFunction([](ValueFactory& value_factory, absl::Time ts,
                          const StringValue& tz) -> Handle<Value> {
            return GetDate(value_factory, ts, tz.ToString());
          })));

  CEL_RETURN_IF_ERROR(registry->Register(
      UnaryFunctionAdapter<Handle<Value>, absl::Time>::CreateDescriptor(
          builtin::kDate, true),
      UnaryFunctionAdapter<Handle<Value>, absl::Time>::WrapFunction(
          [](ValueFactory& value_factory, absl::Time ts) -> Handle<Value> {
            return GetDate(value_factory, ts, "");
          })));

  CEL_RETURN_IF_ERROR(registry->Register(
      BinaryFunctionAdapter<Handle<Value>, absl::Time, const StringValue&>::
          CreateDescriptor(builtin::kDayOfWeek, true),
      BinaryFunctionAdapter<Handle<Value>, absl::Time, const StringValue&>::
          WrapFunction([](ValueFactory& value_factory, absl::Time ts,
                          const StringValue& tz) -> Handle<Value> {
            return GetDayOfWeek(value_factory, ts, tz.ToString());
          })));

  CEL_RETURN_IF_ERROR(registry->Register(
      UnaryFunctionAdapter<Handle<Value>, absl::Time>::CreateDescriptor(
          builtin::kDayOfWeek, true),
      UnaryFunctionAdapter<Handle<Value>, absl::Time>::WrapFunction(
          [](ValueFactory& value_factory, absl::Time ts) -> Handle<Value> {
            return GetDayOfWeek(value_factory, ts, "");
          })));

  CEL_RETURN_IF_ERROR(registry->Register(
      BinaryFunctionAdapter<Handle<Value>, absl::Time, const StringValue&>::
          CreateDescriptor(builtin::kHours, true),
      BinaryFunctionAdapter<Handle<Value>, absl::Time, const StringValue&>::
          WrapFunction([](ValueFactory& value_factory, absl::Time ts,
                          const StringValue& tz) -> Handle<Value> {
            return GetHours(value_factory, ts, tz.ToString());
          })));

  CEL_RETURN_IF_ERROR(registry->Register(
      UnaryFunctionAdapter<Handle<Value>, absl::Time>::CreateDescriptor(
          builtin::kHours, true),
      UnaryFunctionAdapter<Handle<Value>, absl::Time>::WrapFunction(
          [](ValueFactory& value_factory, absl::Time ts) -> Handle<Value> {
            return GetHours(value_factory, ts, "");
          })));

  CEL_RETURN_IF_ERROR(registry->Register(
      BinaryFunctionAdapter<Handle<Value>, absl::Time, const StringValue&>::
          CreateDescriptor(builtin::kMinutes, true),
      BinaryFunctionAdapter<Handle<Value>, absl::Time, const StringValue&>::
          WrapFunction([](ValueFactory& value_factory, absl::Time ts,
                          const StringValue& tz) -> Handle<Value> {
            return GetMinutes(value_factory, ts, tz.ToString());
          })));

  CEL_RETURN_IF_ERROR(registry->Register(
      UnaryFunctionAdapter<Handle<Value>, absl::Time>::CreateDescriptor(
          builtin::kMinutes, true),
      UnaryFunctionAdapter<Handle<Value>, absl::Time>::WrapFunction(
          [](ValueFactory& value_factory, absl::Time ts) -> Handle<Value> {
            return GetMinutes(value_factory, ts, "");
          })));

  CEL_RETURN_IF_ERROR(registry->Register(
      BinaryFunctionAdapter<Handle<Value>, absl::Time, const StringValue&>::
          CreateDescriptor(builtin::kSeconds, true),
      BinaryFunctionAdapter<Handle<Value>, absl::Time, const StringValue&>::
          WrapFunction([](ValueFactory& value_factory, absl::Time ts,
                          const StringValue& tz) -> Handle<Value> {
            return GetSeconds(value_factory, ts, tz.ToString());
          })));

  CEL_RETURN_IF_ERROR(registry->Register(
      UnaryFunctionAdapter<Handle<Value>, absl::Time>::CreateDescriptor(
          builtin::kSeconds, true),
      UnaryFunctionAdapter<Handle<Value>, absl::Time>::WrapFunction(
          [](ValueFactory& value_factory, absl::Time ts) -> Handle<Value> {
            return GetSeconds(value_factory, ts, "");
          })));

  CEL_RETURN_IF_ERROR(registry->Register(
      BinaryFunctionAdapter<Handle<Value>, absl::Time, const StringValue&>::
          CreateDescriptor(builtin::kMilliseconds, true),
      BinaryFunctionAdapter<Handle<Value>, absl::Time, const StringValue&>::
          WrapFunction([](ValueFactory& value_factory, absl::Time ts,
                          const StringValue& tz) -> Handle<Value> {
            return GetMilliseconds(value_factory, ts, tz.ToString());
          })));

  return registry->Register(
      UnaryFunctionAdapter<Handle<Value>, absl::Time>::CreateDescriptor(
          builtin::kMilliseconds, true),
      UnaryFunctionAdapter<Handle<Value>, absl::Time>::WrapFunction(
          [](ValueFactory& value_factory, absl::Time ts) -> Handle<Value> {
            return GetMilliseconds(value_factory, ts, "");
          }));
}

absl::Status RegisterBytesConversionFunctions(CelFunctionRegistry* registry,
                                              const InterpreterOptions&) {
  // bytes -> bytes
  CEL_RETURN_IF_ERROR(registry->Register(
      UnaryFunctionAdapter<Handle<BytesValue>, Handle<BytesValue>>::
          CreateDescriptor(builtin::kBytes, false),
      UnaryFunctionAdapter<Handle<BytesValue>, Handle<BytesValue>>::
          WrapFunction([](ValueFactory&, Handle<BytesValue> value)
                           -> Handle<BytesValue> { return value; })));

  // string -> bytes
  return registry->Register(
      UnaryFunctionAdapter<
          absl::StatusOr<Handle<BytesValue>>,
          const StringValue&>::CreateDescriptor(builtin::kBytes, false),
      UnaryFunctionAdapter<
          absl::StatusOr<Handle<BytesValue>>,
          const StringValue&>::WrapFunction([](ValueFactory& value_factory,
                                               const StringValue& value) {
        return value_factory.CreateBytesValue(value.ToString());
      }));
}

absl::Status RegisterDoubleConversionFunctions(CelFunctionRegistry* registry,
                                               const InterpreterOptions&) {
  // double -> double
  CEL_RETURN_IF_ERROR(
      registry->Register(UnaryFunctionAdapter<double, double>::CreateDescriptor(
                             builtin::kDouble, false),
                         UnaryFunctionAdapter<double, double>::WrapFunction(
                             [](ValueFactory&, double v) { return v; })));

  // int -> double
  CEL_RETURN_IF_ERROR(registry->Register(
      UnaryFunctionAdapter<double, int64_t>::CreateDescriptor(builtin::kDouble,
                                                              false),
      UnaryFunctionAdapter<double, int64_t>::WrapFunction(
          [](ValueFactory&, int64_t v) { return static_cast<double>(v); })));

  // string -> double
  CEL_RETURN_IF_ERROR(registry->Register(
      UnaryFunctionAdapter<Handle<Value>, const StringValue&>::CreateDescriptor(
          builtin::kDouble, false),
      UnaryFunctionAdapter<Handle<Value>, const StringValue&>::WrapFunction(
          [](ValueFactory& value_factory,
             const StringValue& s) -> Handle<Value> {
            double result;
            if (absl::SimpleAtod(s.ToString(), &result)) {
              return value_factory.CreateDoubleValue(result);
            } else {
              return value_factory.CreateErrorValue(absl::InvalidArgumentError(
                  "cannot convert string to double"));
            }
          })));

  // uint -> double
  return registry->Register(
      UnaryFunctionAdapter<double, uint64_t>::CreateDescriptor(builtin::kDouble,
                                                               false),
      UnaryFunctionAdapter<double, uint64_t>::WrapFunction(
          [](ValueFactory&, uint64_t v) { return static_cast<double>(v); }));
}

absl::Status RegisterIntConversionFunctions(CelFunctionRegistry* registry,
                                            const InterpreterOptions&) {
  // bool -> int
  CEL_RETURN_IF_ERROR(registry->Register(
      UnaryFunctionAdapter<int64_t, bool>::CreateDescriptor(builtin::kInt,
                                                            false),
      UnaryFunctionAdapter<int64_t, bool>::WrapFunction(
          [](ValueFactory&, bool v) { return static_cast<int64_t>(v); })));

  // double -> int
  CEL_RETURN_IF_ERROR(registry->Register(
      UnaryFunctionAdapter<Handle<Value>, double>::CreateDescriptor(
          builtin::kInt, false),
      UnaryFunctionAdapter<Handle<Value>, double>::WrapFunction(
          [](ValueFactory& value_factory, double v) -> Handle<Value> {
            auto conv = cel::internal::CheckedDoubleToInt64(v);
            if (!conv.ok()) {
              return value_factory.CreateErrorValue(conv.status());
            }
            return value_factory.CreateIntValue(*conv);
          })));

  // int -> int
  CEL_RETURN_IF_ERROR(registry->Register(
      UnaryFunctionAdapter<int64_t, int64_t>::CreateDescriptor(builtin::kInt,
                                                               false),
      UnaryFunctionAdapter<int64_t, int64_t>::WrapFunction(
          [](ValueFactory&, int64_t v) { return v; })));

  // string -> int
  CEL_RETURN_IF_ERROR(registry->Register(
      UnaryFunctionAdapter<Handle<Value>, const StringValue&>::CreateDescriptor(
          builtin::kInt, false),
      UnaryFunctionAdapter<Handle<Value>, const StringValue&>::WrapFunction(
          [](ValueFactory& value_factory,
             const StringValue& s) -> Handle<Value> {
            int64_t result;
            if (!absl::SimpleAtoi(s.ToString(), &result)) {
              return value_factory.CreateErrorValue(
                  absl::InvalidArgumentError("cannot convert string to int"));
            }
            return value_factory.CreateIntValue(result);
          })));

  // time -> int
  CEL_RETURN_IF_ERROR(registry->Register(
      UnaryFunctionAdapter<int64_t, absl::Time>::CreateDescriptor(builtin::kInt,
                                                                  false),
      UnaryFunctionAdapter<int64_t, absl::Time>::WrapFunction(
          [](ValueFactory&, absl::Time t) { return absl::ToUnixSeconds(t); })));

  // uint -> int
  return registry->Register(
      UnaryFunctionAdapter<Handle<Value>, uint64_t>::CreateDescriptor(
          builtin::kInt, false),
      UnaryFunctionAdapter<Handle<Value>, uint64_t>::WrapFunction(
          [](ValueFactory& value_factory, uint64_t v) -> Handle<Value> {
            auto conv = cel::internal::CheckedUint64ToInt64(v);
            if (!conv.ok()) {
              return value_factory.CreateErrorValue(conv.status());
            }
            return value_factory.CreateIntValue(*conv);
          }));
}

absl::Status RegisterStringConversionFunctions(
    CelFunctionRegistry* registry, const InterpreterOptions& options) {
  // May be optionally disabled to reduce potential allocs.
  if (!options.enable_string_conversion) {
    return absl::OkStatus();
  }

  CEL_RETURN_IF_ERROR(registry->Register(
      UnaryFunctionAdapter<Handle<Value>, const BytesValue&>::CreateDescriptor(
          builtin::kString, false),
      UnaryFunctionAdapter<Handle<Value>, const BytesValue&>::WrapFunction(
          [](ValueFactory& value_factory,
             const BytesValue& value) -> Handle<Value> {
            auto handle_or = value_factory.CreateStringValue(value.ToString());
            if (!handle_or.ok()) {
              return value_factory.CreateErrorValue(handle_or.status());
            }
            return *handle_or;
          })));

  // double -> string
  CEL_RETURN_IF_ERROR(registry->Register(
      UnaryFunctionAdapter<Handle<StringValue>, double>::CreateDescriptor(
          builtin::kString, false),
      UnaryFunctionAdapter<Handle<StringValue>, double>::WrapFunction(
          [](ValueFactory& value_factory, double value) -> Handle<StringValue> {
            return value_factory.CreateUncheckedStringValue(
                absl::StrCat(value));
          })));

  // int -> string
  CEL_RETURN_IF_ERROR(registry->Register(
      UnaryFunctionAdapter<Handle<StringValue>, int64_t>::CreateDescriptor(
          builtin::kString, false),
      UnaryFunctionAdapter<Handle<StringValue>, int64_t>::WrapFunction(
          [](ValueFactory& value_factory,
             int64_t value) -> Handle<StringValue> {
            return value_factory.CreateUncheckedStringValue(
                absl::StrCat(value));
          })));

  // string -> string
  CEL_RETURN_IF_ERROR(registry->Register(
      UnaryFunctionAdapter<Handle<StringValue>, Handle<StringValue>>::
          CreateDescriptor(builtin::kString, false),
      UnaryFunctionAdapter<Handle<StringValue>, Handle<StringValue>>::
          WrapFunction([](ValueFactory&, Handle<StringValue> value)
                           -> Handle<StringValue> { return value; })));

  // uint -> string
  CEL_RETURN_IF_ERROR(registry->Register(
      UnaryFunctionAdapter<Handle<StringValue>, uint64_t>::CreateDescriptor(
          builtin::kString, false),
      UnaryFunctionAdapter<Handle<StringValue>, uint64_t>::WrapFunction(
          [](ValueFactory& value_factory,
             uint64_t value) -> Handle<StringValue> {
            return value_factory.CreateUncheckedStringValue(
                absl::StrCat(value));
          })));

  // duration -> string
  CEL_RETURN_IF_ERROR(registry->Register(
      UnaryFunctionAdapter<Handle<Value>, absl::Duration>::CreateDescriptor(
          builtin::kString, false),
      UnaryFunctionAdapter<Handle<Value>, absl::Duration>::WrapFunction(
          [](ValueFactory& value_factory,
             absl::Duration value) -> Handle<Value> {
            auto encode = EncodeDurationToString(value);
            if (!encode.ok()) {
              return value_factory.CreateErrorValue(encode.status());
            }
            return value_factory.CreateUncheckedStringValue(*encode);
          })));

  // timestamp -> string
  return registry->Register(
      UnaryFunctionAdapter<Handle<Value>, absl::Time>::CreateDescriptor(
          builtin::kString, false),
      UnaryFunctionAdapter<Handle<Value>, absl::Time>::WrapFunction(
          [](ValueFactory& value_factory, absl::Time value) -> Handle<Value> {
            auto encode = EncodeTimeToString(value);
            if (!encode.ok()) {
              return value_factory.CreateErrorValue(encode.status());
            }
            return value_factory.CreateUncheckedStringValue(*encode);
          }));
}

absl::Status RegisterUintConversionFunctions(CelFunctionRegistry* registry,
                                             const InterpreterOptions&) {
  // double -> uint
  CEL_RETURN_IF_ERROR(registry->Register(
      UnaryFunctionAdapter<Handle<Value>, double>::CreateDescriptor(
          builtin::kUint, false),
      UnaryFunctionAdapter<Handle<Value>, double>::WrapFunction(
          [](ValueFactory& value_factory, double v) -> Handle<Value> {
            auto conv = cel::internal::CheckedDoubleToUint64(v);
            if (!conv.ok()) {
              return value_factory.CreateErrorValue(conv.status());
            }
            return value_factory.CreateUintValue(*conv);
          })));

  // int -> uint
  CEL_RETURN_IF_ERROR(registry->Register(
      UnaryFunctionAdapter<Handle<Value>, int64_t>::CreateDescriptor(
          builtin::kUint, false),
      UnaryFunctionAdapter<Handle<Value>, int64_t>::WrapFunction(
          [](ValueFactory& value_factory, int64_t v) -> Handle<Value> {
            auto conv = cel::internal::CheckedInt64ToUint64(v);
            if (!conv.ok()) {
              return value_factory.CreateErrorValue(conv.status());
            }
            return value_factory.CreateUintValue(*conv);
          })));

  // string -> uint
  CEL_RETURN_IF_ERROR(registry->Register(
      UnaryFunctionAdapter<Handle<Value>, const StringValue&>::CreateDescriptor(
          builtin::kUint, false),
      UnaryFunctionAdapter<Handle<Value>, const StringValue&>::WrapFunction(
          [](ValueFactory& value_factory,
             const StringValue& s) -> Handle<Value> {
            uint64_t result;
            if (!absl::SimpleAtoi(s.ToString(), &result)) {
              return value_factory.CreateErrorValue(
                  absl::InvalidArgumentError("doesn't convert to a string"));
            }
            return value_factory.CreateUintValue(result);
          })));

  // uint -> uint
  return registry->Register(
      UnaryFunctionAdapter<uint64_t, uint64_t>::CreateDescriptor(builtin::kUint,
                                                                 false),
      UnaryFunctionAdapter<uint64_t, uint64_t>::WrapFunction(
          [](ValueFactory&, uint64_t v) { return v; }));
}

absl::Status RegisterConversionFunctions(CelFunctionRegistry* registry,
                                         const InterpreterOptions& options) {
  CEL_RETURN_IF_ERROR(RegisterBytesConversionFunctions(registry, options));

  CEL_RETURN_IF_ERROR(RegisterDoubleConversionFunctions(registry, options));

  // duration() conversion from string.
  CEL_RETURN_IF_ERROR(registry->Register(
      UnaryFunctionAdapter<Handle<Value>, const StringValue&>::CreateDescriptor(
          builtin::kDuration, false),
      UnaryFunctionAdapter<Handle<Value>, const StringValue&>::WrapFunction(
          CreateDurationFromString)));

  // dyn() identity function.
  // TODO(issues/102): strip dyn() function references at type-check time.
  CEL_RETURN_IF_ERROR(registry->Register(
      UnaryFunctionAdapter<
          Handle<Value>, const Handle<Value>&>::CreateDescriptor(builtin::kDyn,
                                                                 false),
      UnaryFunctionAdapter<Handle<Value>, const Handle<Value>&>::WrapFunction(
          [](ValueFactory&, const Handle<Value>& value) -> Handle<Value> {
            return value;
          })));

  CEL_RETURN_IF_ERROR(RegisterIntConversionFunctions(registry, options));

  CEL_RETURN_IF_ERROR(RegisterStringConversionFunctions(registry, options));

  // timestamp conversion from int.
  CEL_RETURN_IF_ERROR(registry->Register(
      UnaryFunctionAdapter<Handle<Value>, int64_t>::CreateDescriptor(
          builtin::kTimestamp, false),
      UnaryFunctionAdapter<Handle<Value>, int64_t>::WrapFunction(
          [](ValueFactory&, int64_t epoch_seconds) -> Handle<Value> {
            return cel::interop_internal::CreateTimestampValue(
                absl::FromUnixSeconds(epoch_seconds));
          })));

  // timestamp() conversion from string.
  bool enable_timestamp_duration_overflow_errors =
      options.enable_timestamp_duration_overflow_errors;
  CEL_RETURN_IF_ERROR(registry->Register(
      UnaryFunctionAdapter<Handle<Value>, const StringValue&>::CreateDescriptor(
          builtin::kTimestamp, false),
      UnaryFunctionAdapter<Handle<Value>, const StringValue&>::WrapFunction(
          [=](ValueFactory& value_factory,
              const StringValue& time_str) -> Handle<Value> {
            absl::Time ts;
            if (!absl::ParseTime(absl::RFC3339_full, time_str.ToString(), &ts,
                                 nullptr)) {
              return value_factory.CreateErrorValue(absl::InvalidArgumentError(
                  "String to Timestamp conversion failed"));
            }
            if (enable_timestamp_duration_overflow_errors) {
              if (ts < absl::UniversalEpoch() || ts > kMaxTime) {
                return value_factory.CreateErrorValue(
                    absl::OutOfRangeError("timestamp overflow"));
              }
            }
            return cel::interop_internal::CreateTimestampValue(ts);
          })));

  return RegisterUintConversionFunctions(registry, options);
}

absl::Status RegisterCheckedTimeArithmeticFunctions(
    CelFunctionRegistry* registry) {
  CEL_RETURN_IF_ERROR(registry->Register(
      BinaryFunctionAdapter<Handle<Value>, absl::Time,
                            absl::Duration>::CreateDescriptor(builtin::kAdd,
                                                              false),
      BinaryFunctionAdapter<absl::StatusOr<Handle<Value>>, absl::Time,
                            absl::Duration>::
          WrapFunction([](ValueFactory& value_factory, absl::Time t1,
                          absl::Duration d2) -> absl::StatusOr<Handle<Value>> {
            auto sum = cel::internal::CheckedAdd(t1, d2);
            if (!sum.ok()) {
              return value_factory.CreateErrorValue(sum.status());
            }
            return value_factory.CreateTimestampValue(*sum);
          })));

  CEL_RETURN_IF_ERROR(registry->Register(
      BinaryFunctionAdapter<absl::StatusOr<Handle<Value>>, absl::Duration,
                            absl::Time>::CreateDescriptor(builtin::kAdd, false),
      BinaryFunctionAdapter<absl::StatusOr<Handle<Value>>, absl::Duration,
                            absl::Time>::
          WrapFunction([](ValueFactory& value_factory, absl::Duration d2,
                          absl::Time t1) -> absl::StatusOr<Handle<Value>> {
            auto sum = cel::internal::CheckedAdd(t1, d2);
            if (!sum.ok()) {
              return value_factory.CreateErrorValue(sum.status());
            }
            return value_factory.CreateTimestampValue(*sum);
          })));

  CEL_RETURN_IF_ERROR(registry->Register(
      BinaryFunctionAdapter<absl::StatusOr<Handle<Value>>, absl::Duration,
                            absl::Duration>::CreateDescriptor(builtin::kAdd,
                                                              false),
      BinaryFunctionAdapter<absl::StatusOr<Handle<Value>>, absl::Duration,
                            absl::Duration>::
          WrapFunction([](ValueFactory& value_factory, absl::Duration d1,
                          absl::Duration d2) -> absl::StatusOr<Handle<Value>> {
            auto sum = cel::internal::CheckedAdd(d1, d2);
            if (!sum.ok()) {
              return value_factory.CreateErrorValue(sum.status());
            }
            return value_factory.CreateDurationValue(*sum);
          })));

  CEL_RETURN_IF_ERROR(registry->Register(
      BinaryFunctionAdapter<
          absl::StatusOr<Handle<Value>>, absl::Time,
          absl::Duration>::CreateDescriptor(builtin::kSubtract, false),
      BinaryFunctionAdapter<absl::StatusOr<Handle<Value>>, absl::Time,
                            absl::Duration>::
          WrapFunction([](ValueFactory& value_factory, absl::Time t1,
                          absl::Duration d2) -> absl::StatusOr<Handle<Value>> {
            auto diff = cel::internal::CheckedSub(t1, d2);
            if (!diff.ok()) {
              return value_factory.CreateErrorValue(diff.status());
            }
            return value_factory.CreateTimestampValue(*diff);
          })));

  CEL_RETURN_IF_ERROR(registry->Register(
      BinaryFunctionAdapter<absl::StatusOr<Handle<Value>>, absl::Time,
                            absl::Time>::CreateDescriptor(builtin::kSubtract,
                                                          false),
      BinaryFunctionAdapter<absl::StatusOr<Handle<Value>>, absl::Time,
                            absl::Time>::
          WrapFunction([](ValueFactory& value_factory, absl::Time t1,
                          absl::Time t2) -> absl::StatusOr<Handle<Value>> {
            auto diff = cel::internal::CheckedSub(t1, t2);
            if (!diff.ok()) {
              return value_factory.CreateErrorValue(diff.status());
            }
            return value_factory.CreateDurationValue(*diff);
          })));

  CEL_RETURN_IF_ERROR(registry->Register(
      BinaryFunctionAdapter<
          absl::StatusOr<Handle<Value>>, absl::Duration,
          absl::Duration>::CreateDescriptor(builtin::kSubtract, false),
      BinaryFunctionAdapter<absl::StatusOr<Handle<Value>>, absl::Duration,
                            absl::Duration>::
          WrapFunction([](ValueFactory& value_factory, absl::Duration d1,
                          absl::Duration d2) -> absl::StatusOr<Handle<Value>> {
            auto diff = cel::internal::CheckedSub(d1, d2);
            if (!diff.ok()) {
              return value_factory.CreateErrorValue(diff.status());
            }
            return value_factory.CreateDurationValue(*diff);
          })));

  return absl::OkStatus();
}

absl::Status RegisterUncheckedTimeArithmeticFunctions(
    CelFunctionRegistry* registry) {
  // TODO(issues/5): deprecate unchecked time math functions when clients no
  // longer depend on them.
  CEL_RETURN_IF_ERROR(registry->Register(
      BinaryFunctionAdapter<Handle<Value>, absl::Time,
                            absl::Duration>::CreateDescriptor(builtin::kAdd,
                                                              false),
      BinaryFunctionAdapter<Handle<Value>, absl::Time, absl::Duration>::
          WrapFunction([](ValueFactory& value_factory, absl::Time t1,
                          absl::Duration d2) -> Handle<Value> {
            return value_factory.CreateUncheckedTimestampValue(t1 + d2);
          })));

  CEL_RETURN_IF_ERROR(registry->Register(
      BinaryFunctionAdapter<Handle<Value>, absl::Duration,
                            absl::Time>::CreateDescriptor(builtin::kAdd, false),
      BinaryFunctionAdapter<Handle<Value>, absl::Duration, absl::Time>::
          WrapFunction([](ValueFactory& value_factory, absl::Duration d2,
                          absl::Time t1) -> Handle<Value> {
            return value_factory.CreateUncheckedTimestampValue(t1 + d2);
          })));

  CEL_RETURN_IF_ERROR(registry->Register(
      BinaryFunctionAdapter<Handle<Value>, absl::Duration,
                            absl::Duration>::CreateDescriptor(builtin::kAdd,
                                                              false),
      BinaryFunctionAdapter<Handle<Value>, absl::Duration, absl::Duration>::
          WrapFunction([](ValueFactory& value_factory, absl::Duration d1,
                          absl::Duration d2) -> Handle<Value> {
            return value_factory.CreateUncheckedDurationValue(d1 + d2);
          })));

  CEL_RETURN_IF_ERROR(registry->Register(
      BinaryFunctionAdapter<Handle<Value>, absl::Time, absl::Duration>::
          CreateDescriptor(builtin::kSubtract, false),

      BinaryFunctionAdapter<Handle<Value>, absl::Time, absl::Duration>::
          WrapFunction(

              [](ValueFactory& value_factory, absl::Time t1,
                 absl::Duration d2) -> Handle<Value> {
                return value_factory.CreateUncheckedTimestampValue(t1 - d2);
              })));

  CEL_RETURN_IF_ERROR(registry->Register(
      BinaryFunctionAdapter<Handle<Value>, absl::Time,
                            absl::Time>::CreateDescriptor(builtin::kSubtract,
                                                          false),
      BinaryFunctionAdapter<Handle<Value>, absl::Time, absl::Time>::
          WrapFunction(

              [](ValueFactory& value_factory, absl::Time t1,
                 absl::Time t2) -> Handle<Value> {
                return value_factory.CreateUncheckedDurationValue(t1 - t2);
              })));

  CEL_RETURN_IF_ERROR(registry->Register(
      BinaryFunctionAdapter<Handle<Value>, absl::Duration, absl::Duration>::
          CreateDescriptor(builtin::kSubtract, false),
      BinaryFunctionAdapter<Handle<Value>, absl::Duration, absl::Duration>::
          WrapFunction([](ValueFactory& value_factory, absl::Duration d1,
                          absl::Duration d2) -> Handle<Value> {
            return value_factory.CreateUncheckedDurationValue(d1 - d2);
          })));

  return absl::OkStatus();
}

absl::Status RegisterTimeFunctions(CelFunctionRegistry* registry,
                                   const InterpreterOptions& options) {
  CEL_RETURN_IF_ERROR(RegisterTimestampFunctions(registry, options));

  // Special arithmetic operators for Timestamp and Duration
  if (options.enable_timestamp_duration_overflow_errors) {
    CEL_RETURN_IF_ERROR(RegisterCheckedTimeArithmeticFunctions(registry));
  } else {
    CEL_RETURN_IF_ERROR(RegisterUncheckedTimeArithmeticFunctions(registry));
  }

  // duration breakdown accessor functions
  using DurationAccessorFunction =
      UnaryFunctionAdapter<int64_t, absl::Duration>;
  CEL_RETURN_IF_ERROR(registry->Register(
      DurationAccessorFunction::CreateDescriptor(builtin::kHours, true),
      DurationAccessorFunction::WrapFunction(
          [](ValueFactory&, absl::Duration d) -> int64_t {
            return absl::ToInt64Hours(d);
          })));

  CEL_RETURN_IF_ERROR(registry->Register(
      DurationAccessorFunction::CreateDescriptor(builtin::kMinutes, true),
      DurationAccessorFunction::WrapFunction(
          [](ValueFactory&, absl::Duration d) -> int64_t {
            return absl::ToInt64Minutes(d);
          })));

  CEL_RETURN_IF_ERROR(registry->Register(
      DurationAccessorFunction::CreateDescriptor(builtin::kSeconds, true),
      DurationAccessorFunction::WrapFunction(
          [](ValueFactory&, absl::Duration d) -> int64_t {
            return absl::ToInt64Seconds(d);
          })));

  CEL_RETURN_IF_ERROR(registry->Register(
      DurationAccessorFunction::CreateDescriptor(builtin::kMilliseconds, true),
      DurationAccessorFunction::WrapFunction(
          [](ValueFactory&, absl::Duration d) -> int64_t {
            constexpr int64_t millis_per_second = 1000L;
            return absl::ToInt64Milliseconds(d) % millis_per_second;
          })));

  return absl::OkStatus();
}

int64_t MapSizeImpl(ValueFactory&, const MapValue& value) {
  return value.size();
}

int64_t ListSizeImpl(ValueFactory&, const ListValue& value) {
  return value.size();
}

absl::Status RegisterContainerFunctions(CelFunctionRegistry* registry,
                                        const InterpreterOptions& options) {
  // receiver style = true/false
  // Support both the global and receiver style size() for lists and maps.
  for (bool receiver_style : {true, false}) {
    CEL_RETURN_IF_ERROR(registry->Register(
        UnaryFunctionAdapter<int64_t, const ListValue&>::CreateDescriptor(
            builtin::kSize, receiver_style),
        UnaryFunctionAdapter<int64_t, const ListValue&>::WrapFunction(
            ListSizeImpl)));

    CEL_RETURN_IF_ERROR(registry->Register(
        UnaryFunctionAdapter<int64_t, const MapValue&>::CreateDescriptor(
            builtin::kSize, receiver_style),
        UnaryFunctionAdapter<int64_t, const MapValue&>::WrapFunction(
            MapSizeImpl)));
  }

  if (options.enable_list_concat) {
    CEL_RETURN_IF_ERROR(registry->Register(
        BinaryFunctionAdapter<absl::StatusOr<Handle<Value>>, const ListValue&,
                              const ListValue&>::CreateDescriptor(builtin::kAdd,
                                                                  false),
        BinaryFunctionAdapter<absl::StatusOr<Handle<Value>>, const ListValue&,
                              const ListValue&>::WrapFunction(ConcatList)));
  }

  return registry->Register(PortableBinaryFunctionAdapter<
                            const CelList*, const CelList*,
                            const CelList*>::Create(builtin::kRuntimeListAppend,
                                                    false, AppendList));
}

}  // namespace

absl::Status RegisterBuiltinFunctions(CelFunctionRegistry* registry,
                                      const InterpreterOptions& options) {
  CEL_RETURN_IF_ERROR(registry->RegisterAll(
      {
          &RegisterEqualityFunctions,
          &RegisterComparisonFunctions,
          &RegisterLogicalFunctions,
          &RegisterNumericArithmeticFunctions,
          &RegisterConversionFunctions,
          &RegisterTimeFunctions,
          &RegisterStringFunctions,
          &RegisterRegexFunctions,
          &RegisterSetMembershipFunctions,
          &RegisterContainerFunctions,
      },
      options));

  return registry->Register(
      UnaryFunctionAdapter<
          Handle<Value>, const Handle<Value>&>::CreateDescriptor(builtin::kType,
                                                                 false),
      UnaryFunctionAdapter<Handle<Value>, const Handle<Value>&>::WrapFunction(
          [](ValueFactory& factory, const Handle<Value>& value) {
            // TODO(issues/5): legacy types don't interop with type values
            // from factory. This should simply be:
            //
            // return factory.CreateTypeValue(value->type());
            Arena* arena =
                cel::extensions::ProtoMemoryManager::CastToProtoArena(
                    factory.memory_manager());
            CelValue legacy_value =
                cel::interop_internal::ModernValueToLegacyValueOrDie(arena,
                                                                     value);
            return cel::interop_internal::CreateTypeValueFromView(
                legacy_value.ObtainCelType().CelTypeOrDie().value());
          }));
}

}  // namespace google::api::expr::runtime
