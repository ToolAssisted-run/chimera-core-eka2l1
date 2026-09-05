#pragma once

// A filesystem the machine carries in its own memory.
//
// The sandbox has no filesystem: miniBox mounts a flat list of named files and
// there are no directories, no creation and no enumeration. Symbian needs all
// three - the application scan alone walks drive Z looking for registration
// files - so the drives are served from here instead, through EKA2L1's own
// pluggable filesystem interface rather than by pretending to be a host.
//
// Two kinds of file live in the tree. One is bytes the machine wrote, which
// are the machine's state and belong in its memory and its savestates. The
// other is a slice of a device pack the host mounted, read as the machine
// reads it: 36 megabytes of ROM files that never change are not the machine's
// state, and copying them into the arena would put them in every savestate for
// nothing.
#include <common/types.h>
#include <loader/rom.h>
#include <vfs/vfs.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace chimera {
    struct memory_node {
        std::string name;
        bool is_dir = false;

        // Children by lowercased name: Symbian paths are case-insensitive,
        // and the order files come back in must not depend on the host.
        std::map<std::string, memory_node> children;

        std::vector<std::uint8_t> bytes;

        bool in_pack = false;
        std::uint64_t pack_offset = 0;
        std::uint64_t pack_size = 0;
    };

    class memory_file_system : public eka2l1::abstract_file_system {
    public:
        memory_file_system();
        ~memory_file_system();

        // Reads the index of a device pack the host mounted under `pack_name`
        // and grafts its tree onto `drv`, which is mounted read-only. The
        // contents stay in the pack; only the index is held here.
        bool graft_pack(const std::string &pack_name, const drive_number drv,
            const std::uint32_t attrib);

        // The ROM this device booted from. Files served from the pack are
        // copies of the ROM's own, so the machine can ask where one lives in
        // its address space and be told the truth - which it does: a Symbian
        // application opens a resource file "in raw mode" and then reads it
        // straight out of ROM rather than through the file server.
        void set_rom(eka2l1::loader::rom *rom) { rom_ = rom; }

        address rom_address_of(const std::string &path) const;

        // Mounts an empty writable drive.
        bool mount_empty(const drive_number drv, const drive_media media,
            const std::uint32_t attrib);

        // What the pack said about the device it came from. Empty until a pack
        // is grafted.
        const std::string &device_firmcode() const { return firmcode_; }
        const std::string &device_model() const { return model_; }
        const std::string &device_manufacturer() const { return manufacturer_; }
        std::uint32_t device_epocver() const { return epocver_; }
        std::uint32_t device_machine_uid() const { return machine_uid_; }

        // Diagnostics: how many entries the tree holds, and how many bytes of
        // it are the machine's own rather than the pack's.
        std::size_t entry_count() const;
        std::size_t written_bytes() const;

        const std::string &pack_name() const { return pack_name_; }

        bool exists(const std::u16string &path) override;
        bool replace(const std::u16string &old_path, const std::u16string &new_path) override;
        bool mount_volume_from_path(const drive_number drv, const drive_media media,
            const std::uint32_t attrib, const std::u16string &physical_path) override;
        bool unmount(const drive_number drv) override;
        std::unique_ptr<eka2l1::file> open_file(const std::u16string &path, const int mode) override;
        std::unique_ptr<eka2l1::directory> open_directory(const std::u16string &path,
            eka2l1::epoc::uid_type type, const std::uint32_t attrib) override;
        std::optional<eka2l1::entry_info> get_entry_info(const std::u16string &path) override;
        eka2l1::abstract_file_system_err_code is_entry_in_rom(const std::u16string &path) override;
        bool delete_entry(const std::u16string &path) override;
        bool create_directory(const std::u16string &path) override;
        bool create_directories(const std::u16string &path) override;
        std::optional<eka2l1::drive> get_drive_entry(const drive_number drv) override;
        std::optional<std::u16string> get_raw_path(const std::u16string &path) override;
        void set_epoc_ver(const epocver ver) override;
        void validate_for_host() override;

        // Resolution, exposed for the directory iterator.
        memory_node *resolve(const std::string &path);
        memory_node *resolve_parent(const std::string &path, std::string &leaf);
        bool drive_is_mounted(const drive_number drv) const;
        bool drive_is_write_protected(const drive_number drv) const;

    private:
        std::array<memory_node, drive_z + 1> roots_;
        std::array<std::pair<eka2l1::drive, bool>, drive_z + 1> mappings_;

        std::string pack_name_;
        eka2l1::loader::rom *rom_ = nullptr;
        epocver version_ = epocver::epoc6;
        std::string firmcode_;
        std::string model_;
        std::string manufacturer_;
        std::uint32_t epocver_ = 0;
        std::uint32_t machine_uid_ = 0;
    };
}
