#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>

namespace padsplit::app {

enum class UiLang {
    En = 0,
    Zh = 1
};

inline UiLang DetectSystemLang() {
    LANGID lid = GetUserDefaultUILanguage();
    return (PRIMARYLANGID(lid) == LANG_CHINESE) ? UiLang::Zh : UiLang::En;
}

inline UiLang ResolveUiLang(const std::string& pref) {
    if (pref == "zh") return UiLang::Zh;
    if (pref == "en") return UiLang::En;
    return DetectSystemLang();
}

inline const char* Tr(UiLang lang, const char* en, const char* zh) {
    return (lang == UiLang::Zh) ? zh : en;
}

inline const wchar_t* TrW(UiLang lang, const wchar_t* en, const wchar_t* zh) {
    return (lang == UiLang::Zh) ? zh : en;
}

} // namespace padsplit::app
