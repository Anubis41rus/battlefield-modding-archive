#include "../src/GameAssetRoutes.h"
#include <cstdio>
int main() {
    int failed = 0, passed = 0;
    const char* valid[] = {"__fb/UI/Art/Persistence/Weapons/Fancy/M16A4.png",
        "__fb/ui/art/persistence/weapons/fancy/m67%20grenade.png", "__fb/ui/art/persistence/ranks/rank001.png"};
    const char* invalid[] = {"__fb/../../EVBots.dll", "__fb/C:/secret.png", "__fb/ui/art/persistence/weapons/../other.png",
        "__fb/ui/art/persistence/weapons/%2e%2e/other.png", "__fb/ui/art/persistence/weapons/%252e%252e/other.png",
        "__fb/ui/art/persistence/weapons/thing.png:secret", "__fb/ui/art/persistence/weapons/thing.png?x",
        "__fb/ui/art/persistence/weapons/thing%00.png", "__fb/ui/art/persistence/weapons/x\\other.png",
        "__fb/ui/art/persistence/weapons//thing.png", "__fb/ui/art/persistence/weapons/ thing.png",
        "__fb/ui/art/persistence/weapons/thing .png", "__fb/ui/art/persistence/weapons/.png",
        "__fb/ui/art/persistence/secret.png", "__fb/ui/art/persistence/ranks/rank004.png"};
    for (const char* route : valid) { std::wstring path; if (evbots::GameAssetRelativePngPath(route, &path)) ++passed; else { ++failed; printf("FAIL %s\n", route); } }
    for (const char* route : invalid) { std::wstring path; if (!evbots::GameAssetRelativePngPath(route, &path)) ++passed; else { ++failed; printf("FAIL %s\n", route); } }
    std::wstring path;
    evbots::GameAssetRelativePngPath(valid[1], &path);
    if (path == L"ui\\art\\persistence\\weapons\\fancy\\m67 grenade.png") ++passed; else ++failed;
    printf("RESULT pass=%d fail=%d\n", passed, failed);
    return failed ? 1 : 0;
}
