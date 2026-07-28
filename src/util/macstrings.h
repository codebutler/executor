#pragma once

#include <string>
#include <string_view>

// libc++ >= 19 removed the non-standard char_traits<unsigned char>
// specialization that mac_string / mac_string_view rely on. Provide it.
#if defined(_LIBCPP_VERSION) && _LIBCPP_VERSION >= 190000
#include <cstring>
namespace std
{
template <>
struct char_traits<unsigned char>
{
    using char_type = unsigned char;
    using int_type = int;
    using off_type = streamoff;
    using pos_type = streampos;
    using state_type = mbstate_t;

    static void assign(char_type& a, const char_type& b) noexcept { a = b; }
    static constexpr bool eq(char_type a, char_type b) noexcept { return a == b; }
    static constexpr bool lt(char_type a, char_type b) noexcept { return a < b; }
    static int compare(const char_type* a, const char_type* b, size_t n) { return n == 0 ? 0 : memcmp(a, b, n); }
    static size_t length(const char_type* s) { return strlen(reinterpret_cast<const char*>(s)); }
    static const char_type* find(const char_type* s, size_t n, const char_type& c)
    { return static_cast<const char_type*>(memchr(s, c, n)); }
    static char_type* move(char_type* d, const char_type* s, size_t n)
    { return static_cast<char_type*>(memmove(d, s, n)); }
    static char_type* copy(char_type* d, const char_type* s, size_t n)
    { return static_cast<char_type*>(memcpy(d, s, n)); }
    static char_type* assign(char_type* d, size_t n, char_type c)
    { return static_cast<char_type*>(memset(d, c, n)); }
    static constexpr int_type not_eof(int_type i) noexcept { return eq_int_type(i, eof()) ? 0 : i; }
    static constexpr char_type to_char_type(int_type i) noexcept { return char_type(i); }
    static constexpr int_type to_int_type(char_type c) noexcept { return int_type(c); }
    static constexpr bool eq_int_type(int_type a, int_type b) noexcept { return a == b; }
    static constexpr int_type eof() noexcept { return int_type(EOF); }
};
}
#endif
#include <rsys/filesystem.h>
#include <base/mactype.h>

namespace Executor
{

//using mac_string_view = std::basic_string_view<unsigned char>;

struct mac_string_view : std::basic_string_view<unsigned char>
{
    mac_string_view() = default;
    mac_string_view(const unsigned char* pascalString)
    {
        if(pascalString)
            *this = mac_string_view(pascalString + 1, pascalString[0]);
    }
    mac_string_view(GUEST<unsigned char*> pascalString)
        : mac_string_view(toHost(pascalString))
    {
    }
    mac_string_view(GUEST<const unsigned char*> pascalString)
        : mac_string_view(toHost(pascalString))
    {
    }

    mac_string_view(const unsigned char* p, size_t n)
        : basic_string_view(p,n)
    {
    }

    mac_string_view(std::basic_string_view<unsigned char> v)
        : basic_string_view(std::move(v))
    {
    }
    mac_string_view(const std::basic_string<unsigned char>& v)
        : basic_string_view(v)
    {
    }

    template<class It>
    mac_string_view(It p, It q)
        : basic_string_view(&*p,q-p)
    {
    }
};

inline mac_string_view PascalStringView(const unsigned char *s)
{
    if(s)
        return mac_string_view(s+1, (size_t)s[0]);
    else
        return mac_string_view();
}

unsigned char *assignPString(unsigned char* s, mac_string_view v, int max = 255);

using mac_string = std::basic_string<unsigned char>;

std::u32string toUnicode(mac_string_view sv);
mac_string toMacRoman(const std::u32string& s);

fs::path toUnicodeFilename(mac_string_view sv);
mac_string toMacRomanFilename(const fs::path& s, int index = 0);

bool matchesMacRomanFilename(const fs::path& s, mac_string_view sv);

bool equalCaseInsensitive(mac_string_view s1, mac_string_view s2);
}
