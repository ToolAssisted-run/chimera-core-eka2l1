#pragma once

// A filesystem the machine carries in its own memory.
//
// The sandbox has no filesystem: miniBox mounts a flat list of named files and
// there are no directories, no creation and no enumeration. Symbian needs all
// three - the application scan alone walks drive Z looking for registration
// files - so the drives are served from here instead, through EKA2L1's own
// pluggable filesystem interface rather than by pretending to be a host.
//
// Everything in it is bytes the machine wrote, which are the machine's state
// and belong in its memory and its savestates. Drive Z is not here: the ROM
// carries its own filesystem and serves it, so the drives this holds are the
// writable ones.
#include <common/types.h>
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
    };

    class memory_file_system : public eka2l1::abstract_file_system {
    public:
        memory_file_system();
        ~memory_file_system();

        // Reads the device descriptor the host mounted under `name`: which
        // device this is, which Symbian it runs, and what its machine uid is.
        // The descriptor is all the machine needs beside its ROM.
        bool read_device_info(const std::string &name);

        // Mounts an empty writable drive.
        bool mount_empty(const drive_number drv, const drive_media media,
            const std::uint32_t attrib);

        // What the descriptor said. Empty until one is read.
        const std::string &device_firmcode() const { return firmcode_; }
        const std::string &device_model() const { return model_; }
        const std::string &device_manufacturer() const { return manufacturer_; }
        std::uint32_t device_epocver() const { return epocver_; }
        std::uint32_t device_machine_uid() const { return machine_uid_; }

        // Diagnostics: how many entries the tree holds, and how many bytes the
        // machine has written into it.
        std::size_t entry_count() const;
        std::size_t written_bytes() const;

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

        epocver version_ = epocver::epoc6;
        std::string firmcode_;
        std::string model_;
        std::string manufacturer_;
        std::uint32_t epocver_ = 0;
        std::uint32_t machine_uid_ = 0;
    };
}
