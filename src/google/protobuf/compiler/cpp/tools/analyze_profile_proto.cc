// Protocol Buffers - Google's data interchange format
// Copyright 2023 Google Inc.  All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "google/protobuf/compiler/cpp/tools/analyze_profile_proto.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "google/protobuf/testing/file.h"
#include "google/protobuf/testing/file.h"
#include "google/protobuf/compiler/access_info_map.h"
#include "google/protobuf/compiler/profile_bootstrap.pb.h"
#include "absl/log/absl_log.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/cord.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/string_view.h"
#include "google/protobuf/compiler/cpp/cpp_access_info_parse_helper.h"
#include "google/protobuf/compiler/cpp/helpers.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"
#include "third_party/re2/re2.h"

namespace google {
namespace protobuf {
namespace compiler {
namespace tools {

namespace {

enum PDProtoScale { kNever, kRarely, kDefault, kLikely };

struct PDProtoAnalysis {
  PDProtoScale presence = PDProtoScale::kDefault;
  PDProtoScale usage = PDProtoScale::kDefault;
};

std::ostream& operator<<(std::ostream& s, PDProtoScale scale) {
  switch (scale) {
    case PDProtoScale::kNever:
      return s << "NEVER";
    case PDProtoScale::kRarely:
      return s << "RARELY";
    case PDProtoScale::kDefault:
      return s << "DEFAULT";
    case PDProtoScale::kLikely:
      return s << "LIKELY";
  }
  return s;
}

enum PDProtoOptimization { kNone, kLazy, kInline, kSplit };

std::ostream& operator<<(std::ostream& s, PDProtoOptimization optimization) {
  switch (optimization) {
    case PDProtoOptimization::kNone:
      return s << "NONE";
    case PDProtoOptimization::kLazy:
      return s << "LAZY";
    case PDProtoOptimization::kInline:
      return s << "INLINE";
    case PDProtoOptimization::kSplit:
      return s << "SPLIT";
  }
  return s;
}

class PDProtoAnalyzer {
 public:
  explicit PDProtoAnalyzer(const AccessInfo& access_info)
      : info_map_(access_info) {
    info_map_.SetAccessInfoParseHelper(
        std::make_unique<cpp::CppAccessInfoParseHelper>());
  }

  bool HasProfile(const Descriptor* descriptor) const {
    return info_map_.HasProfile(descriptor);
  }

  PDProtoAnalysis AnalyzeField(const FieldDescriptor* field) {
    PDProtoAnalysis analysis;

    if (info_map_.InProfile(field)) {
      if (IsLikelyPresent(field)) {
        analysis.presence = PDProtoScale::kLikely;
      } else if (IsRarelyPresent(field)) {
        analysis.presence = PDProtoScale::kRarely;
      }
    }

    if (info_map_.InProfile(field) &&
        info_map_.AccessCount(field, AccessInfoMap::kReadWriteOther) <=
            info_map_.GetUnlikelyUsedThreshold()) {
      analysis.usage = PDProtoScale::kRarely;
    }

    return analysis;
  }

  PDProtoOptimization OptimizeField(const FieldDescriptor* field) {
    PDProtoAnalysis analysis = AnalyzeField(field);
    if (field->cpp_type() == FieldDescriptor::CPPTYPE_STRING) {
      if (analysis.presence >= PDProtoScale::kLikely) {
        if (cpp::CanStringBeInlined(field)) {
          return PDProtoOptimization::kInline;
        }
      }
    }

    if (field->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE) {
      if (analysis.presence > PDProtoScale::kRarely) {
        // Exclude 'never' as that may simply mean we have no data.
        if (analysis.usage == PDProtoScale::kRarely) {
          if (!field->is_repeated()) {
            return PDProtoOptimization::kLazy;
          }
        }
      }
    }
    return PDProtoOptimization::kNone;
  }

  uint64_t UnlikelyUsedThreshold() const {
    return info_map_.GetUnlikelyUsedThreshold();
  }

 private:
  bool IsLikelyPresent(const FieldDescriptor* field) const {
    // This threshold was arbitrarily chosen based on a few macrobenchmark
    // results.
    constexpr double kHotRatio = 0.90;

    return (info_map_.IsHot(field, AccessInfoMap::kRead, kHotRatio) ||
            info_map_.IsHot(field, AccessInfoMap::kWrite, kHotRatio));
  }

  bool IsRarelyPresent(const FieldDescriptor* field) const {
    // This threshold was arbitrarily chosen based on a few macrobenchmark
    // results. Since most cold fields have zero presence count, PDProto
    // optimization hasn't been sensitive to the threshold.
    constexpr double kColdRatio = 0.005;

    return info_map_.IsCold(field, AccessInfoMap::kRead, kColdRatio) &&
           info_map_.IsCold(field, AccessInfoMap::kWrite, kColdRatio);
  }

  AccessInfoMap info_map_;
};

size_t GetLongestName(const DescriptorPool& pool, absl::string_view name,
                      size_t min_length) {
  size_t pos = name.length();
  while (pos > min_length) {
    if (name[--pos] == '_') {
      if (pool.FindMessageTypeByName(name.substr(0, pos))) {
        return pos;
      }
    }
  }
  return 0;
}

const Descriptor* FindMessageTypeByCppName(const DescriptorPool& pool,
                                           absl::string_view name) {
  std::string s = absl::StrReplaceAll(name, {{"::", "."}});
  const Descriptor* descriptor = pool.FindMessageTypeByName(s);
  if (descriptor) return descriptor;

  size_t min_length = 1;
  while (size_t pos = GetLongestName(pool, s, min_length)) {
    s[pos] = '.';
    descriptor = pool.FindMessageTypeByName(s);
    if (descriptor) return descriptor;
    min_length = pos + 1;
  }

  ABSL_LOG(WARNING) << "Unknown c++ message name '" << name << "'";
  return nullptr;
}

std::string TypeName(const FieldDescriptor* descriptor) {
  if (descriptor == nullptr) return "UNKNOWN";
  std::string s;
  switch (descriptor->cpp_type()) {
    case FieldDescriptor::CPPTYPE_INT32:
      s = "int32";
      break;
    case FieldDescriptor::CPPTYPE_INT64:
      s = "int64";
      break;
    case FieldDescriptor::CPPTYPE_UINT32:
      s = "uint32";
      break;
    case FieldDescriptor::CPPTYPE_UINT64:
      s = "uint64";
      break;
    case FieldDescriptor::CPPTYPE_DOUBLE:
      s = "double";
      break;
    case FieldDescriptor::CPPTYPE_FLOAT:
      s = "float";
      break;
    case FieldDescriptor::CPPTYPE_BOOL:
      s = "bool";
      break;
    case FieldDescriptor::CPPTYPE_ENUM:
      s = "enum";
      break;
    case FieldDescriptor::CPPTYPE_STRING:
      s = "string";
      break;
    case FieldDescriptor::CPPTYPE_MESSAGE:
      s = descriptor->message_type()->name();
      break;
    default:
      s = "UNKNOWN";
      break;
  }
  if (descriptor->is_repeated()) s += "[]";
  return s;
}

absl::StatusOr<AccessInfo> AccessInfoFromFile(absl::string_view profile) {
  absl::Cord cord;
  absl::Status status = GetContents(profile, &cord, true);
  if (!status.ok()) {
    return status;
  }

  AccessInfo access_info_proto;
  if (!access_info_proto.ParseFromCord(cord)) {
    return absl::DataLossError("Failed to parse AccessInfo");
  }

  return access_info_proto;
}

std::vector<const MessageAccessInfo*> SortMessages(
    const AccessInfo& access_info) {
  std::vector<const MessageAccessInfo*> sorted;
  for (const MessageAccessInfo& info : access_info.message()) {
    sorted.push_back(&info);
  }
  std::sort(sorted.begin(), sorted.end(),
            [](const MessageAccessInfo* lhs, const MessageAccessInfo* rhs) {
              return lhs->name() < rhs->name();
            });
  return sorted;
}

}  // namespace

absl::Status AnalyzeProfileProtoToText(
    std::ostream& stream, absl::string_view proto_profile,
    const AnalyzeProfileProtoOptions& options) {
  if (options.pool == nullptr) {
    return absl::InvalidArgumentError("pool must not be null");
  }
  const DescriptorPool& pool = *options.pool;
  RE2 regex(options.message_filter.empty() ? ".*" : options.message_filter);
  if (!regex.ok()) {
    return absl::InvalidArgumentError("Invalid regular expression");
  }

  absl::StatusOr<AccessInfo> access_info = AccessInfoFromFile(proto_profile);
  if (!access_info.ok()) {
    return access_info.status();
  }
  PDProtoAnalyzer analyzer(*access_info);

  if (options.print_unused_threshold) {
    stream << "Unlikely Used Threshold = " << analyzer.UnlikelyUsedThreshold()
           << "\n"
           << "See http://go/pdlazy for more information\n"
           << "-----------------------------------------\n";
  }

  for (const MessageAccessInfo* message : SortMessages(*access_info)) {
    if (RE2::PartialMatch(message->name(), regex)) {
      if (const Descriptor* descriptor =
              FindMessageTypeByCppName(pool, message->name())) {
        if (analyzer.HasProfile(descriptor)) {
          bool message_header = false;
          for (int i = 0; i < descriptor->field_count(); ++i) {
            const FieldDescriptor* field = descriptor->field(i);
            PDProtoAnalysis analysis = analyzer.AnalyzeField(field);
            PDProtoOptimization optimized = analyzer.OptimizeField(field);
            if (options.print_all_fields || options.print_analysis ||
                optimized != PDProtoOptimization::kNone) {
              if (!message_header) {
                message_header = true;
                stream << "Message " << descriptor->full_name() << "\n";
              }
              stream << "  " << TypeName(field) << " " << field->name() << ":";

              if (options.print_analysis) {
                if (analysis.presence != PDProtoScale::kDefault) {
                  stream << " " << analysis.presence << "_PRESENT";
                }
                if (analysis.usage != PDProtoScale::kDefault) {
                  stream << " " << analysis.usage << "_USED";
                }
              }
              if (optimized != PDProtoOptimization::kNone) {
                stream << " " << optimized;
              }
              stream << "\n";
            }
          }
        }
      }
    }
  }
  return absl::OkStatus();
}

}  // namespace tools
}  // namespace compiler
}  // namespace protobuf
}  // namespace google
