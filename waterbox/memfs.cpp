#include "memfs.h"

#include <common/cvt.h>
#include <common/path.h>
#include <common/wildcard.h>


#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace chimera {
    namespace {
        std::string lowered(const std::string &s) {
            std::string out = s;
            for (char &c : out) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return out;
        }

        // Splits a Symbian path into its drive letter and its components.
        // Either separator is accepted: the emulator builds paths with both.
        bool split_path(const std::string &path, char &drive_letter, std::vector<std::string> &parts) {
            parts.clear();
            drive_letter = '\0';

            std::size_t at = 0;

            if ((path.size() >= 2) && (path[1] == ':')) {
                drive_letter = static_cast<char>(std::tolower(static_cast<unsigned char>(path[0])));
                at = 2;
            }

            std::string current;

            for (; at < path.size(); at++) {
                const char c = path[at];

                if ((c == '\\') || (c == '/')) {
                    if (!current.empty()) {
                        parts.push_back(current);
                        current.clear();
                    }
                } else {
                    current.push_back(c);
                }
            }

            if (!current.empty()) {
                parts.push_back(current);
            }

            return drive_letter != '\0';
        }

        // EKA2 moved two directories, and the emulator rewrites the guest's
        // path rather than the tree. A filesystem that serves those paths has
        // to know the same trick or the machine looks in the wrong place.
        std::string rewrite_for_eka2(const std::string &path) {
            if (path.size() < 14) {
                return path;
            }

            auto starts = [&path](const char *what) {
                const std::size_t length = std::strlen(what);

                if (path.size() < 2 + length) {
                    return false;
                }

                for (std::size_t i = 0; i < length; i++) {
                    const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(path[2 + i])));
                    const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(what[i])));

                    if ((a == b) || ((a == '/') && (b == '\\')) || ((a == '\\') && (b == '/'))) {
                        continue;
                    }

                    return false;
                }

                return true;
            };

            if (starts("\\system\\libs")) {
                return path.substr(0, 2) + "\\sys\\bin" + path.substr(14);
            }

            if (starts("\\system\\programs")) {
                return path.substr(0, 2) + "\\sys\\bin" + path.substr(18);
            }

            return path;
        }

        std::size_t count_entries(const memory_node &node) {
            std::size_t total = 1;

            for (const auto &child : node.children) {
                total += count_entries(child.second);
            }

            return total;
        }

        std::size_t count_written(const memory_node &node) {
            std::size_t total = node.bytes.size();

            for (const auto &child : node.children) {
                total += count_written(child.second);
            }

            return total;
        }
    }

    // ---- one file ----------------------------------------------------------

    class memory_file : public eka2l1::file {
    public:
        memory_file(memory_node *node, const std::u16string &name, const int mode)
            : eka2l1::file(0)
            , node_(node)
            , name_(name)
            , mode_(mode)
            , position_(0) {
            type = eka2l1::io_component_type::file;

            if (mode_ & APPEND_MODE) {
                position_ = size();
            }
        }

        ~memory_file() override {
            close();
        }

        std::size_t write_file(const void *data, std::uint32_t size, std::uint32_t count) override {
            if ((!(mode_ & WRITE_MODE) && !(mode_ & APPEND_MODE)) || node_->fixed) {
                return 0;
            }

            const std::size_t total = static_cast<std::size_t>(size) * count;


            if (node_->bytes.size() < position_ + total) {
                node_->bytes.resize(position_ + total);
            }

            std::memcpy(node_->bytes.data() + position_, data, total);
            position_ += total;

            // Bytes, not elements: ro_file_stream and every other caller reads
            // the answer as a byte count.
            return total;
        }

        std::size_t read_file(void *data, std::uint32_t size, std::uint32_t count) override {
            const std::uint64_t left = this->size() - std::min(position_, this->size());
            std::uint64_t want = static_cast<std::uint64_t>(size) * count;

            if (want > left) {
                want = left;
            }

            if (want == 0) {
                return 0;
            }


            const std::uint8_t *from = node_->fixed ? node_->fixed : node_->bytes.data();
            std::memcpy(data, from + position_, static_cast<std::size_t>(want));
            position_ += want;


            return static_cast<std::size_t>(want);
        }

        int file_mode() const override {
            return mode_;
        }

        std::u16string file_name() const override {
            return name_;
        }

        std::uint64_t size() const override {
            return node_->fixed ? node_->fixed_size : node_->bytes.size();
        }

        std::uint64_t seek(std::int64_t offset, eka2l1::file_seek_mode where) override {

            switch (where) {
            case eka2l1::file_seek_mode::beg:
                position_ = static_cast<std::uint64_t>(offset);
                break;

            case eka2l1::file_seek_mode::crr:
                position_ = static_cast<std::uint64_t>(static_cast<std::int64_t>(position_) + offset);
                break;

            case eka2l1::file_seek_mode::end:
                position_ = static_cast<std::uint64_t>(static_cast<std::int64_t>(size()) + offset);
                break;

            default:
                // "address" asks where the file lives in the machine's memory,
                // so that a caller can read it in place. Nothing here does: the
                // drives are the machine's own memory, not the ROM.
                //
                // The answer to that is REFUSAL, and it has to be the sixty-four
                // bit all-ones the file server checks for. Anything else is an
                // address as far as the file server is concerned, and it hands
                // it to the guest to read: answering the file's size sent one
                // caller looking for an icon at an address that was not one, and
                // answering 0xFFFFFFFF killed an application that asked whether
                // its own resource file could be read in place - it read from
                // 0xFFFFFFFF and took a KERN-EXEC 3 for it.
                position_ = static_cast<std::uint64_t>(offset);
                return 0xFFFFFFFFFFFFFFFF;
            }


            return position_;
        }

        std::uint64_t tell() override {
            return position_;
        }

        bool close() override {
            return true;
        }

        std::string get_error_descriptor() override {
            return "no error";
        }

        bool is_in_rom() const override {
            // Nothing here is: the ROM serves its own files.
            return false;
        }

        address rom_address() const override {
            return 0;
        }

        bool resize(const std::size_t new_size) override {
            if (node_->fixed) {
                return false;
            }

            node_->bytes.resize(new_size);
            return true;
        }

        bool valid() override {
            return position_ < size();
        }

        std::uint64_t last_modify_since_0ad() override {
            // The machine's files have no history: a timestamp read from the
            // host would be the one thing here that changed between runs.
            return 0;
        }

    private:
        memory_node *node_;
        std::u16string name_;
        int mode_;
        std::uint64_t position_;
    };

    // ---- one directory -----------------------------------------------------

    class memory_directory : public eka2l1::directory {
    public:
        memory_directory(memory_file_system *fs, memory_node *node, const std::string &virtual_path,
            const std::string &filter, eka2l1::epoc::uid_type type, const std::uint32_t attrib)
            : eka2l1::directory(attrib)
            , fs_(fs)
            , virtual_path_(virtual_path)
            , filter_(filter)
            , uid_(type) {
            this->type = eka2l1::io_component_type::dir;

            for (auto &child : node->children) {
                names_.push_back(child.second.name);
            }
        }

        std::optional<eka2l1::entry_info> get_next_entry() override {
            if (peeking_) {
                peeking_ = false;
                return peeked_;
            }

            while (at_ < names_.size()) {
                const std::string name = names_[at_++];

                if (!eka2l1::common::full_wildcard_match<char>(lowered(name), lowered(filter_), false)) {
                    continue;
                }

                const std::string full = eka2l1::add_path(virtual_path_, name);
                memory_node *node = fs_->resolve(full);

                if (!node) {
                    continue;
                }

                if (attribute != io_attrib_none) {
                    if (!(attribute & io_attrib_include_dir) && node->is_dir) {
                        continue;
                    }

                    if (!(attribute & io_attrib_include_file) && !node->is_dir) {
                        continue;
                    }
                }

                std::optional<eka2l1::entry_info> info = fs_->get_entry_info(eka2l1::common::utf8_to_ucs2(full));

                if (!info.has_value()) {
                    continue;
                }

                if ((attribute & io_attrib_include_file) && (attribute & io_attrib_allow_uid) && !node->is_dir) {
                    eka2l1::epoc::uid_type found;

                    std::unique_ptr<eka2l1::file> handle = fs_->open_file(eka2l1::common::utf8_to_ucs2(full),
                        READ_MODE | BIN_MODE);

                    if (!handle || (handle->read_file(&found, 1, sizeof(found)) != sizeof(found))) {
                        continue;
                    }

                    if (((uid_.uid1 != 0) && (uid_.uid1 != found.uid1))
                        || ((uid_.uid2 != 0) && (uid_.uid2 != found.uid2))
                        || ((uid_.uid3 != 0) && (uid_.uid3 != found.uid3))) {
                        continue;
                    }
                }

                return info;
            }

            return std::nullopt;
        }

        std::optional<eka2l1::entry_info> peek_next_entry() override {
            if (!peeking_) {
                peeked_ = get_next_entry();
                peeking_ = true;
            }

            return peeked_;
        }

    private:
        memory_file_system *fs_;
        std::string virtual_path_;
        std::string filter_;
        eka2l1::epoc::uid_type uid_;

        std::vector<std::string> names_;
        std::size_t at_ = 0;

        bool peeking_ = false;
        std::optional<eka2l1::entry_info> peeked_;
    };

    // ---- the filesystem ----------------------------------------------------

    memory_file_system::memory_file_system() {
        for (auto &mapping : mappings_) {
            mapping.second = false;
        }

        for (auto &root : roots_) {
            root.is_dir = true;
        }
    }

    memory_file_system::~memory_file_system() = default;

    memory_node *memory_file_system::resolve(const std::string &raw) {
        const std::string path = (static_cast<int>(version_) >= static_cast<int>(epocver::eka2))
            ? rewrite_for_eka2(raw)
            : raw;

        char letter = '\0';
        std::vector<std::string> parts;

        if (!split_path(path, letter, parts)) {
            return nullptr;
        }

        const int drv = letter - 'a';

        if ((drv < 0) || (drv >= static_cast<int>(roots_.size())) || !mappings_[drv].second) {
            return nullptr;
        }

        memory_node *at = &roots_[drv];

        for (const std::string &part : parts) {
            auto found = at->children.find(lowered(part));

            if (found == at->children.end()) {
                return nullptr;
            }

            at = &found->second;
        }

        return at;
    }

    memory_node *memory_file_system::resolve_parent(const std::string &raw, std::string &leaf) {
        const std::string path = (static_cast<int>(version_) >= static_cast<int>(epocver::eka2))
            ? rewrite_for_eka2(raw)
            : raw;

        char letter = '\0';
        std::vector<std::string> parts;

        if (!split_path(path, letter, parts) || parts.empty()) {
            return nullptr;
        }

        leaf = parts.back();
        parts.pop_back();

        const int drv = letter - 'a';

        if ((drv < 0) || (drv >= static_cast<int>(roots_.size())) || !mappings_[drv].second) {
            return nullptr;
        }

        memory_node *at = &roots_[drv];

        for (const std::string &part : parts) {
            auto found = at->children.find(lowered(part));

            if (found == at->children.end()) {
                return nullptr;
            }

            at = &found->second;
        }

        return at;
    }

    bool memory_file_system::drive_is_mounted(const drive_number drv) const {
        return mappings_[static_cast<int>(drv)].second;
    }

    bool memory_file_system::drive_is_write_protected(const drive_number drv) const {
        return (mappings_[static_cast<int>(drv)].first.attribute & io_attrib_write_protected) != 0;
    }

    bool memory_file_system::mount_empty(const drive_number drv, const drive_media media,
        const std::uint32_t attrib) {
        const int index = static_cast<int>(drv);

        if (mappings_[index].second) {
            return false;
        }

        eka2l1::drive &entry = mappings_[index].first;

        entry.attribute = attrib;
        entry.type = eka2l1::io_component_type::drive;
        entry.drive_name = std::string(1, static_cast<char>('a' + index)) + ':';
        entry.media_type = media;

        // There is no host path behind any of this, and saying so is the
        // point: code that asks for one has to cope, and all of it does.
        entry.real_path.clear();

        mappings_[index].second = true;
        roots_[index].is_dir = true;

        return true;
    }

    int memory_file_system::mount_rpkg(const drive_number drv, std::vector<std::uint8_t> package) {
        const std::uint8_t *at = package.data();
        const std::uint8_t *end = at + package.size();

        auto take = [&at, end](void *out, const std::size_t n) {
            if (static_cast<std::size_t>(end - at) < n) {
                return false;
            }

            std::memcpy(out, at, n);
            at += n;
            return true;
        };

        // The header, as the emulator's installer reads it (rpkg.cpp): four
        // words of magic - one letter to a 32-bit word - the ROM's version, the entry count, and on the
        // second format the header's own size and the machine's UID.
        std::uint32_t magic[4];
        std::uint8_t major_rom = 0;
        std::uint8_t minor_rom = 0;
        std::uint16_t build_rom = 0;
        std::uint32_t count = 0;

        if (!take(magic, 16) || (magic[0] != 'R') || (magic[1] != 'P') || (magic[2] != 'K')
            || ((magic[3] != 'G') && (magic[3] != '2'))) {
            return -1;
        }

        if (!take(&major_rom, 1) || !take(&minor_rom, 1) || !take(&build_rom, 2) || !take(&count, 4)) {
            return -1;
        }

        if (magic[3] == '2') {
            std::uint32_t header_size = 0;
            std::uint32_t machine_uid = 0;

            if (!take(&header_size, 4) || !take(&machine_uid, 4) || (header_size != 32)) {
                return -1;
            }
        }

        if (!mount_empty(drv, drive_media::rom, io_attrib_internal | io_attrib_write_protected)) {
            return -1;
        }

        // Moving a vector keeps its buffer, so what was read so far still
        // points into the package once it is held here.
        packages_.push_back(std::move(package));

        const std::uint8_t *cursor = at;
        const std::uint8_t *stop = end;

        int files = 0;

        while (static_cast<std::size_t>(stop - cursor) >= 24) {
            std::uint64_t attrib = 0;
            std::uint64_t time = 0;
            std::uint64_t path_len = 0;

            std::memcpy(&attrib, cursor, 8);
            std::memcpy(&time, cursor + 8, 8);
            std::memcpy(&path_len, cursor + 16, 8);
            cursor += 24;

            if (static_cast<std::size_t>(stop - cursor) < path_len * 2 + 8) {
                break;
            }

            std::u16string path(static_cast<std::size_t>(path_len), u'\0');
            std::memcpy(path.data(), cursor, path_len * 2);
            cursor += path_len * 2;

            std::uint64_t data_size = 0;
            std::memcpy(&data_size, cursor, 8);
            cursor += 8;

            if (static_cast<std::uint64_t>(stop - cursor) < data_size) {
                break;
            }

            // "Z:\..." with the drive dropped and the rest lowercased: what the
            // installer writes beside the ROM, so the same names come back.
            const std::string relative = (path.size() > 3)
                ? lowered(eka2l1::common::ucs2_to_utf8(path.substr(3)))
                : std::string();

            char letter = '\0';
            std::vector<std::string> parts;
            split_path(std::string("z:\\") + relative, letter, parts);

            if (!parts.empty()) {
                memory_node *node = &roots_[static_cast<int>(drv)];

                for (std::size_t i = 0; i < parts.size(); i++) {
                    memory_node &child = node->children[parts[i]];

                    if (child.name.empty()) {
                        child.name = parts[i];
                    }

                    child.is_dir = (i + 1 < parts.size());
                    node = &child;
                }

                node->fixed = cursor;
                node->fixed_size = static_cast<std::size_t>(data_size);
                files++;
            }

            cursor += data_size;
        }

        return files;
    }

    bool memory_file_system::read_whole(const std::string &path, std::string &out) {
        memory_node *node = resolve(path);

        if (!node || node->is_dir) {
            return false;
        }

        const std::uint8_t *from = node->fixed ? node->fixed : node->bytes.data();
        const std::size_t size = node->fixed ? node->fixed_size : node->bytes.size();

        out.assign(reinterpret_cast<const char *>(from), size);
        return true;
    }

    std::vector<std::string> memory_file_system::list_names(const std::string &path) {
        std::vector<std::string> names;
        memory_node *node = resolve(path);

        if (node && node->is_dir) {
            for (const auto &child : node->children) {
                names.push_back(child.second.name);
            }
        }

        return names;
    }

    std::size_t memory_file_system::entry_count() const {
        std::size_t total = 0;

        for (std::size_t i = 0; i < roots_.size(); i++) {
            if (mappings_[i].second) {
                total += count_entries(roots_[i]) - 1;
            }
        }

        return total;
    }

    static void walk_files(const memory_node &node, const std::string &prefix,
        const std::function<void(const std::string &, const std::vector<std::uint8_t> &)> &visitor) {
        for (const auto &child : node.children) {
            const std::string path = prefix + child.second.name;

            if (child.second.is_dir) {
                walk_files(child.second, path + "\\", visitor);
            } else {
                visitor(path, child.second.bytes);
            }
        }
    }

    void memory_file_system::each_file(
        const std::function<void(const std::string &, const std::vector<std::uint8_t> &)> &visitor) const {
        for (std::size_t i = 0; i < roots_.size(); i++) {
            if (mappings_[i].second) {
                walk_files(roots_[i], std::string(1, static_cast<char>('A' + i)) + ":\\", visitor);
            }
        }
    }

    std::size_t memory_file_system::written_bytes() const {
        std::size_t total = 0;

        for (std::size_t i = 0; i < roots_.size(); i++) {
            if (mappings_[i].second) {
                total += count_written(roots_[i]);
            }
        }

        return total;
    }

    bool memory_file_system::exists(const std::u16string &path) {
        return resolve(eka2l1::common::ucs2_to_utf8(path)) != nullptr;
    }

    bool memory_file_system::replace(const std::u16string &old_path, const std::u16string &new_path) {
        const std::string from = eka2l1::common::ucs2_to_utf8(old_path);
        const std::string to = eka2l1::common::ucs2_to_utf8(new_path);

        std::string from_leaf;
        memory_node *from_parent = resolve_parent(from, from_leaf);

        if (!from_parent) {
            return false;
        }

        auto found = from_parent->children.find(lowered(from_leaf));

        if (found == from_parent->children.end()) {
            return false;
        }

        std::string to_leaf;
        memory_node *to_parent = resolve_parent(to, to_leaf);

        if (!to_parent) {
            return false;
        }

        memory_node moved = std::move(found->second);
        moved.name = to_leaf;

        from_parent->children.erase(found);
        to_parent->children[lowered(to_leaf)] = std::move(moved);

        return true;
    }

    bool memory_file_system::mount_volume_from_path(const drive_number drv, const drive_media media,
        const std::uint32_t attrib, const std::u16string &physical_path) {
        // Nothing here has a physical path. A caller that names one is asking
        // for the host's filesystem, which is the other filesystem's job.
        (void)physical_path;
        (void)drv;
        (void)media;
        (void)attrib;

        return false;
    }

    bool memory_file_system::unmount(const drive_number drv) {
        const int index = static_cast<int>(drv);

        if (!mappings_[index].second) {
            return false;
        }

        mappings_[index].second = false;
        roots_[index].children.clear();

        return true;
    }

    std::unique_ptr<eka2l1::file> memory_file_system::open_file(const std::u16string &path, const int mode) {
        const std::string utf8 = eka2l1::common::ucs2_to_utf8(path);

        char letter = '\0';
        std::vector<std::string> parts;

        if (!split_path(utf8, letter, parts)) {
            return nullptr;
        }

        const int drv = letter - 'a';

        if ((drv < 0) || (drv >= static_cast<int>(roots_.size())) || !mappings_[drv].second) {
            return nullptr;
        }

        if ((mode & (WRITE_MODE | APPEND_MODE))
            && (mappings_[drv].first.attribute & io_attrib_write_protected)) {
            return nullptr;
        }


        memory_node *node = resolve(utf8);

        if (!node && (mode & WRITE_MODE)) {
            std::string leaf;
            memory_node *parent = resolve_parent(utf8, leaf);

            if (!parent) {
                return nullptr;
            }

            memory_node &created = parent->children[lowered(leaf)];
            created.name = leaf;
            created.is_dir = false;
            node = &created;
        }

        if (!node || node->is_dir) {
            return nullptr;
        }

        // Truncate only when the caller asked to WRITE and nothing else. That
        // is the host filesystem's contract, and the file server leans on it:
        // RFile::Replace opens write-only ("wb+", start empty), RFile::Open
        // with EFileWrite opens read AND write ("rb+", keep what is there).
        // Emptying the file for the second one destroys what the caller was
        // about to read - an installer streaming a package into place reads
        // its own source through such a handle, and got nothing.
        if (node->fixed && (mode & (WRITE_MODE | APPEND_MODE))) {
            return nullptr;
        }

        if ((mode & WRITE_MODE) && !(mode & READ_MODE) && !(mode & APPEND_MODE)) {
            node->bytes.clear();
        }

        return std::make_unique<memory_file>(node, path, mode);
    }

    std::unique_ptr<eka2l1::directory> memory_file_system::open_directory(const std::u16string &path,
        eka2l1::epoc::uid_type type, const std::uint32_t attrib) {
        std::string utf8 = eka2l1::common::ucs2_to_utf8(path);
        std::string filter("*");

        // The last component is a filter unless the path ends in a separator,
        // which is the same rule the host-backed filesystem follows.
        const std::size_t last = utf8.find_last_of("\\/");

        if ((last != std::string::npos) && (last != utf8.length() - 1)) {
            filter = utf8.substr(last + 1);
            utf8.erase(utf8.begin() + last + 1, utf8.end());
        }

        memory_node *node = resolve(utf8);

        if (!node || !node->is_dir) {
            return nullptr;
        }

        return std::make_unique<memory_directory>(this, node, utf8, filter, type, attrib);
    }

    std::optional<eka2l1::entry_info> memory_file_system::get_entry_info(const std::u16string &path) {
        const std::string utf8 = eka2l1::common::ucs2_to_utf8(path);
        memory_node *node = resolve(utf8);

        if (!node) {
            return std::nullopt;
        }

        eka2l1::entry_info info;

        info.type = node->is_dir ? eka2l1::io_component_type::dir : eka2l1::io_component_type::file;
        info.size = node->is_dir ? 0 : (node->fixed ? node->fixed_size : node->bytes.size());
        info.full_path = utf8;
        info.name = eka2l1::filename(utf8);
        info.last_write = 0;

        char letter = '\0';
        std::vector<std::string> parts;
        split_path(utf8, letter, parts);

        info.attribute = mappings_[letter - 'a'].first.attribute;

        return info;
    }

    eka2l1::abstract_file_system_err_code memory_file_system::is_entry_in_rom(const std::u16string &path) {
        // The pack is a copy of a ROM's files, not the ROM image: nothing in
        // it has an address in the machine's memory.
        (void)path;
        return eka2l1::abstract_file_system_err_code::no;
    }

    bool memory_file_system::delete_entry(const std::u16string &path) {
        const std::string utf8 = eka2l1::common::ucs2_to_utf8(path);

        std::string leaf;
        memory_node *parent = resolve_parent(utf8, leaf);

        if (!parent) {
            return false;
        }

        auto found = parent->children.find(lowered(leaf));

        if (found == parent->children.end()) {
            return false;
        }

        // A directory goes only when it is empty, as RmDir and the host's own
        // remove() have it. An installer's integrity service tidies up by
        // removing each directory on the way back to the root and counts on
        // the ones still holding something to refuse: taking the whole tree
        // took C:\sys\bin with it, and every program installed there.
        if (found->second.is_dir && !found->second.children.empty()) {
            return false;
        }

        parent->children.erase(found);
        return true;
    }

    bool memory_file_system::create_directory(const std::u16string &path) {
        const std::string utf8 = eka2l1::common::ucs2_to_utf8(path);

        std::string leaf;
        memory_node *parent = resolve_parent(utf8, leaf);

        if (!parent) {
            return false;
        }

        memory_node &created = parent->children[lowered(leaf)];
        created.name = leaf;
        created.is_dir = true;

        return true;
    }

    bool memory_file_system::create_directories(const std::u16string &path) {
        const std::string utf8 = eka2l1::common::ucs2_to_utf8(path);

        char letter = '\0';
        std::vector<std::string> parts;

        if (!split_path(utf8, letter, parts)) {
            return false;
        }

        const int drv = letter - 'a';

        if ((drv < 0) || (drv >= static_cast<int>(roots_.size())) || !mappings_[drv].second) {
            return false;
        }

        memory_node *at = &roots_[drv];

        for (const std::string &part : parts) {
            memory_node &child = at->children[lowered(part)];

            if (child.name.empty()) {
                child.name = part;
            }

            child.is_dir = true;
            at = &child;
        }

        return true;
    }

    std::optional<eka2l1::drive> memory_file_system::get_drive_entry(const drive_number drv) {
        const int index = static_cast<int>(drv);

        if (!mappings_[index].second) {
            return std::nullopt;
        }

        return mappings_[index].first;
    }

    std::optional<std::u16string> memory_file_system::get_raw_path(const std::u16string &path) {
        // A path on the host is exactly what these files do not have.
        (void)path;
        return std::nullopt;
    }

    void memory_file_system::set_epoc_ver(const epocver ver) {
        version_ = ver;
    }

    void memory_file_system::validate_for_host() {
    }
}
