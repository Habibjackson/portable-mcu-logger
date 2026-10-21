#include "log_format.h"
#include "log_storage_ram.h"

#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; \
} } while (0)

static int test_record(void)
{
    static const uint8_t payload[] = { 0x11u, 0x22u, 0x33u };
    const elog_record_t source = {
        ELOG_TYPE_EVENT, ELOG_EVENT, ELOG_CODEC_NONE, 0u,
        UINT64_C(0x0102030405060708), UINT64_C(0x1112131415161718),
        UINT32_C(0xA1B2C3D4), 3u, 3u, payload
    };
    uint8_t image[ELOG_MAX_RECORD];
    elog_record_layout_t layout;
    elog_record_t decoded;
    CHECK(elog_crc32((const uint8_t *)"123456789", 9u) == UINT32_C(0xCBF43926));
    CHECK(elog_record_encode(&source, 2u, image, sizeof image, &layout) == ELOG_OK);
    CHECK(layout.total_length == 46u);
    CHECK(memcmp(image, "ELOG", 4u) == 0);
    CHECK(image[12] == 0x08u && image[19] == 0x01u);
    CHECK(image[28] == 0xD4u && image[31] == 0xA1u);
    CHECK(image[layout.commit_offset] == ELOG_COMMIT_BYTE);
    CHECK(elog_record_decode(image, layout.total_length, 2u, &decoded,
                             &layout) == ELOG_OK);
    CHECK(decoded.sequence == source.sequence &&
          decoded.timestamp == source.timestamp &&
          decoded.event_id == source.event_id &&
          memcmp(decoded.payload, payload, sizeof payload) == 0);
    image[36] ^= 1u;
    CHECK(elog_record_decode(image, layout.total_length, 2u, &decoded,
                             &layout) == ELOG_CORRUPT);
    image[36] ^= 1u;
    image[layout.commit_offset] = 0xFFu;
    CHECK(elog_record_decode(image, layout.total_length, 2u, &decoded,
                             &layout) == ELOG_CORRUPT);
    CHECK(elog_record_encode(&source, 8u, image, sizeof image, &layout) == ELOG_OK);
    CHECK(layout.total_length == 56u);
    CHECK(elog_record_decode(image, layout.total_length, 8u, &decoded,
                             &layout) == ELOG_OK);
    image[4] = 2u;
    CHECK(elog_record_decode(image, layout.total_length, 8u, &decoded,
                             &layout) == ELOG_UNSUPPORTED);
    return 0;
}

static int test_ram(void)
{
    uint8_t bytes[512];
    const elog_erase_region_t regions[] = { {0u, 128u}, {128u, 384u} };
    elog_ram_storage_t ram;
    elog_storage_t storage;
    const uint8_t data[4] = {1u, 2u, 3u, 4u};
    uint8_t readback[4];
    elog_erase_region_t region;
    CHECK(elog_ram_storage_init(&ram, bytes, sizeof bytes, regions, 2u, 2u,
                                64u, &storage) == ELOG_OK);
    CHECK(elog_ram_storage_format(&ram) == ELOG_OK);
    CHECK(storage.ops->erase_region(storage.context, 200u, &region) == ELOG_OK);
    CHECK(region.start == 128u && region.length == 384u);
    CHECK(storage.ops->program(storage.context, 2u, data, sizeof data, 1u) == ELOG_OK);
    CHECK(storage.ops->program(storage.context, 2u, data, sizeof data, 1u) == ELOG_STORAGE_IO);
    CHECK(storage.ops->program(storage.context, 1u, data, sizeof data, 1u) == ELOG_INVALID_ARGUMENT);
    CHECK(storage.ops->read(storage.context, 2u, readback, sizeof readback) == ELOG_OK);
    CHECK(memcmp(readback, data, sizeof data) == 0);
    CHECK(storage.ops->erase(storage.context, 0u, 128u, 1u) == ELOG_OK);
    CHECK(storage.ops->read(storage.context, 2u, readback, sizeof readback) == ELOG_OK);
    CHECK(readback[0] == 0xFFu);
    CHECK(storage.ops->erase(storage.context, 0u, 512u, 1u) == ELOG_INVALID_ARGUMENT);
    return 0;
}

int main(void)
{
    if (test_record() != 0 || test_ram() != 0) {
        return 1;
    }
    puts("record format and RAM backend: OK");
    return 0;
}
