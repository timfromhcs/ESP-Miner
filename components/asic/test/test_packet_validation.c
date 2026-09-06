#include "unity.h"
#include "crc.h"
#include "asic_common.h"
#include <string.h>

TEST_CASE("Validate CRC5 calculation for ASIC command and response packets", "[asic_crc5]")
{
    // Test known BM1366 chip ID request: 55 AA 52 05 00 00 0A (CRC5 is 0x0A on 0x52, 0x05, 0x00, 0x00)
    uint8_t init3_data[4] = {0x52, 0x05, 0x00, 0x00};
    uint8_t calculated_crc = crc5(init3_data, 4);
    TEST_ASSERT_EQUAL_UINT8(0x0A, calculated_crc);

    // Test known BM1366 chain inactive command: 55 AA 53 05 00 00 03 (CRC5 is 0x03)
    uint8_t inactive_data[4] = {0x53, 0x05, 0x00, 0x00};
    TEST_ASSERT_EQUAL_UINT8(0x03, crc5(inactive_data, 4));

    // Test CRC5 difference on modified payload byte
    uint8_t modified_data[4] = {0x52, 0x05, 0x01, 0x00};
    TEST_ASSERT_NOT_EQUAL(calculated_crc, crc5(modified_data, 4));
}

TEST_CASE("Validate bit reversal and power of two math", "[asic_math]")
{
    TEST_ASSERT_EQUAL_UINT8(0x80, _reverse_bits(0x01));
    TEST_ASSERT_EQUAL_UINT8(0x0F, _reverse_bits(0xF0));
    TEST_ASSERT_EQUAL_UINT8(0xAA, _reverse_bits(0x55));
    TEST_ASSERT_EQUAL_UINT8(0x00, _reverse_bits(0x00));
    TEST_ASSERT_EQUAL_UINT8(0xFF, _reverse_bits(0xFF));

    TEST_ASSERT_EQUAL_INT(1, _next_power_of_two(1));
    TEST_ASSERT_EQUAL_INT(2, _next_power_of_two(2));
    TEST_ASSERT_EQUAL_INT(4, _next_power_of_two(3));
    TEST_ASSERT_EQUAL_INT(4, _next_power_of_two(4));
    TEST_ASSERT_EQUAL_INT(8, _next_power_of_two(5));
    TEST_ASSERT_EQUAL_INT(256, _next_power_of_two(200));
}

TEST_CASE("Validate ASIC difficulty mask generation", "[asic_difficulty_mask]")
{
    uint8_t mask_256[6];
    get_difficulty_mask(256.0, mask_256);
    TEST_ASSERT_EQUAL_HEX8(0x00, mask_256[0]);
    TEST_ASSERT_EQUAL_HEX8(0x14, mask_256[1]);

    uint8_t mask_512[6];
    get_difficulty_mask(512.0, mask_512);
    TEST_ASSERT_EQUAL_HEX8(0x00, mask_512[0]);
    TEST_ASSERT_EQUAL_HEX8(0x14, mask_512[1]);
}

TEST_CASE("Simulate ASIC response stream framing and resynchronization", "[asic_framing]")
{
    // Simulate valid BM1366 11-byte frame with preamble 0xAA55
    uint8_t valid_frame[11] = {
        0xAA, 0x55,             // Preamble (little endian in uint16_t: 0xAA55)
        0x12, 0x34, 0x56, 0x78, // Nonce
        0x00, 0x01,             // midstate, id
        0x00, 0x00,             // version
        0x00                    // CRC placeholder
    };
    // Calculate CRC5 for bytes 2..9 (8 bytes)
    uint8_t expected_crc = crc5(valid_frame + 2, 8);
    valid_frame[10] = expected_crc;

    // Verify preamble
    uint16_t preamble = (valid_frame[0] << 8) | valid_frame[1];
    TEST_ASSERT_EQUAL_HEX16(0xAA55, preamble);

    // Verify CRC5 match on valid frame
    TEST_ASSERT_EQUAL_UINT8(expected_crc, crc5(valid_frame + 2, 8));

    // Simulate corrupted byte in payload and verify CRC mismatch
    uint8_t corrupted_frame[11];
    memcpy(corrupted_frame, valid_frame, 11);
    corrupted_frame[4] ^= 0xFF; // flip bits in nonce
    TEST_ASSERT_NOT_EQUAL(expected_crc, crc5(corrupted_frame + 2, 8));
}
