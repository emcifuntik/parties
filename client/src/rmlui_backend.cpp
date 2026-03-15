#include <client/rmlui_backend.h>

#include <cstdio>
#include <cstring>
#include <fstream>

namespace parties::client {

// ═══════════════════════════════════════════════════════════════════════
// Embedded UI resources via C++23 #embed
// ═══════════════════════════════════════════════════════════════════════

static constexpr unsigned char embed_lobby_rml[] = {
    #embed "../ui/lobby.rml"
};

static constexpr unsigned char embed_style_rcss[] = {
    #embed "../ui/style.rcss"
};

static constexpr unsigned char embed_logo_svg[] = {
    #embed "../ui/logo.svg"
};

static constexpr unsigned char embed_icon_unmute[] = {
    #embed "../ui/icon-unmute.svg"
};

static constexpr unsigned char embed_icon_mute[] = {
    #embed "../ui/icon-mute.svg"
};

static constexpr unsigned char embed_icon_undeafen[] = {
    #embed "../ui/icon-undeafen.svg"
};

static constexpr unsigned char embed_icon_deafen[] = {
    #embed "../ui/icon-deafen.svg"
};

static constexpr unsigned char embed_icon_share[] = {
    #embed "../ui/icon-share.svg"
};

static constexpr unsigned char embed_icon_sharing[] = {
    #embed "../ui/icon-sharing.svg"
};

static constexpr unsigned char embed_icon_leave[] = {
    #embed "../ui/icon-leave.svg"
};

static constexpr unsigned char embed_icon_settings[] = {
    #embed "../ui/icon-settings.svg"
};

static constexpr unsigned char embed_icon_volume[] = {
    #embed "../ui/icon-volume.svg"
};

static constexpr unsigned char embed_icon_disconnect[] = {
    #embed "../ui/icon-disconnect.svg"
};

static constexpr unsigned char embed_icon_room[] = {
    #embed "../ui/icon-room.svg"
};

static constexpr unsigned char embed_font_regular[] = {
    #embed "../ui/fonts/Inter-Regular.ttf"
};

static constexpr unsigned char embed_font_medium[] = {
    #embed "../ui/fonts/Inter-Medium.ttf"
};

static constexpr unsigned char embed_font_bold[] = {
    #embed "../ui/fonts/Inter-Bold.ttf"
};

namespace {

struct OpenFile {
    enum Type { MEMORY, FILESYSTEM };
    Type type;
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t pos = 0;
    std::ifstream* fp = nullptr;
};

} // anonymous namespace

EmbeddedFileInterface::EmbeddedFileInterface() {
    entries_["ui/lobby.rml"] = { embed_lobby_rml, sizeof(embed_lobby_rml) };
    entries_["ui/style.rcss"] = { embed_style_rcss, sizeof(embed_style_rcss) };
    entries_["ui/logo.svg"] = { embed_logo_svg, sizeof(embed_logo_svg) };
    entries_["ui/icon-unmute.svg"] = { embed_icon_unmute, sizeof(embed_icon_unmute) };
    entries_["ui/icon-mute.svg"] = { embed_icon_mute, sizeof(embed_icon_mute) };
    entries_["ui/icon-undeafen.svg"] = { embed_icon_undeafen, sizeof(embed_icon_undeafen) };
    entries_["ui/icon-deafen.svg"] = { embed_icon_deafen, sizeof(embed_icon_deafen) };
    entries_["ui/icon-share.svg"] = { embed_icon_share, sizeof(embed_icon_share) };
    entries_["ui/icon-sharing.svg"] = { embed_icon_sharing, sizeof(embed_icon_sharing) };
    entries_["ui/icon-leave.svg"] = { embed_icon_leave, sizeof(embed_icon_leave) };
    entries_["ui/icon-settings.svg"] = { embed_icon_settings, sizeof(embed_icon_settings) };
    entries_["ui/icon-volume.svg"] = { embed_icon_volume, sizeof(embed_icon_volume) };
    entries_["ui/icon-disconnect.svg"] = { embed_icon_disconnect, sizeof(embed_icon_disconnect) };
    entries_["ui/icon-room.svg"] = { embed_icon_room, sizeof(embed_icon_room) };
    entries_["ui/fonts/Inter-Regular.ttf"] = { embed_font_regular, sizeof(embed_font_regular) };
    entries_["ui/fonts/Inter-Medium.ttf"] = { embed_font_medium, sizeof(embed_font_medium) };
    entries_["ui/fonts/Inter-Bold.ttf"] = { embed_font_bold, sizeof(embed_font_bold) };
    std::printf("[UI] Embedded resources: %zu files\n", entries_.size());
}

Rml::FileHandle EmbeddedFileInterface::Open(const Rml::String& path) {
    std::string normalized = path;
    for (char& c : normalized) {
        if (c == '\\') c = '/';
    }
    while (normalized.size() >= 2 && normalized[0] == '.' && normalized[1] == '/')
        normalized = normalized.substr(2);

    auto it = entries_.find(normalized);
    if (it != entries_.end()) {
        auto* f = new OpenFile{};
        f->type = OpenFile::MEMORY;
        f->data = it->second.data;
        f->size = it->second.size;
        return reinterpret_cast<Rml::FileHandle>(f);
    }

    auto* fs = new std::ifstream(path, std::ios::binary);
    if (!fs->is_open()) {
        delete fs;
        return 0;
    }
    auto* f = new OpenFile{};
    f->type = OpenFile::FILESYSTEM;
    f->fp = fs;
    return reinterpret_cast<Rml::FileHandle>(f);
}

void EmbeddedFileInterface::Close(Rml::FileHandle file) {
    auto* f = reinterpret_cast<OpenFile*>(file);
    delete f->fp;
    delete f;
}

size_t EmbeddedFileInterface::Read(void* buffer, size_t size, Rml::FileHandle file) {
    auto* f = reinterpret_cast<OpenFile*>(file);
    if (f->type == OpenFile::FILESYSTEM) {
        f->fp->read(static_cast<char*>(buffer), size);
        return static_cast<size_t>(f->fp->gcount());
    }
    size_t avail = f->size - f->pos;
    size_t to_read = (size < avail) ? size : avail;
    std::memcpy(buffer, f->data + f->pos, to_read);
    f->pos += to_read;
    return to_read;
}

bool EmbeddedFileInterface::Seek(Rml::FileHandle file, long offset, int origin) {
    auto* f = reinterpret_cast<OpenFile*>(file);
    if (f->type == OpenFile::FILESYSTEM) {
        std::ios_base::seekdir dir;
        switch (origin) {
            case SEEK_SET: dir = std::ios::beg; break;
            case SEEK_CUR: dir = std::ios::cur; break;
            case SEEK_END: dir = std::ios::end; break;
            default: return false;
        }
        f->fp->seekg(offset, dir);
        return !f->fp->fail();
    }
    long long new_pos;
    switch (origin) {
        case SEEK_SET: new_pos = offset; break;
        case SEEK_CUR: new_pos = static_cast<long long>(f->pos) + offset; break;
        case SEEK_END: new_pos = static_cast<long long>(f->size) + offset; break;
        default: return false;
    }
    if (new_pos < 0 || static_cast<size_t>(new_pos) > f->size) return false;
    f->pos = static_cast<size_t>(new_pos);
    return true;
}

size_t EmbeddedFileInterface::Tell(Rml::FileHandle file) {
    auto* f = reinterpret_cast<OpenFile*>(file);
    if (f->type == OpenFile::FILESYSTEM)
        return static_cast<size_t>(f->fp->tellg());
    return f->pos;
}

} // namespace parties::client
