module;

#include <format>
#include <stacktrace>

#include <Windows.h>

export module ErrorHelpers;

using namespace std;

#define MESSAGE format("{}{}{}", message, empty(message) ? "" : "\n\n", to_string(stacktrace))

#define ThrowException(Type, Succeeded) \
	void ThrowIfFailed(same_as<Type> auto value, string_view message = {}, const stacktrace& stacktrace = stacktrace::current()) { \
		if (!Succeeded) throw_std_system_error(static_cast<int>(GetLastError()), message, stacktrace); \
	}

export namespace ErrorHelpers {
	[[noreturn]] void throw_std_system_error(int code, string_view message = {}, const stacktrace& stacktrace = stacktrace::current()) {
		throw system_error(code, system_category(), MESSAGE);
	}

	template <constructible_from<const char*> T>
	[[noreturn]] void Throw(string_view message, const stacktrace& stacktrace = stacktrace::current()) { throw T(MESSAGE); }

	ThrowException(BOOL, value);
	ThrowException(HRESULT, SUCCEEDED(value));
}
