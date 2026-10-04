#include "coinbase_decoder.h"
#include "stratum_api.h"
#include "utils.h"
#include "segwit_addr.h"
#include "libbase58.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "esp_log.h"

static const char * TAG = "coinbase";

#define BIP110_SIGNAL_BIT 4
#define BIP110_SIGNAL_EXPIRY_BLOCK 965664

// Wrapper for SHA256 to match libbase58's expected signature
static bool my_sha256(void *digest, const void *data, size_t datasz) {
    sha256_bin(data, datasz, digest);
    return true;
}

static void ensure_base58_init(void) {
    if (b58_sha256_impl == NULL) {
        b58_sha256_impl = my_sha256;
    }
}

uint64_t coinbase_decode_varint(const uint8_t *data, int *offset) {
    uint8_t first_byte = data[*offset];
    (*offset)++;
    
    if (first_byte < 0xFD) {
        return first_byte;
    } else if (first_byte == 0xFD) {
        uint64_t value = data[*offset] | (data[*offset + 1] << 8);
        *offset += 2;
        return value;
    } else if (first_byte == 0xFE) {
        uint64_t value = data[*offset] | (data[*offset + 1] << 8) | 
                        (data[*offset + 2] << 16) | (data[*offset + 3] << 24);
        *offset += 4;
        return value;
    } else { // 0xFF
        uint64_t value = 0;
        for (int i = 0; i < 8; i++) {
            value |= ((uint64_t)data[*offset + i]) << (i * 8);
        }
        *offset += 8;
        return value;
    }
}

void coinbase_decode_address_from_scriptpubkey(const uint8_t *script, size_t script_len, 
                                                char *output, size_t output_len,
                                                const char *bech32_hrp, bool is_testnet) {
    if (script_len == 0 || output_len < 65) {
        snprintf(output, output_len, "unknown");
        return;
    }
    
    ensure_base58_init();
    
    uint8_t p2pkh_version = is_testnet ? 0x6F : 0x00;
    uint8_t p2sh_version  = is_testnet ? 0xC4 : 0x05;

    // P2PKH: OP_DUP OP_HASH160 <20 bytes> OP_EQUALVERIFY OP_CHECKSIG
    if (script_len == 25 && script[0] == OP_DUP && script[1] == OP_HASH160 && 
        script[2] == OP_PUSHDATA_20 && script[23] == OP_EQUALVERIFY && script[24] == OP_CHECKSIG) {
        size_t b58sz = output_len;
        if (b58check_enc(output, &b58sz, p2pkh_version, script + 3, 20)) {
            return;
        }
        // Fallback
        snprintf(output, output_len, "P2PKH:");
        bin2hex(script + 3, 20, output + 6, output_len - 6);
        return;
    }
    
    // P2SH: OP_HASH160 <20 bytes> OP_EQUAL
    if (script_len == 23 && script[0] == OP_HASH160 && script[1] == OP_PUSHDATA_20 && script[22] == OP_EQUAL) {
        size_t b58sz = output_len;
        if (b58check_enc(output, &b58sz, p2sh_version, script + 2, 20)) {
            return;
        }
        // Fallback
        snprintf(output, output_len, "P2SH:");
        bin2hex(script + 2, 20, output + 5, output_len - 5);
        return;
    }
    
    // P2WPKH: OP_0 <20 bytes>
    if (script_len == 22 && script[0] == OP_0 && script[1] == OP_PUSHDATA_20) {
        if (segwit_addr_encode(output, bech32_hrp, 0, script + 2, 20)) {
            return;
        }
        // Fallback to hex if encoding fails
        snprintf(output, output_len, "P2WPKH:");
        bin2hex(script + 2, 20, output + 7, output_len - 7);
        return;
    }
    
    // P2WSH: OP_0 <32 bytes>
    if (script_len == 34 && script[0] == OP_0 && script[1] == OP_PUSHDATA_32) {
        if (segwit_addr_encode(output, bech32_hrp, 0, script + 2, 32)) {
            return;
        }
        // Fallback to hex if encoding fails
        snprintf(output, output_len, "P2WSH:");
        bin2hex(script + 2, 32, output + 6, output_len - 6);
        return;
    }
    
    // P2TR: OP_1 <32 bytes>
    if (script_len == 34 && script[0] == OP_1 && script[1] == OP_PUSHDATA_32) {
        if (segwit_addr_encode(output, bech32_hrp, 1, script + 2, 32)) {
            return;
        }
        // Fallback to hex if encoding fails
        snprintf(output, output_len, "P2TR:");
        bin2hex(script + 2, 32, output + 5, output_len - 5);
        return;
    }

    // OP_RETURN: OP_RETURN <data>
    if (script_len > 0 && script[0] == OP_RETURN) {
        snprintf(output, output_len, "OP_RETURN: ");
        size_t offset = 1;
        
        // Simple check for small pushdata to skip the length byte
        // If script[1] is the length of the remaining data
        if (script_len > 1 && script[1] > 0 && script[1] <= 0x4b && (size_t)script[1] + 2 == script_len) {
            offset = 2;
        }
        
        size_t out_idx = strlen(output);
        for (size_t i = offset; i < script_len && out_idx < output_len - 1; i++) {
            unsigned char c = script[i];
            output[out_idx++] = isprint(c) ? c : '.';
        }
        output[out_idx] = '\0';
        return;
    }
    
    // Unknown format - just show hex
    snprintf(output, output_len, "UNKNOWN:");
    size_t hex_len = script_len < 32 ? script_len : 32; // Limit to 32 bytes
    bin2hex(script, hex_len, output + 8, output_len - 8);
}

esp_err_t coinbase_process_notification(const mining_notify *notification,
                                 const char *extranonce1,
                                 int extranonce2_len,
                                 const char *user_address,
                                 bool decode_coinbase_tx,
                                 mining_notification_result_t *result) {
    if (!notification || !extranonce1 || !result) { ESP_LOGE(TAG, "coinbase reject @%d: null arg", __LINE__); return ESP_ERR_INVALID_ARG; }

    // Initialize result
    result->total_value_satoshis = 0;
    result->user_value_satoshis = 0;
    result->decode_coinbase_tx = decode_coinbase_tx;

    // Detect network from user address prefix for correct address encoding
    const char *bech32_hrp = "bc";
    bool is_testnet = false;
    if (user_address) {
        if (strncmp(user_address, "bcrt1", 4) == 0) {
            bech32_hrp = "bcrt";
            is_testnet = true;
        } else if (strncmp(user_address, "tb1", 3) == 0) {
            bech32_hrp = "tb";
            is_testnet = true;
        } else if (user_address[0] == 'm' || user_address[0] == 'n' || user_address[0] == '2') {
            bech32_hrp = "tb";
            is_testnet = true;
        }
    }

    // 1. Calculate difficulty
    result->network_difficulty = networkDifficulty(notification->target);

    // 2. Parse Coinbase 1 for ScriptSig info
    int coinbase_1_len = strlen(notification->coinbase_1) / 2;
    int coinbase_1_offset = 41; // Skip version (4), inputcount (1), prevhash (32), vout (4)

    if (coinbase_1_len < coinbase_1_offset) { ESP_LOGE(TAG, "coinbase reject @%d: cb1 short (%d < %d)", __LINE__, coinbase_1_len, coinbase_1_offset); return ESP_ERR_INVALID_ARG; }

    uint8_t scriptsig_len;
    hex2bin(notification->coinbase_1 + (coinbase_1_offset * 2), &scriptsig_len, 1);
    coinbase_1_offset++;

    if (coinbase_1_len < coinbase_1_offset) { ESP_LOGE(TAG, "coinbase reject @%d: cb1 short (%d < %d)", __LINE__, coinbase_1_len, coinbase_1_offset); return ESP_ERR_INVALID_ARG; }

    uint8_t block_height_len;
    hex2bin(notification->coinbase_1 + (coinbase_1_offset * 2), &block_height_len, 1);

    /* The BIP 34 block height lives at the start of the miner tag in coinbase_1.
     *
     * Two pools layouts break a naive read:
     *   - the tag region is already filled with 0xff because the tag has spilled
     *     into coinbase_2, so 0xff is not a length
     *   - coinbase_1 stops right after the vout, so there is no tag at all and the
     *     whole height-plus-extranonce region is in coinbase_2
     *
     * Both were seen with zpool: every notification after the first was rejected as
     * invalid_coinbase, so the unit never submitted a share. The block height is only
     * used for display and BIP-110/BIP-54 signalling, both optional, so the safe
     * reading is to carry on without it rather than discard a usable job. */
    bool height_in_cb1 = (block_height_len >= 1 && block_height_len <= 4);
    if (!height_in_cb1) {
        ESP_LOGD(TAG, "coinbase: no BIP34 height in coinbase_1 at offset %d (len=0x%02x), "
                      "continuing without block height", coinbase_1_offset, block_height_len);
        result->block_height = 0;
        block_height_len = 0;
    } else {
        coinbase_1_offset++;
        if (coinbase_1_len < coinbase_1_offset) { ESP_LOGE(TAG, "coinbase reject @%d: cb1 short (%d < %d)", __LINE__, coinbase_1_len, coinbase_1_offset); return ESP_ERR_INVALID_ARG; }
        result->block_height = 0;
        hex2bin(notification->coinbase_1 + (coinbase_1_offset * 2),
                (uint8_t *)&result->block_height, block_height_len);
        coinbase_1_offset += block_height_len;
    }

    // Detect BIP-110 signaling: check if bit 4 (0x00000010) is set in version
    result->bip110_signaling = decode_coinbase_tx && result->block_height < BIP110_SIGNAL_EXPIRY_BLOCK && (notification->version & (1U << BIP110_SIGNAL_BIT)) != 0;

    // Calculate remaining scriptsig length (excluding block height part)
    int scriptsig_length = scriptsig_len - 1 - block_height_len;
    size_t extranonce1_len = strlen(extranonce1) / 2;
    
    // Check if scriptsig extends into coinbase_2 (meaning it covers the extranonces)
    // If so, subtract extranonce lengths to get just the miner tag length
    if (coinbase_1_len - coinbase_1_offset < scriptsig_length) {
        scriptsig_length -= (extranonce1_len + extranonce2_len);
    }
    
    // Extract miner tag if present
    if (scriptsig_length > 0) {
        char *tag = malloc(scriptsig_length + 1);
        if (tag) {
            int coinbase_1_tag_len = coinbase_1_len - coinbase_1_offset;
            if (coinbase_1_tag_len > scriptsig_length) {
                coinbase_1_tag_len = scriptsig_length;
            }

            hex2bin(notification->coinbase_1 + (coinbase_1_offset * 2), (uint8_t *)tag, coinbase_1_tag_len);

            int coinbase_2_tag_len = scriptsig_length - coinbase_1_tag_len;
            int coinbase_2_len = strlen(notification->coinbase_2) / 2;
            
            if (coinbase_2_len >= coinbase_2_tag_len) {
                if (coinbase_2_tag_len > 0) {
                    hex2bin(notification->coinbase_2, (uint8_t *)tag + coinbase_1_tag_len, coinbase_2_tag_len);
                }
                
                // Filter non-printable characters
                for (int i = 0; i < scriptsig_length; i++) {
                    if (!isprint((unsigned char)tag[i])) {
                        tag[i] = '.';
                    }                }
                tag[scriptsig_length] = '\0';
                result->scriptsig = tag;
            } else {
                free(tag);
                // Tag extraction failed due to length mismatch, but we can continue
            }
        }
    }

    // 3. Parse Coinbase 2 for Outputs
    //
    // BIP 34 defines the start of the transaction body: the height push followed by
    // the extranonce, padded out to exactly 100 bytes.
    //
    // The padding spans both halves. What is already in coinbase_1 after the header
    // counts towards it, and the remainder is taken off the front of coinbase_2.
    // When the height is not in coinbase_1 at all - the layout that made every
    // zpool notification after the first fail - nothing but the extranonce is in
    // coinbase_1, so more of the padding remains to skip in coinbase_2.
    //
    // The offset used to be derived by subtracting the extranonce length from the end
    // of the scriptsig, which assumes the extranonce is the last thing in the
    // scriptsig. It is not necessarily: the tag can spill into coinbase_2, and pools
    // append extra bytes after the extranonce. That arithmetic then produced an
    // offset past the end of coinbase_2 and the notification was rejected as
    // invalid_coinbase.
    int extranonce_bytes = extranonce1_len + extranonce2_len;
    int region_in_cb1 = (coinbase_1_len - coinbase_1_offset);
    int region_total = 1 + block_height_len + extranonce_bytes;   /* push + height + extranonce */
    int padding_total = (region_total < 100) ? (100 - region_total) : 0;
    int padding_left = padding_total - region_in_cb1;
    if (padding_left < 0) {
        padding_left = 0;
    }

    int coinbase_2_offset = padding_left;
    int coinbase_2_len = strlen(notification->coinbase_2) / 2;

    /* Bound the offset to what coinbase_2 actually holds. Reading past the buffer
     * would be worse than an imprecise offset, so clamp rather than reject. */
    if (coinbase_2_offset + 5 > coinbase_2_len) {
        ESP_LOGD(TAG, "coinbase: BIP34 offset %d exceeds coinbase_2 (%d B), clamping",
                 coinbase_2_offset, coinbase_2_len);
        coinbase_2_offset = (coinbase_2_len > 5) ? (coinbase_2_len - 5) : 0;
    }

    uint8_t *coinbase_2_bin = malloc(coinbase_2_len);
    if (!coinbase_2_bin) {
        return ESP_ERR_NO_MEM; // Memory error is fatal
    }
    
    hex2bin(notification->coinbase_2, coinbase_2_bin, coinbase_2_len);

    int offset = coinbase_2_offset;

    /* A body we cannot read must not cost us the job.
     *
     * The coinbase transaction is only parsed for display and to report which part
     * of the reward goes to this unit. Nothing about mining depends on it: the work
     * itself comes from the notification's job id, prevhash and merkle root. So when
     * the body cannot be walked - which happens when a pool's padding split differs
     * from what BIP 34 describes - the transaction is skipped and the job is kept.
     *
     * Discarding it instead is what made a unit accept exactly one job from a given
     * pool and then sit at zero shares for the rest of its life. */
    bool body_ok = true;
    if (offset + 5 > coinbase_2_len) {
        ESP_LOGE(TAG, "coinbase reject @%d: no room for nSequence (offset %d, len %d)",
                 __LINE__, offset, coinbase_2_len);
        body_ok = false;
    }

    uint32_t nSequence = 0xffffffff;
    uint64_t num_outputs = 0;
    result->output_count = 0;

    if (body_ok) {
        for (int i = 0; i < 4; i++) {
            nSequence |= ((uint32_t) coinbase_2_bin[offset + i]) << (i * 8);
        }
        offset += 4;

        num_outputs = coinbase_decode_varint(coinbase_2_bin, &offset);
    }

    // Parse each output
    for (uint64_t i = 0; body_ok && i < num_outputs && offset < coinbase_2_len; i++) {
        // Read value (8 bytes, little-endian)
        if (offset + 8 > coinbase_2_len) break;

        uint64_t value_satoshis = 0;
        for (int i = 0; i < 8; i++) {
            value_satoshis |= ((uint64_t)coinbase_2_bin[offset + i]) << (i * 8);
        }
        offset += 8;

        // Add to total value
        result->total_value_satoshis += value_satoshis;

        // Read scriptPubKey length
        if (offset >= coinbase_2_len) break;
        uint64_t script_len = coinbase_decode_varint(coinbase_2_bin, &offset);

        if (offset + script_len > coinbase_2_len) break;

        if (decode_coinbase_tx) {
            if (value_satoshis > 0) {            
                char output_address[MAX_ADDRESS_STRING_LEN];
                coinbase_decode_address_from_scriptpubkey(coinbase_2_bin + offset, script_len, output_address, MAX_ADDRESS_STRING_LEN, bech32_hrp, is_testnet);
                bool is_user_address = strncmp(user_address, output_address, strlen(output_address)) == 0;

                if (is_user_address) result->user_value_satoshis += value_satoshis;

                if (i < MAX_COINBASE_TX_OUTPUTS) {
                    strncpy(result->outputs[i].address, output_address, MAX_ADDRESS_STRING_LEN);
                    result->outputs[i].value_satoshis = value_satoshis;
                    result->outputs[i].is_user_output = is_user_address;
                    result->output_count++;
                }
            } else {
                if (i < MAX_COINBASE_TX_OUTPUTS) {
                    coinbase_decode_address_from_scriptpubkey(coinbase_2_bin + offset, script_len, result->outputs[i].address, MAX_ADDRESS_STRING_LEN, bech32_hrp, is_testnet);
                    result->outputs[i].value_satoshis = 0;
                    result->outputs[i].is_user_output = false;
                    result->output_count++;
                }
            }
        }

        offset += script_len;
    }
    
    // Read nLockTime (4 bytes at the end of the transaction) for BIP-54 detection
    uint32_t nLockTime = 0;
    if (offset + 4 <= coinbase_2_len) {
        for (int i = 0; i < 4; i++) {
            nLockTime |= ((uint32_t) coinbase_2_bin[offset + i]) << (i * 8);
        }
    }
    
    // Detect BIP-54 signaling: nLockTime = block_height - 1 AND nSequence != 0xffffffff
    result->bip54_signaling = decode_coinbase_tx && (nLockTime == result->block_height - 1) && (nSequence != 0xffffffff);
    
    free(coinbase_2_bin);
    return ESP_OK;
}
