// Изолированный тест выбора модов: без DLL, Lua, игровых файлов и инициализаторов.
#include "ModSelection.h"

#include <cstdio>

using namespace evbots::modselection;

namespace {
int passed = 0, failed = 0;
void Check(bool ok, const char* label) {
    if (ok) ++passed;
    else { ++failed; std::printf("FAIL %s\n", label); }
}

Candidate Mod(const wchar_t* folder, const char* version, const char* dependencies = "{}") {
    Candidate c;
    c.folderName = folder; c.hasDescriptor = true;
    c.descriptor = std::string("{\"Name\":\"Display name\",\"Version\":\"") + version +
        "\",\"HasVeniceEXT\":true,\"Dependencies\":" + dependencies + "}";
    return c;
}

std::vector<std::wstring> Names(const std::vector<SelectedMod>& mods) {
    std::vector<std::wstring> result;
    for (const auto& mod : mods) result.push_back(mod.folderName);
    return result;
}

bool MatchesText(const char* version, const char* expression) {
    Version v; Requirement r;
    return ParseVersion(version, &v) && ParseRequirement(expression, &r) && Matches(v, r);
}

void Reject(const std::vector<Candidate>& candidates, const std::wstring& selection, const char* label,
            const char* expected = "", bool defined = true) {
    std::vector<SelectedMod> out(1);  // Ошибка обязана очистить даже прошлый результат.
    std::string error;
    const bool ok = Resolve(candidates, defined, selection, "1.12.0", &out, &error);
    Check(!ok && out.empty() && !error.empty() && error.find(expected) != std::string::npos, label);
}
}  // namespace

int main() {
    const std::vector<Candidate> original = {
        Mod(L"fun-bots", "2.9.0", R"({"veniceext":">=1.0.0"})"),
        Mod(L"VU-BattleRoyale", "1.0.0", R"({"veniceext":"^1.4.0","VU-BattleRoyale-Compass":"^1.0.0","VEManager":"^0.2.0-rc.1"})"),
        Mod(L"VEManager", "0.2.0-rc.1", R"({"veniceext":"^1.0.0"})"),
        Mod(L"VU-BattleRoyale-Compass", "1.0.0", R"({"veniceext":"^1.0.0"})")
    };
    std::vector<SelectedMod> out;
    std::string error;
    const std::vector<std::wstring> royale = {L"VU-BattleRoyale-Compass", L"VEManager", L"VU-BattleRoyale"};
    Check(Resolve(original, true, L"VU-BattleRoyale-Compass;VEManager;VU-BattleRoyale", "1.12.0", &out, &error) &&
          Names(out) == royale, "explicit original profile excludes fun-bots");
    Check(Resolve(original, true, L"VU-BattleRoyale", "1.12.0", &out, &error) && Names(out) == royale,
          "transitive dependencies before original Royale");
    Check(Resolve(original, true, L" vu-battleroyale ; VU-BATTLEROYALE ", "1.12.0", &out, &error) && Names(out) == royale,
          "case-insensitive names, trimming, duplicate roots, canonical output");
    Check(Resolve(original, true, L"fun-bots", "1.12.0", &out, &error) && Names(out) == std::vector<std::wstring>{L"fun-bots"},
          "select fun-bots only");
    Check(Resolve(original, false, L"", "1.12.0", &out, &error) && out.size() == 4,
          "undefined selection preserves load-all");
    Check(Resolve(original, true, L"@none", "1.12.0", &out, &error) && out.empty(), "explicit empty set");
    Check(Resolve({}, true, L" @NONE ", "1.12.0", &out, &error) && out.empty(), "empty set needs no mod root");
    Check(Resolve({}, false, L"", "1.12.0", &out, &error) && out.empty(), "legacy empty mod root");
    Reject(original, L"absent", "missing selected folder", "absent");
    Reject(original, L"Display name", "display name is not folder identity");
    Reject(original, L"", "defined empty selection is not load-all");
    Reject(original, L"fun-bots;", "empty final name is invalid");
    Reject(original, L"fun-bots;;VEManager", "empty middle name is invalid");
    Reject(original, L"@none;fun-bots", "none cannot mix with folders");
    Reject(original, L"veniceext", "built-in cannot be selected as folder");
    for (const wchar_t* invalid : {L"..", L".", L"../fun-bots", L"C:\\fun-bots", L"a/b", L"a\\b", L"a:b",
                                   L"a*", L"a?", L"a|b", L"a\"b", L"a.", L"CON", L"con.json", L"LPT1", L"COM9.mod"}) {
        Reject(original, invalid, "invalid or reserved folder name");
    }
    Reject(original, std::wstring(256, L'a'), "overlong folder name");
    Reject(original, std::wstring(L"abc\0def", 7), "NUL in folder name");

    auto candidates = original;
    Candidate broken = Mod(L"broken", "1.0.0"); broken.descriptor = "{";
    candidates.push_back(broken);
    Check(Resolve(candidates, true, L"fun-bots", "1.12.0", &out, &error) && out.size() == 1,
          "unselected malformed descriptor ignored");
    Reject(candidates, L"broken", "selected malformed descriptor is fatal");
    Reject(candidates, L"", "load-all malformed descriptor is fatal", "broken", false);
    broken.descriptor = "[]"; Reject({broken}, L"broken", "descriptor must be object");
    for (const char* number : {"+1", "01", "1.", ".1", "1e", "1e+", "1.2.3", "--1", "e", "E"}) {
        broken.descriptor = std::string("{\"Version\":\"1.0.0\",\"Extra\":") + number + "}";
        Reject({broken}, L"broken", "invalid JSON numeric token rejected");
    }
    broken.descriptor = "{\"Version\":\"1.0.0\",\"Description\":\"raw\nnewline\"}";
    Reject({broken}, L"broken", "raw newline in JSON string rejected");
    broken.descriptor = "{\"Version\":\"1.0.0\",\"Description\":\"\xff\"}";
    Reject({broken}, L"broken", "invalid UTF-8 descriptor rejected");
    broken.descriptor = R"({"Version":"1.0.0","Description":"\uD800"})";
    Reject({broken}, L"broken", "unpaired surrogate escape rejected");
    broken.descriptor = R"({"Version":"1.0.0","Description":"\uD83D\uDE00\n","Extra":-1.25e+3})";
    Check(Resolve({broken}, true, L"broken", "1.12.0", &out, &error), "valid Unicode escapes and JSON number");
    broken.descriptor = "{\"Version\":\"1.0.0\",\"Version\":\"2.0.0\"}";
    Reject({broken}, L"broken", "duplicate metadata rejected");
    broken.descriptor = "{}"; Reject({broken}, L"broken", "version required");
    broken = Mod(L"broken", "01.0.0"); Reject({broken}, L"broken", "invalid selected version");
    broken.hasDescriptor = false; Reject({broken}, L"broken", "selected folder without descriptor");
    Check(Resolve({broken}, false, L"", "1.12.0", &out, &error) && out.empty(), "load-all ignores non-mod directories");
    broken.hasDescriptor = true; broken.readError = "access denied";
    Reject({broken}, L"broken", "descriptor read failure includes detail", "access denied");

    Reject({Mod(L"a", "1.0.0", R"({"missing":"^1.0.0"})")}, L"a", "missing dependency", "missing");
    Reject({Mod(L"a", "1.0.0", R"({"../escape":"1.0.0"})")}, L"a", "invalid dependency name");
    Reject({Mod(L"a", "1.0.0", "[]")}, L"a", "invalid dependency object");
    Reject({Mod(L"a", "1.0.0", R"({"b":7})")}, L"a", "dependency expression must be string");
    Reject({Mod(L"a", "1.0.0", R"({"b":"*"})")}, L"a", "unsupported wildcard rejected");
    Reject({Mod(L"a", "1.0.0", R"({"b":"1.0.0","B":"1.0.0"})")}, L"a", "duplicate case-folded dependency rejected");
    Reject({Mod(L"a", "1.0.0"), Mod(L"A", "1.0.0")}, L"a", "ambiguous directory rejected");
    Reject({Mod(L"a", "1.0.0", R"({"a":"^1.0.0"})")}, L"a", "self cycle", "a -> a");
    Reject({Mod(L"a", "1.0.0", R"({"b":"1.0.0"})"), Mod(L"b", "1.0.0", R"({"a":"1.0.0"})")}, L"a",
           "dependency cycle", "a -> b -> a");
    candidates = {Mod(L"a", "1.0.0", R"({"b":"1.0.0","c":"1.0.0"})"),
                  Mod(L"b", "1.0.0", R"({"d":"1.0.0"})"), Mod(L"c", "1.0.0", R"({"d":"1.0.0"})"), Mod(L"d", "1.0.0")};
    Check(Resolve(candidates, true, L"a", "1.12.0", &out, &error) &&
          Names(out) == std::vector<std::wstring>{L"d", L"b", L"c", L"a"}, "diamond dependency loads once in order");
    candidates[2] = Mod(L"c", "1.0.0", R"({"d":"2.0.0"})");
    Reject(candidates, L"a", "already visited dependency rechecked against every requirement", "2.0.0");
    Reject({Mod(L"a", "1.0.0", R"({"veniceext":"^2.0.0"})")}, L"a", "built-in version mismatch", "1.12.0");
    Check(Resolve({Mod(L"a", "1.0.0", R"({"veniceext":"1.12.0"})")}, true, L"a", "1.12.0", &out, &error) &&
          out.size() == 1 && out[0].dependsVeniceExt == "1.12.0", "built-in exact version");
    Check(Resolve({Mod(L"Мод", "1.0.0")}, true, L"мод", "1.12.0", &out, &error) && out[0].folderName == L"Мод",
          "Windows ordinal Unicode case-insensitive identity");

    Version v;
    for (const char* invalid : {"", "1", "1.0", "01.0.0", "1.00.0", "1.0.00", "-1.0.0", "v1.0.0", "1.0.0 ",
                                "1.0.0-", "1.0.0-01", "1.0.0-alpha..1", "1.0.0+", "1.0.0+a+b", "1.0.0-a_b"}) {
        Check(!ParseVersion(invalid, &v), "invalid SemVer rejected");
    }
    Check(ParseVersion("999999999999999999999999999.0.0-alpha.10+build.01", &v), "SemVer numbers cannot overflow integers");
    const char* precedence[] = {"1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta", "1.0.0-beta.2",
                                "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0"};
    for (std::size_t i = 1; i < sizeof(precedence) / sizeof(precedence[0]); ++i) {
        Version a, b;
        Check(ParseVersion(precedence[i - 1], &a) && ParseVersion(precedence[i], &b) && CompareVersion(a, b) < 0,
              "SemVer prerelease precedence");
    }
    Check(MatchesText("1.2.3+other", "1.2.3+build"), "build metadata does not affect precedence");
    Check(!MatchesText("1.2.3-rc.1", "1.2.3"), "exact stable excludes prerelease");
    Check(MatchesText("1.12.0", "^1.4.0") && !MatchesText("1.3.99", "^1.4.0") && !MatchesText("2.0.0", "^1.4.0"),
          "caret respects lower bound and major");
    Check(MatchesText("0.9.0", "^0.2.0-rc.1") && MatchesText("0.2.0-rc.2", "^0.2.0-rc.1") &&
          !MatchesText("0.2.0-rc.0", "^0.2.0-rc.1"), "VU documented caret major-zero and rc versions");
    Check(MatchesText("1.2.9", "~1.2.3") && !MatchesText("1.3.0", "~1.2.3") && !MatchesText("1.2.2", "~1.2.3"),
          "tilde respects lower bound and minor");
    Check(MatchesText("2.0.0", ">1.2.3") && !MatchesText("1.2.3", ">1.2.3") && MatchesText("1.2.3", ">=1.2.3"),
          "greater and greater-equal");
    Check(!MatchesText("1.0.0", "1.x") && !MatchesText("1.0.0", "<=2.0.0") && !MatchesText("1.0.0", "1.0.0 || 2.0.0"),
          "undocumented expressions fail closed");
    std::printf("ModSelection RESULT pass=%d fail=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
