#include "research_console_parse.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static const char *const HELP =
    "research status                     - session and device state\n"
    "research challenge                  - issue a pairing token (printed here)\n"
    "research arm <6 digits>             - consume the token and arm the session\n"
    "research pause | resume | cancel     - session transitions\n"
    "research mining off | on            - stop/start mining (the same flag the HTTP route sets)\n"
    "research disarm                     - end the session\n"
    "research trace [rounds]              - trace the block uploaded with 'research block'\n"
    "research trace <128 hex> [rounds]    - one-shot form, only where the console keeps long lines\n"
    "research block <0-3> <32 hex>        - upload 16 bytes of the 64-byte block\n"
    "research digest <hex> [sha256d]      - software digest of a message (<= 512 bytes)\n"
    "research help                       - this text\n";

const char *research_console_help(void)
{
    return HELP;
}

int research_console_block_put(uint8_t *block, size_t *have, unsigned index, const uint8_t *bytes, size_t len)
{
    if (block == NULL || have == NULL || bytes == NULL || len == 0) {
        return -1;
    }
    if (index * RESEARCH_CONSOLE_CHUNK_BYTES >= RESEARCH_SHA_BLOCK_LEN) {
        return -1;
    }
    size_t offset = (size_t) index * RESEARCH_CONSOLE_CHUNK_BYTES;
    if (offset + len > RESEARCH_SHA_BLOCK_LEN) {
        return -1;
    }
    /* Chunks must arrive in order and without gaps: a silent hole in the block
     * would produce a trace of a message the caller never sent. */
    if (offset > *have) {
        return -1;
    }
    memcpy(block + offset, bytes, len);
    if (offset + len > *have) {
        *have = offset + len;
    }
    return 0;
}

static void set_error(char *error, size_t error_len, const char *text)
{
    if (error == NULL || error_len == 0) {
        return;
    }
    size_t n = strlen(text);
    if (n >= error_len) {
        n = error_len - 1;
    }
    memcpy(error, text, n);
    error[n] = '\0';
}

/* Parses a decimal uint32. Rejects empty, signed, trailing junk and overflow. */
static int parse_u32(const char *text, uint32_t *out)
{
    if (text == NULL || *text == '\0') {
        return -1;
    }
    /* strtoul() accepts a leading sign and wraps a negative value into a huge
     * unsigned one, which would sail past the range check below on 32-bit
     * "unsigned long". A token or a round count is never signed. */
    if (*text == '+' || *text == '-') {
        return -1;
    }
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > 0xFFFFFFFFul) {
        return -1;
    }
    *out = (uint32_t) value;
    return 0;
}

int research_console_parse(const char *line, size_t len, research_console_request_t *out, char *error, size_t error_len)
{
    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->rounds = RESEARCH_SHA_ROUNDS;
    out->feed_forward = true;

    if (line == NULL || len == 0) {
        out->command = RESEARCH_CONSOLE_EMPTY;
        return 0;
    }

    /* Trailing CR (CRLF consoles) and spaces are noise, not syntax errors. */
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' || line[len - 1] == ' ')) {
        len--;
    }
    if (len == 0) {
        out->command = RESEARCH_CONSOLE_EMPTY;
        return 0;
    }

    /* The first word must be "research"; anything else is refused so a stray
       keystroke cannot be mistaken for a command. */
    const char *p = line;
    const size_t prefix_len = strlen("research");
    if (len < prefix_len || memcmp(p, "research", prefix_len) != 0 || (len > prefix_len && line[prefix_len] != ' ' && line[prefix_len] != '\t')) {
        out->command = RESEARCH_CONSOLE_INVALID;
        set_error(error, error_len, "unknown command; type 'research help'");
        return -1;
    }
    size_t pos = prefix_len;
    while (pos < len && (line[pos] == ' ' || line[pos] == '\t')) {
        pos++;
    }
    if (pos >= len) {
        out->command = RESEARCH_CONSOLE_HELP;
        return 0;
    }

    size_t word_start = pos;
    while (pos < len && line[pos] != ' ' && line[pos] != '\t') {
        pos++;
    }
    size_t word_len = pos - word_start;
    char word[16];
    if (word_len >= sizeof(word)) {
        out->command = RESEARCH_CONSOLE_INVALID;
        set_error(error, error_len, "unknown command; type 'research help'");
        return -1;
    }
    memcpy(word, line + word_start, word_len);
    word[word_len] = '\0';

    /* Remaining arguments, parsed one at a time. */
    char arg[RESEARCH_CONSOLE_MAX_MESSAGE * 2 + 64];
    size_t arg_start;
    bool has_arg;

#define NEXT_ARG()                                                                                                       \
    do {                                                                                                                 \
        while (pos < len && (line[pos] == ' ' || line[pos] == '\t')) {                                                   \
            pos++;                                                                                                       \
        }                                                                                                                \
        arg_start = pos;                                                                                                 \
        while (pos < len && line[pos] != ' ' && line[pos] != '\t') {                                                     \
            pos++;                                                                                                       \
        }                                                                                                                \
        has_arg = pos > arg_start;                                                                                       \
        if (has_arg) {                                                                                                   \
            size_t n = pos - arg_start;                                                                                  \
            if (n >= sizeof(arg)) {                                                                                      \
                out->command = RESEARCH_CONSOLE_INVALID;                                                                  \
                set_error(error, error_len, "argument too long");                                                        \
                return -1;                                                                                               \
            }                                                                                                            \
            memcpy(arg, line + arg_start, n);                                                                            \
            arg[n] = '\0';                                                                                               \
        }                                                                                                                \
    } while (0)

    if (strcmp(word, "status") == 0) {
        out->command = RESEARCH_CONSOLE_STATUS;
        return 0;
    }
    if (strcmp(word, "help") == 0) {
        out->command = RESEARCH_CONSOLE_HELP;
        return 0;
    }
    if (strcmp(word, "challenge") == 0) {
        out->command = RESEARCH_CONSOLE_CHALLENGE;
        return 0;
    }
    if (strcmp(word, "pause") == 0) {
        out->command = RESEARCH_CONSOLE_PAUSE;
        return 0;
    }
    if (strcmp(word, "resume") == 0) {
        out->command = RESEARCH_CONSOLE_RESUME;
        return 0;
    }
    if (strcmp(word, "cancel") == 0) {
        out->command = RESEARCH_CONSOLE_CANCEL;
        return 0;
    }
    if (strcmp(word, "disarm") == 0) {
        out->command = RESEARCH_CONSOLE_DISARM;
        return 0;
    }
    if (strcmp(word, "mining") == 0) {
        /* Stopping mining is what makes the session legal at all, and a console
         * session must not depend on the network for it. Same flag the HTTP pause
         * route sets, nothing else. */
        NEXT_ARG();
        if (!has_arg || (strcmp(arg, "off") != 0 && strcmp(arg, "on") != 0)) {
            out->command = RESEARCH_CONSOLE_INVALID;
            set_error(error, error_len, "usage: research mining off | on");
            return -1;
        }
        out->command = (arg[1] == 'f') ? RESEARCH_CONSOLE_MINING_OFF : RESEARCH_CONSOLE_MINING_ON;
        return 0;
    }
    if (strcmp(word, "arm") == 0) {
        NEXT_ARG();
        if (!has_arg) {
            out->command = RESEARCH_CONSOLE_INVALID;
            set_error(error, error_len, "usage: research arm <6 digits>");
            return -1;
        }
        if (parse_u32(arg, &out->token) != 0 || out->token == 0) {
            out->command = RESEARCH_CONSOLE_INVALID;
            set_error(error, error_len, "token must be a positive decimal integer");
            return -1;
        }
        out->command = RESEARCH_CONSOLE_ARM;
        return 0;
    }
    if (strcmp(word, "block") == 0) {
        NEXT_ARG();
        if (!has_arg) {
            out->command = RESEARCH_CONSOLE_INVALID;
            set_error(error, error_len, "usage: research block <0-3> <32 hex>");
            return -1;
        }
        uint32_t index = 0;
        if (parse_u32(arg, &index) != 0 || index > 3) {
            out->command = RESEARCH_CONSOLE_INVALID;
            set_error(error, error_len, "block index must be 0..3");
            return -1;
        }
        NEXT_ARG();
        if (!has_arg) {
            out->command = RESEARCH_CONSOLE_INVALID;
            set_error(error, error_len, "usage: research block <0-3> <32 hex>");
            return -1;
        }
        int n = research_sha_hex_to_bytes(arg, out->block, sizeof(out->block));
        if (n <= 0 || (size_t) n > RESEARCH_CONSOLE_CHUNK_BYTES) {
            out->command = RESEARCH_CONSOLE_INVALID;
            set_error(error, error_len, "one block chunk is 32 hex characters (16 bytes)");
            return -1;
        }
        out->block_index = (unsigned) index;
        out->block_len = (size_t) n;
        out->command = RESEARCH_CONSOLE_BLOCK;
        return 0;
    }
    if (strcmp(word, "trace") == 0) {
        NEXT_ARG();
        if (!has_arg) {
            /* The uploaded block, with the default round count. */
            out->command = RESEARCH_CONSOLE_TRACE;
            out->block_len = 0;
            return 0;
        }
        /* A bare small number is a round count for the uploaded block; anything
         * else has to be the whole 128-hex-character block. */
        bool digits_only = strlen(arg) <= 2;
        for (size_t i = 0; digits_only && arg[i] != '\0'; i++) {
            if (arg[i] < '0' || arg[i] > '9') {
                digits_only = false;
            }
        }
        if (digits_only) {
            uint32_t rounds = 0;
            if (parse_u32(arg, &rounds) != 0 || rounds > RESEARCH_SHA_ROUNDS) {
                out->command = RESEARCH_CONSOLE_INVALID;
                set_error(error, error_len, "rounds must be an integer 0..64");
                return -1;
            }
            out->command = RESEARCH_CONSOLE_TRACE;
            out->block_len = 0;
            out->rounds = (unsigned) rounds;
            return 0;
        }
        int n = research_sha_hex_to_bytes(arg, out->block, sizeof(out->block));
        if (n != RESEARCH_SHA_BLOCK_LEN) {
            out->command = RESEARCH_CONSOLE_INVALID;
            set_error(error, error_len, "trace needs a 64-byte block: upload it with 'research block' or pass 128 hex characters");
            return -1;
        }
        out->block_len = (size_t) n;
        NEXT_ARG();
        if (has_arg) {
            uint32_t rounds = 0;
            if (parse_u32(arg, &rounds) != 0 || rounds > RESEARCH_SHA_ROUNDS) {
                out->command = RESEARCH_CONSOLE_INVALID;
                set_error(error, error_len, "rounds must be an integer 0..64");
                return -1;
            }
            out->rounds = (unsigned) rounds;
        }
        out->command = RESEARCH_CONSOLE_TRACE;
        return 0;
    }
    if (strcmp(word, "digest") == 0) {
        NEXT_ARG();
        if (!has_arg) {
            out->command = RESEARCH_CONSOLE_INVALID;
            set_error(error, error_len, "usage: research digest <hex> [sha256d]");
            return -1;
        }
        int n = research_sha_hex_to_bytes(arg, out->message, sizeof(out->message));
        if (n < 0) {
            out->command = RESEARCH_CONSOLE_INVALID;
            set_error(error, error_len, "message must be even-length hex of at most 512 bytes");
            return -1;
        }
        out->message_len = (size_t) n;
        NEXT_ARG();
        if (has_arg) {
            if (strcmp(arg, "sha256d") != 0) {
                out->command = RESEARCH_CONSOLE_INVALID;
                set_error(error, error_len, "second argument must be 'sha256d'");
                return -1;
            }
            out->sha256d = true;
        }
        out->command = RESEARCH_CONSOLE_DIGEST;
        return 0;
    }

#undef NEXT_ARG

    out->command = RESEARCH_CONSOLE_INVALID;
    set_error(error, error_len, "unknown command; type 'research help'");
    return -1;
}
