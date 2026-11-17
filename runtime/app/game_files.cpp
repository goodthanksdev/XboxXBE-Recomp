// The game in assets: its files, or a disc image (.iso) with this executable's xbe on it. The xbe must be the one
// this executable was recompiled from; another release's would run its own data under this code.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <string_view>

#include "app/app.h"
#include "core/log.h"
#include "image_symbols.h"
#include "kernel/crypto.h"
#include "kernel/disc.h"

extern const char kImageSha1[];  // the xbe recompiled into this executable, written by CMake

namespace app {

namespace {

namespace fs = std::filesystem;

// assets beside the exe or in any folder above it (the build output)
fs::path FindAssets() {
    auto dir = ExeDir();
    std::error_code ec;
    for (int i = 0; i < 6; i++) {
        if (fs::is_directory(dir / "assets", ec)) return dir / "assets";
        if (!dir.has_parent_path() || dir.parent_path() == dir) break;
        dir = dir.parent_path();
    }
    return {};
}

// the xbe this executable was recompiled from, or another release of it with its own config
bool Runs(const std::string& sha) {
    if (sha == kImageSha1) return true;
    for (auto& [xbe, release] : kGame.releases)
        if (std::string_view(xbe) == IMAGE_XBE && sha == release) return true;
    return false;
}

bool ReadHostFile(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

}  // namespace

std::string Sha1(const std::vector<uint8_t>& data) {
    alignas(8) uint8_t ctx[128];
    uint8_t digest[20];
    crypto::ShaInit(ctx);
    crypto::ShaUpdate(ctx, data.data(), data.size());
    crypto::ShaFinal(ctx, digest);
    std::string hex;
    for (uint8_t b : digest) {
        hex += "0123456789abcdef"[b >> 4];
        hex += "0123456789abcdef"[b & 15];
    }
    return hex;
}

bool FindGame(GameFiles& game, bool ask, GameFound* found) {
    // where to look: the disc image or folder picked in the setup, then assets
    std::vector<fs::path> dirs, isos;
    std::error_code ec;
    auto is_iso = [](const fs::path& p) {
        std::wstring ext = p.extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
        return ext == L".iso";
    };
    if (!Get().game_path.empty()) {
        fs::path chosen = fs::u8path(Get().game_path);
        if (fs::is_directory(chosen, ec)) dirs.push_back(chosen);
        else if (is_iso(chosen) && fs::is_regular_file(chosen, ec)) isos.push_back(chosen);
    }
    if (fs::path assets = FindAssets(); !assets.empty()) dirs.push_back(assets);
    for (const fs::path& dir : dirs) {
        std::vector<fs::path> here;
        for (auto& e : fs::directory_iterator(dir, ec))
            if (is_iso(e.path()) && e.is_regular_file(ec)) here.push_back(e.path());
        std::sort(here.begin(), here.end());
        isos.insert(isos.end(), here.begin(), here.end());
    }

    // the first copy with this xbe wins, the extracted files before any image; another release is kept to offer
    GameFiles other;
    std::unique_ptr<kernel::Disc> other_disc;
    fs::path other_name;
    auto consider = [&](const fs::path& dir, const fs::path& name, std::vector<uint8_t> xbe,
                        std::unique_ptr<kernel::Disc> disc) {
        std::string sha = Sha1(xbe);
        bool match = Runs(sha);
        LOGR("Game: %s in %s, sha1 %s%s", IMAGE_XBE, name.string().c_str(), sha.c_str(), match ? "" : ", another release");
        if (match) {
            game = {dir, std::move(xbe)};
            kernel::MountDisc(std::move(disc));
            if (found) *found = {name, false};
            return true;
        }
        if (other_name.empty()) {
            other = {dir, std::move(xbe)};
            other_disc = std::move(disc);
            other_name = name;
        }
        return false;
    };

    std::vector<uint8_t> xbe;
    for (const fs::path& dir : dirs)
        if (ReadHostFile(dir / IMAGE_XBE, xbe) && consider(dir, dir, std::move(xbe), nullptr)) return true;
    for (const fs::path& iso : isos) {
        auto disc = std::make_unique<kernel::Disc>();
        std::string error;
        if (!disc->Open(iso, error)) {
            LOGW("game: %s: %s", iso.string().c_str(), error.c_str());
            continue;
        }
        if (!disc->Load(IMAGE_XBE, xbe)) {
            LOGW("game: %s has no %s", iso.string().c_str(), IMAGE_XBE);
            continue;
        }
        if (consider(iso.parent_path(), iso, std::move(xbe), std::move(disc))) return true;
    }

    if (!ask) {
        if (found) *found = {other_name, !other_name.empty()};
        return false;
    }
    if (other_name.empty()) {
        MessageBoxW(nullptr, kGame.missing, kGame.short_title, MB_ICONERROR);
        return false;
    }
    std::wstring text = other_name.filename().wstring() +
                        L" is a different release of the game than this build was made from, and will most likely "
                        L"crash.\n\nStart it anyway?";
    if (MessageBoxW(nullptr, text.c_str(), kGame.short_title, MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES) return false;
    game = std::move(other);
    kernel::MountDisc(std::move(other_disc));
    return true;
}

}  // namespace app
