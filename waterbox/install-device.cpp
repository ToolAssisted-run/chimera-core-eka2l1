// Provisioning: turn a Symbian ROM dump into an installed device under a
// storage root, the way the frontend's device dialog does it, with no dialog.
//
//   install-device --data <storage root> --rom <SYM.ROM> [--rpkg <file>]
//
// This is not part of the machine. It runs once, before anything is emulated,
// and what it writes - drive Z, the ROM image, devices.yml - is the device the
// core is later pointed at. The ROM is the user's; nothing it produces belongs
// in this repository either.
#include <common/log.h>
#include <common/path.h>
#include <config/config.h>
#include <system/devices.h>
#include <system/installation/rpkg.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {
    const char *error_text(const eka2l1::device_installation_error error) {
        switch (error) {
        case eka2l1::device_installation_none:
            return "ok";
        case eka2l1::device_installation_not_exist:
            return "the file does not exist";
        case eka2l1::device_installation_insufficent:
            return "not enough space";
        case eka2l1::device_installation_rpkg_corrupt:
            return "the RPKG is corrupt";
        case eka2l1::device_installation_determine_product_failure:
            return "the product could not be determined from the dump";
        case eka2l1::device_installation_already_exist:
            return "the device is already installed";
        case eka2l1::device_installation_general_failure:
            return "general failure";
        case eka2l1::device_installation_rom_fail_to_copy:
            return "the ROM could not be copied";
        case eka2l1::device_installation_vpl_file_invalid:
            return "the VPL file is invalid";
        case eka2l1::device_installation_rofs_corrupt:
            return "the ROFS is corrupt";
        case eka2l1::device_installation_rom_file_corrupt:
            return "the ROM file is corrupt";
        case eka2l1::device_installation_fpsx_corrupt:
            return "the FPSX is corrupt";
        case eka2l1::device_installation_rpkg_missing:
            return "this ROM keeps drive Z in ROFS and needs the RPKG dump alongside it";
        case eka2l1::device_installation_archive_corrupt:
            return "the archive could not be read";
        case eka2l1::device_installation_archive_no_device:
            return "the archive holds no device dump";
        default:
            return "unknown";
        }
    }
}

int main(int argc, char **argv) {
    std::string storage = "data";
    std::string rom;
    std::string rpkg;

    for (int i = 1; i < argc; i++) {
        const bool has_value = (i + 1 < argc);

        if ((std::strcmp(argv[i], "--data") == 0) && has_value) {
            storage = argv[++i];
        } else if ((std::strcmp(argv[i], "--rom") == 0) && has_value) {
            rom = argv[++i];
        } else if ((std::strcmp(argv[i], "--rpkg") == 0) && has_value) {
            rpkg = argv[++i];
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (rom.empty()) {
        std::fprintf(stderr, "usage: install-device --data <storage> --rom <SYM.ROM> [--rpkg <file>]\n");
        return 2;
    }

    eka2l1::log::setup_log(nullptr);

    eka2l1::config::state conf;
    conf.storage = storage;

    eka2l1::device_manager devices(&conf);

    const std::string rom_resident_path = eka2l1::add_path(storage, "roms/");
    const std::string drive_z_path = eka2l1::add_path(storage, "drives/z/");

    auto progress = [](const std::size_t done, const std::size_t total) {
        (void)done;
        (void)total;
    };

    auto cancelled = []() {
        return false;
    };

    const eka2l1::device_installation_error error = eka2l1::loader::install_rom_with_optional_rpkg(
        &devices, rom, rpkg, rom_resident_path, drive_z_path, progress, cancelled);

    std::printf("install: %s\n", error_text(error));

    if (error != eka2l1::device_installation_none) {
        return 1;
    }

    devices.save_devices();

    std::printf("devices: %zu\n", devices.total());

    for (std::size_t i = 0; i < devices.total(); i++) {
        const eka2l1::device &dvc = devices.get_devices()[i];
        std::printf("device %zu: %s %s (%s) epocver=%d\n", i, dvc.manufacturer.c_str(),
            dvc.model.c_str(), dvc.firmware_code.c_str(), static_cast<int>(dvc.ver));
    }

    return 0;
}
