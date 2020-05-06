#include "Environment.h"
#include "WindowsSlim.h"
#include <regex>
#include <cassert>

using std::wstring;
using std::wregex;
using std::wsmatch;
using std::wsregex_iterator;

namespace xloil
{

  std::wstring getEnvVar(const wchar_t * name)
  {
    return captureStringBuffer(
      [name](auto* buf, auto len)
    {
      return GetEnvironmentVariable(name, buf, (DWORD)len);
    });
  }

  std::wstring expandEnvironmentStrings(const wchar_t* str)
  {
    return captureStringBuffer(
      [str](auto* buf, auto len)
    {
      return ExpandEnvironmentStrings(str, buf, (DWORD)len);
    }
    );
  }

  PushEnvVar::PushEnvVar(const wstring& name, const wstring& value)
    : PushEnvVar(name.c_str(), value.c_str())
  {}

  PushEnvVar::PushEnvVar(const wchar_t* name, const wchar_t* value)
    : _name(name)
    , _previous(getEnvVar(name))
  {
    auto s = expandEnvironmentStrings(value);
    SetEnvironmentVariable(name, s.c_str());
  }

  PushEnvVar::~PushEnvVar()
  {
    pop();
  }

  void PushEnvVar::pop()
  {
    if (_name.empty())
      return;

    SetEnvironmentVariable(_name.c_str(), _previous.c_str());
    _name.clear();
    _previous.clear();
  }

  bool getWindowsRegistryValue(
    const std::wstring& hive, 
    const std::wstring& location,
    std::wstring& result)
  {
    HKEY root;
    if (hive == L"HKLM")
      root = HKEY_LOCAL_MACHINE;
    else if (hive == L"HKCU")
      root = HKEY_CURRENT_USER;
    else if (hive == L"HKCR")
      root = HKEY_CLASSES_ROOT;
    else
      return false;

    const auto lastSlash = location.rfind(L'\\');
    const auto subKey = location.substr(0, lastSlash);
    const auto value = lastSlash + 1 < location.size() 
      ? location.substr(lastSlash + 1) : wstring();
    wchar_t buffer[1024];
    DWORD bufSize = sizeof(buffer) / sizeof(wchar_t);
    if (ERROR_SUCCESS == RegGetValue(
      root,
      subKey.c_str(),
      value.c_str(),
      RRF_RT_REG_SZ,
      nullptr /*type not required*/,
      &buffer,
      &bufSize))
    {
      result = buffer;
      return true;
    }
    return false;
  }

  static wregex registryExpander(L"<(HK[A-Z][A-Z])\\\\([^>]*)>",
    std::regex_constants::optimize | std::regex_constants::ECMAScript);

  wstring expandWindowsRegistryStrings(const std::wstring& str)
  {
    wstring result;

    wsregex_iterator next(str.begin(), str.end(), registryExpander);
    wsregex_iterator end;
    wsmatch match;
    wstring regValue;
    if (next == end)
      return str;
    while (next != end)
    {
      match = *next;
      assert(match.size() == 3);
      result += match.prefix().str();
      if (getWindowsRegistryValue(match[1].str(), match[2].str(), regValue))
        result += regValue;
      next++;
    }
    result += match.suffix().str();
    
    return result;
  }
}