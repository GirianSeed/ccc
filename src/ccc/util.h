// This file is part of the Chaos Compiler Collection.
// SPDX-License-Identifier: MIT

#pragma once

#include <set>
#include <span>
#include <cstdio>
#include <vector>
#include <memory>
#include <string>
#include <cstdint>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <optional>

namespace ccc {

using u8 = unsigned char;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

using s8 = signed char;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;

#ifdef _WIN32
	#define CCC_ANSI_COLOUR_OFF ""
	#define CCC_ANSI_COLOUR_RED ""
	#define CCC_ANSI_COLOUR_MAGENTA ""
	#define CCC_ANSI_COLOUR_GRAY ""
#else
	#define CCC_ANSI_COLOUR_OFF "\033[0m"
	#define CCC_ANSI_COLOUR_RED "\033[31m"
	#define CCC_ANSI_COLOUR_MAGENTA "\033[35m"
	#define CCC_ANSI_COLOUR_GRAY "\033[90m"
#endif

struct Error {
	std::string message;
	const char* source_file;
	s32 source_line;
};

Error format_error(const char* source_file, int source_line, const char* format, ...);
void print_error(FILE* out, const Error& error);
void print_warning(FILE* out, const Error& warning);

#define CCC_FATAL(...) \
	{ \
		ccc::Error error = ccc::format_error(__FILE__, __LINE__, __VA_ARGS__); \
		ccc::print_error(stderr, error); \
		exit(1); \
	}
	
#define CCC_CHECK_FATAL(condition, ...) \
	if(!(condition)) { \
		ccc::Error error = ccc::format_error(__FILE__, __LINE__, __VA_ARGS__); \
		ccc::print_error(stderr, error); \
		exit(1); \
	}

#define CCC_ASSERT(condition) \
	CCC_CHECK_FATAL(condition, #condition)

// The main error handling construct in CCC. This class is used to bundle
// together a return value and a pointer to error information, so that errors
// can be propagated up the stack.
template <typename Value>
class [[nodiscard]] Result {
	template <typename OtherValue>
	friend class Result;
protected:
	Value m_value;
	std::unique_ptr<Error> m_error;
	
	Result() {}
	
public:
	Result(Value value) : m_value(std::move(value)), m_error(nullptr) {}
	
	// Used to propagate errors up the call stack.
	template <typename OtherValue>
	Result(Result<OtherValue>&& rhs) {
		CCC_ASSERT(rhs.m_error != nullptr);
		m_error = std::move(rhs.m_error);
	}
	
	static Result<Value> failure(Error error) {
		Result<Value> result;
		result.m_error = std::make_unique<Error>(std::move(error));
		return result;
	}
	
	bool success() const {
		return m_error == nullptr;
	}
	
	const Error& error() const {
		CCC_ASSERT(m_error != nullptr);
		return *m_error;
	}
	
	Value& operator*() {
		CCC_ASSERT(m_error == nullptr);
		return m_value;
	}
	
	const Value& operator*() const {
		CCC_ASSERT(m_error == nullptr);
		return m_value;
	}
	
	Value* operator->() {
		CCC_ASSERT(m_error == nullptr);
		return &m_value;
	}
	
	const Value* operator->() const {
		CCC_ASSERT(m_error == nullptr);
		return &m_value;
	}
};

template <>
class [[nodiscard]] Result<void> : public Result<int> {
public:
	Result() : Result<int>(0) {}
	
	// Used to propagate errors up the call stack.
	template <typename OtherValue>
	Result(Result<OtherValue>&& rhs) {
		CCC_ASSERT(rhs.m_error != nullptr);
		m_error = std::move(rhs.m_error);
	}
};

#define CCC_FAILURE(...) ccc::Result<int>::failure(ccc::format_error(__FILE__, __LINE__, __VA_ARGS__))

#define CCC_CHECK(condition, ...) \
	if(!(condition)) { \
		return CCC_FAILURE(__VA_ARGS__); \
	}

#define CCC_EXPECT_CHAR(input, c, context) \
	CCC_CHECK(*(input++) == c, \
		"Expected '%c' in %s, got '%c' (%02hhx)", \
		c, context, *(input - 1), *(input - 1))

#define CCC_RETURN_IF_ERROR(result) \
	if(!(result).success()) { \
		return (result); \
	}

#define CCC_EXIT_IF_ERROR(result) \
	if(!(result).success()) { \
		ccc::print_error(stderr, (result).error()); \
		exit(1); \
	}

#define CCC_GTEST_FAIL_IF_ERROR(result) \
	if(!(result).success()) { \
		FAIL() << (result).error().message; \
	}

template <typename... Args>
void warn_impl(const char* source_file, int source_line, const char* format, Args... args) {
	Error warning = format_error(source_file, source_line, format, args...);
	print_warning(stderr, warning);
}
#define CCC_WARN(...) \
	ccc::warn_impl(__FILE__, __LINE__, __VA_ARGS__)

#ifdef _MSTORAGE_CLASS_VER
	#define CCC_PACKED_STRUCT(name, ...) \
		__pragma(pack(push, 1)) struct name { __VA_ARGS__ } __pragma(pack(pop));
#else
	#define CCC_PACKED_STRUCT(name, ...) \
		struct __attribute__((__packed__)) name { __VA_ARGS__ };
#endif

template <typename T>
const T* get_packed(std::span<const u8> bytes, u64 offset) {
	if(offset + sizeof(T) <= bytes.size()) {
		return reinterpret_cast<const T*>(&bytes[offset]);
	} else {
		return nullptr;
	}
}

const char* get_string(std::span<const u8> bytes, u64 offset);

#define CCC_BEGIN_END(x) (x).begin(), (x).end()
#define CCC_ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

#define CCC_FOURCC(string) ((string)[0] | (string)[1] << 8 | (string)[2] << 16 | (string)[3] << 24)

struct Address {
	u32 value = (u32) -1;
	
	Address() {}
	Address(u32 v) : value(v) {}
	
	bool valid() const {
		return value != (u32) -1;
	}
	
	u32 get_or_zero() const {
		if(valid()) {
			return value;
		} else {
			return 0;
		}
	}
	
	friend auto operator<=>(const Address& lhs, const Address& rhs) = default;
};

struct AddressRange {
	Address low;
	Address high;
	
	friend auto operator<=>(const AddressRange& lhs, const AddressRange& rhs) = default;
	bool valid() const { return low.valid(); }
};

// These functions are to be used only for source file paths present in the
// symbol table, since we want them to be handled consistently across different
// platforms, which with std::filesystem::path doesn't seem to be possible.
std::string merge_paths(const std::string& base, const std::string& path);
std::string normalise_path(const char* input, bool use_backslashes_as_path_separators);
bool guess_is_windows_path(const char* path);
std::string extract_file_name(const std::string& path);

namespace ast { struct Node; }

// These are used to reference STABS types from other types within a single
// translation unit. For most games these will just be a single number, the type
// number. In some cases, for example with the homebrew SDK, type numbers are a
// pair of two numbers surrounded by round brackets e.g. (1,23) where the first
// number is the index of the include file to use (includes are listed for each
// translation unit separately), and the second number is the type number.
struct StabsTypeNumber {
	s32 file = -1;
	s32 type = -1;
	
	friend auto operator<=>(const StabsTypeNumber& lhs, const StabsTypeNumber& rhs) = default;
};

enum StorageClass {
	STORAGE_CLASS_NONE = 0,
	STORAGE_CLASS_TYPEDEF = 1,
	STORAGE_CLASS_EXTERN = 2,
	STORAGE_CLASS_STATIC = 3,
	STORAGE_CLASS_AUTO = 4,
	STORAGE_CLASS_REGISTER = 5
};

}
