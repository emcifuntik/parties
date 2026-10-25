#pragma once

#include <RmlUi/Core/FileInterface.h>

#include <cstdint>
#include <string>
#include <unordered_map>

namespace parties::client {

class EmbeddedFileInterface : public Rml::FileInterface {
public:
    EmbeddedFileInterface();

    Rml::FileHandle Open(const Rml::String& path) override;
    void Close(Rml::FileHandle file) override;
    size_t Read(void* buffer, size_t size, Rml::FileHandle file) override;
    bool Seek(Rml::FileHandle file, long offset, int origin) override;
    size_t Tell(Rml::FileHandle file) override;

    struct Entry {
        const uint8_t* data;
        size_t size;
    };

    size_t num_entries() const { return entries_.size(); }

private:
    std::unordered_map<std::string, Entry> entries_;
};

}
