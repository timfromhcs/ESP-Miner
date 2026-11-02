#ifndef RESEARCH_CONSOLE_PARSE_H_
#define RESEARCH_CONSOLE_PARSE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "research_sha.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Line parser for the research console (USB-Serial/JTAG).
 *
 * The console is the second way to drive the same session the HTTP routes drive.
 * It exists because the operator asked for "research over USB": the pairing token
 * is already printed there, and with this a session can be run without any WLAN at
 * all. It is not a second authority model - the same guard, the same token, the
 * same budgets. It removes the network from the loop, not the checks.
 *
 * Parsing is separated from execution so every shape a human might type can be
 * exercised on the host, including the ones that must be refused:
 *
 *   research status
 *   research challenge
 *   research arm 482913
 *   research pause | resume | cancel | disarm
 *   research trace <128 hex> [rounds]
 *   research digest <hex> [sha256d]
 *   research help
 */

#define RESEARCH_CONSOLE_MAX_MESSAGE 512

typedef enum {
    RESEARCH_CONSOLE_STATUS = 0,
    RESEARCH_CONSOLE_CHALLENGE,
    RESEARCH_CONSOLE_ARM,
    RESEARCH_CONSOLE_PAUSE,
    RESEARCH_CONSOLE_RESUME,
    RESEARCH_CONSOLE_CANCEL,
    RESEARCH_CONSOLE_DISARM,
    RESEARCH_CONSOLE_TRACE,
    RESEARCH_CONSOLE_BLOCK,
    RESEARCH_CONSOLE_DIGEST,
    RESEARCH_CONSOLE_MINING_OFF,
    RESEARCH_CONSOLE_MINING_ON,
    RESEARCH_CONSOLE_HELP,
    RESEARCH_CONSOLE_EMPTY,      /* blank line: not an error, do nothing */
    RESEARCH_CONSOLE_INVALID,    /* unknown command or bad argument: say why */
} research_console_command_t;

typedef struct {
    research_console_command_t command;
    uint32_t token;                                  /* ARM */
    uint8_t block[RESEARCH_SHA_BLOCK_LEN];           /* TRACE, BLOCK */
    size_t block_len;
    unsigned block_index;                            /* BLOCK: 0..3 */
    unsigned rounds;                                 /* TRACE, default 64 */
    bool feed_forward;                               /* TRACE, default true */
    uint8_t message[RESEARCH_CONSOLE_MAX_MESSAGE];   /* DIGEST */
    size_t message_len;
    bool sha256d;                                    /* DIGEST */
} research_console_request_t;

/* Bytes covered by one uploaded chunk. The USB-Serial/JTAG console loses
 * characters when a single line is longer than about 64 bytes (measured: a
 * 97-character line arrives corrupted, a 57-character line does not), so a
 * 128-hex-character block cannot be typed in one go. Four chunks of 32 hex
 * characters, each on its own short line, do arrive intact. */
#define RESEARCH_CONSOLE_CHUNK_BYTES 16

/* Writes one chunk into `block`, which must be 64 bytes; `have` tracks how many
 * bytes are already present. Returns 0 on success, -1 if the chunk is out of
 * range or does not complete a prefix of the block. Pure, so the console task and
 * the host tests share exactly one implementation. */
int research_console_block_put(uint8_t *block, size_t *have, unsigned index, const uint8_t *bytes, size_t len);

/* Parses one line. Returns 0 when the request is well formed (or empty), -1 when
 * it must be refused; `error` then holds a one-line explanation suitable for a
 * console, and is always NUL-terminated when `error_len` > 0. */
int research_console_parse(const char *line, size_t len, research_console_request_t *out, char *error, size_t error_len);

/* The help text, one entry per line, so the console and the docs cannot drift. */
const char *research_console_help(void);

#ifdef __cplusplus
}
#endif

#endif /* RESEARCH_CONSOLE_PARSE_H_ */
