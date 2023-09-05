// Copyright 2022 Google LLC
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

#ifndef THIRD_PARTY_CEL_CPP_BASE_VALUES_DURATION_VALUE_H_
#define THIRD_PARTY_CEL_CPP_BASE_VALUES_DURATION_VALUE_H_

#include <string>

#include "absl/base/attributes.h"
#include "absl/log/absl_check.h"
#include "absl/time/time.h"
#include "base/types/duration_type.h"
#include "base/value.h"

namespace cel {

class DurationValue final
    : public base_internal::SimpleValue<DurationType, absl::Duration> {
 private:
  using Base = base_internal::SimpleValue<DurationType, absl::Duration>;

 public:
  ABSL_ATTRIBUTE_PURE_FUNCTION static std::string DebugString(
      absl::Duration value);

  using Base::kKind;

  using Base::Is;

  static const DurationValue& Cast(const Value& value) {
    ABSL_DCHECK(Is(value)) << "cannot cast " << value.type()->name()
                           << " to google.protobuf.Duration";
    return static_cast<const DurationValue&>(value);
  }

  static Handle<DurationValue> Zero(ValueFactory& value_factory);

  using Base::kind;

  using Base::type;

  std::string DebugString() const;

  absl::StatusOr<Any> ConvertToAny(ValueFactory&) const;

  absl::StatusOr<Json> ConvertToJson(ValueFactory&) const;

  absl::StatusOr<Handle<Value>> Equals(ValueFactory& value_factory,
                                       const Value& other) const;

  using Base::value;

 private:
  using Base::Base;

  CEL_INTERNAL_SIMPLE_VALUE_MEMBERS(DurationValue);
};

CEL_INTERNAL_SIMPLE_VALUE_STANDALONES(DurationValue);

inline bool operator==(const DurationValue& lhs, const DurationValue& rhs) {
  return lhs.value() == rhs.value();
}

namespace base_internal {

template <>
struct ValueTraits<DurationValue> {
  using type = DurationValue;

  using type_type = DurationType;

  using underlying_type = absl::Duration;

  static std::string DebugString(underlying_type value) {
    return type::DebugString(value);
  }

  static std::string DebugString(const type& value) {
    return value.DebugString();
  }

  static Handle<type> Wrap(ValueFactory& value_factory, underlying_type value);

  static underlying_type Unwrap(underlying_type value) { return value; }

  static underlying_type Unwrap(const Handle<type>& value) {
    return Unwrap(value->value());
  }
};

}  // namespace base_internal

}  // namespace cel

#endif  // THIRD_PARTY_CEL_CPP_BASE_VALUES_DURATION_VALUE_H_
