#ifndef PSX_MACOS_BUNDLE_STORAGE_H
#define PSX_MACOS_BUNDLE_STORAGE_H
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace psx_bundle {
namespace fs = std::filesystem;
inline std::string read(const fs::path &path) {
    std::ifstream file(path,std::ios::binary);
    if(!file)throw std::runtime_error("Cannot read application defaults: "+path.string());
    return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
}
/* Preserve player edits; update an untouched previous default on app upgrades. */
inline void seed(const fs::path &source,const fs::path &target,const fs::path &previous,bool preferences=false) {
    if(!fs::exists(source))return;
    if(!fs::exists(target) || (!preferences && fs::exists(previous) && read(target)==read(previous))) {
        fs::create_directories(target.parent_path());
        fs::copy_file(source,target,fs::copy_options::overwrite_existing);
    }
    fs::create_directories(previous.parent_path());
    fs::copy_file(source,previous,fs::copy_options::overwrite_existing);
}
inline void resource_link(const fs::path &source,const fs::path &target) {
    if(!fs::exists(source))return;
    if(fs::is_symlink(target)) {
        if(fs::read_symlink(target)==source)return;
        fs::remove(target);
    } else if(fs::exists(target)) {
        throw std::runtime_error("Application resource path is occupied: "+target.string());
    }
    fs::create_directories(target.parent_path());
    fs::create_directory_symlink(source,target);
}
/* Standalone developer binaries retain their portable layout. A signed bundle
 * owns immutable defaults in Resources/runtime; player state lives outside it. */
inline fs::path prepare(const fs::path &executable,const fs::path &home,const fs::path &override_root={}) {
    const auto macos=executable.parent_path();
    if(macos.filename()!="MacOS" || macos.parent_path().filename()!="Contents")return {};
    const auto resources=fs::weakly_canonical(macos.parent_path()/"Resources/runtime");
    if(!fs::exists(resources/"bundle-id"))return {};
    auto id=read(resources/"bundle-id");
    while(!id.empty() && (id.back()=='\n' || id.back()=='\r'))id.pop_back();
    if(id.empty() || id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-")!=std::string::npos || id=="." || id=="..")
        throw std::runtime_error("Invalid application storage identifier");
    const auto root=override_root.empty()?home/"Library/Application Support"/id:override_root;
    if(!root.is_absolute())throw std::runtime_error("Application storage must use an absolute path");
    fs::create_directories(root);
    for(const auto *name:{"assets","bios","postfx","mods/bundled"})
        resource_link(resources/name,root/name);
    for(const auto *name:{"game.toml","game_options.toml"})
        seed(resources/name,root/name,root/".defaults"/name);
    seed(resources/"mods/state.toml",root/"mods/state.toml",root/".defaults/mod-state.toml",true);
    return root;
}
}
#endif
