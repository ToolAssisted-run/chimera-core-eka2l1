#pragma once

namespace chimera {
    // A file EKA2L1 would have opened beside its executable, carried inside the
    // binary instead. Named by file name, exactly as the folder walk this
    // machine has no folder for would have named it.
    struct embedded_file {
        const char *name;
        const unsigned char *data;
        unsigned int size;
    };

    // EKA2L1's prebuilt patch libraries and the map files that say which ROM
    // exports they replace.
    extern const embedded_file PATCH_BLOBS[];
    extern const unsigned int PATCH_BLOB_COUNT;

    // The graphics driver's shader sources. Without them every draw is a
    // silent no-op.
    extern const embedded_file SHADER_BLOBS[];
    extern const unsigned int SHADER_BLOB_COUNT;
}
