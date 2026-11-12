#include "macos_bundle_storage.h"
#include <cassert>
#include <chrono>
static void write(const psx_bundle::fs::path &p,const char *text) {
    psx_bundle::fs::create_directories(p.parent_path());std::ofstream(p)<<text;
}
int main() {
    namespace fs=std::filesystem;
    auto base=fs::temp_directory_path()/("psx-bundle-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto exe=base/"R4.app/Contents/MacOS/r4-runtime";
    auto resources=base/"R4.app/Contents/Resources/runtime";
    auto home=base/"home";
    write(exe,"executable");write(resources/"bundle-id","net.example.test\n");
    write(resources/"game.toml","default-v1");write(resources/"game_options.toml","options-v1");
    write(resources/"mods/state.toml","enabled=true");write(resources/"assets/fonts/font","font");
    auto root=psx_bundle::prepare(exe,home);
    assert(root==home/"Library/Application Support/net.example.test");
    assert(psx_bundle::read(root/"game.toml")=="default-v1");
    assert(fs::is_symlink(root/"assets"));
    write(root/"mods/state.toml","player preference");
    write(root/"game_options.toml","player options");
    write(resources/"game.toml","default-v2");write(resources/"game_options.toml","options-v2");
    psx_bundle::prepare(exe,home);
    assert(psx_bundle::read(root/"game.toml")=="default-v2");
    assert(psx_bundle::read(root/"game_options.toml")=="player options");
    assert(psx_bundle::read(root/"mods/state.toml")=="player preference");
    assert(psx_bundle::read(resources/"mods/state.toml")=="enabled=true");
    auto moved=base/"Moved.app";fs::rename(base/"R4.app",moved);
    psx_bundle::prepare(moved/"Contents/MacOS/r4-runtime",home);
    assert(psx_bundle::read(root/"assets/fonts/font")=="font");
    assert(psx_bundle::prepare(base/"standalone/r4-runtime",home).empty());
    auto isolated=base/"isolated";
    assert(psx_bundle::prepare(moved/"Contents/MacOS/r4-runtime",home,isolated)==isolated);
    bool rejected=false;try{psx_bundle::prepare(moved/"Contents/MacOS/r4-runtime",home,"relative");}catch(...){rejected=true;}
    assert(rejected);
    fs::remove_all(base);
}
