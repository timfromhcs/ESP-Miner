/*
 * Host tests for the research console parser (main/research/research_console_parse.c).
 *
 * The console is the USB path into the same session the HTTP routes drive, so its
 * parser has to be as unforgiving as the HTTP one: a typo must be refused with a
 * reason, never guessed into a command. Every acceptance and every refusal below
 * is a case someone will actually type at a serial prompt.
 */

#include "research_console_parse.h"

#include <stdio.h>
#include <string.h>

static int checks = 0;
static int failures = 0;

static void check(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

static int parse(const char *line, research_console_request_t *req, char *err, size_t err_len)
{
    return research_console_parse(line, strlen(line), req, err, err_len);
}

static void expect_ok(const char *line, research_console_command_t want, const char *what)
{
    research_console_request_t req;
    char err[128] = { 0 };
    int rc = parse(line, &req, err, sizeof(err));
    checks++;
    if (rc != 0 || req.command != want) {
        failures++;
        printf("  FAIL: %s (rc=%d command=%d err=%s)\n", what, rc, (int) req.command, err);
    }
}

static void expect_error(const char *line, const char *needle, const char *what)
{
    research_console_request_t req;
    char err[128] = { 0 };
    int rc = parse(line, &req, err, sizeof(err));
    checks++;
    if (rc == 0) {
        failures++;
        printf("  FAIL: %s (accepted, command=%d)\n", what, (int) req.command);
        return;
    }
    checks++;
    if (strstr(err, needle) == NULL) {
        failures++;
        printf("  FAIL: %s (error %.*s does not mention %s)\n", what, (int) (sizeof(err) - 1), err, needle);
    }
}

static void test_commands(void)
{
    printf("== command words ==\n");
    expect_ok("research status", RESEARCH_CONSOLE_STATUS, "status");
    expect_ok("research help", RESEARCH_CONSOLE_HELP, "help");
    expect_ok("research", RESEARCH_CONSOLE_HELP, "bare 'research' shows help");
    expect_ok("research challenge", RESEARCH_CONSOLE_CHALLENGE, "challenge");
    expect_ok("research pause", RESEARCH_CONSOLE_PAUSE, "pause");
    expect_ok("research resume", RESEARCH_CONSOLE_RESUME, "resume");
    expect_ok("research cancel", RESEARCH_CONSOLE_CANCEL, "cancel");
    expect_ok("research disarm", RESEARCH_CONSOLE_DISARM, "disarm");
    expect_ok("research mining off", RESEARCH_CONSOLE_MINING_OFF, "mining off");
    expect_ok("research mining on", RESEARCH_CONSOLE_MINING_ON, "mining on");
    expect_error("research mining", "usage", "mining without an argument");
    expect_error("research mining sideways", "usage", "mining with an unknown argument");
    expect_ok("research  status ", RESEARCH_CONSOLE_STATUS, "extra whitespace tolerated");
    expect_ok("research\r", RESEARCH_CONSOLE_HELP, "bare CR trimmed");

    research_console_request_t req;
    char err[128];
    check(research_console_parse("", 0, &req, err, sizeof(err)) == 0 && req.command == RESEARCH_CONSOLE_EMPTY, "empty line is a no-op");
    check(research_console_parse("\r\n", 2, &req, err, sizeof(err)) == 0 && req.command == RESEARCH_CONSOLE_EMPTY, "CRLF-only line is a no-op");
}

static void test_arm(void)
{
    printf("== arm ==\n");
    research_console_request_t req;
    char err[128];
    check(parse("research arm 482913", &req, err, sizeof(err)) == 0 && req.command == RESEARCH_CONSOLE_ARM && req.token == 482913u,
          "arm parses the token");
    check(parse("research arm 1", &req, err, sizeof(err)) == 0 && req.token == 1u, "short token accepted");

    expect_error("research arm", "usage", "arm without a token");
    expect_error("research arm abc", "decimal", "non-numeric token");
    expect_error("research arm 0", "positive", "zero token");
    expect_error("research arm -5", "decimal", "negative token");
    expect_error("research arm 4294967296", "decimal", "token beyond 32 bits");
    expect_error("research arm 12x", "decimal", "trailing junk");
}

static void test_trace(void)
{
    printf("== trace ==\n");
    static const char *block = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
                               "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f";
    research_console_request_t req;
    char err[128];

    char line[256];
    snprintf(line, sizeof(line), "research trace %s", block);
    check(parse(line, &req, err, sizeof(err)) == 0 && req.command == RESEARCH_CONSOLE_TRACE, "trace accepted");
    check(req.block_len == 64 && req.block[0] == 0x00 && req.block[63] == 0x3f, "block decoded");
    check(req.rounds == 64 && req.feed_forward, "defaults: 64 rounds, feed-forward");

    snprintf(line, sizeof(line), "research trace %s 7", block);
    check(parse(line, &req, err, sizeof(err)) == 0 && req.rounds == 7, "explicit round count");
    snprintf(line, sizeof(line), "research trace %s 0", block);
    check(parse(line, &req, err, sizeof(err)) == 0 && req.rounds == 0, "zero rounds accepted");

    check(parse("research trace", &req, err, sizeof(err)) == 0 && req.block_len == 0, "bare trace is valid: it uses the uploaded block");
    expect_error("research trace 0011", "64-byte block", "short block");
    expect_error("research trace zz", "64-byte block", "non-hex block");

    snprintf(line, sizeof(line), "research trace %s 65", block);
    expect_error(line, "0..64", "round count above 64");
    snprintf(line, sizeof(line), "research trace %s -1", block);
    expect_error(line, "0..64", "negative round count");
}

static void test_digest(void)
{
    printf("== digest ==\n");
    research_console_request_t req;
    char err[128];

    check(parse("research digest 616263", &req, err, sizeof(err)) == 0, "digest accepted");
    check(req.command == RESEARCH_CONSOLE_DIGEST && req.message_len == 3 && !req.sha256d, "message decoded, sha256 by default");
    check(parse("research digest 616263 sha256d", &req, err, sizeof(err)) == 0 && req.sha256d, "sha256d flag");
    check(parse("research digest  zz ", &req, err, sizeof(err)) != 0, "non-hex message refused");

    expect_error("research digest", "usage", "digest without a message");
    expect_error("research digest 616263 sha256", "sha256d", "unknown second argument");
    expect_error("research digest 0", "even-length", "odd-length hex");

    /* 513 bytes of hex is over the 512-byte message limit. */
    static char long_line[4 + 8 + 1026 + 8];
    strcpy(long_line, "research digest ");
    for (int i = 0; i < 513; i++) {
        strcat(long_line, "61");
    }
    expect_error(long_line, "at most 512", "message above 512 bytes");
}

static void test_block_chunks(void)
{
    printf("== block chunks ==\n");
    uint8_t block[RESEARCH_SHA_BLOCK_LEN];
    size_t have = 0;
    uint8_t chunk[RESEARCH_CONSOLE_CHUNK_BYTES];
    for (size_t i = 0; i < sizeof(chunk); i++) {
        chunk[i] = (uint8_t) (0x10 + i);
    }

    check(research_console_block_put(block, &have, 0, chunk, sizeof(chunk)) == 0 && have == 16, "chunk 0 accepted");
    check(research_console_block_put(block, &have, 2, chunk, sizeof(chunk)) != 0, "chunk 2 before 1 is refused (no gaps)");
    check(research_console_block_put(block, &have, 1, chunk, sizeof(chunk)) == 0 && have == 32, "chunk 1 accepted");
    check(block[0] == 0x10 && block[16] == 0x10 && block[31] == 0x1f, "bytes land at the right offsets");
    check(research_console_block_put(block, &have, 4, chunk, sizeof(chunk)) != 0, "index 4 is out of range");
    check(research_console_block_put(block, &have, 3, chunk, 24) != 0, "a chunk longer than 16 bytes is refused");

    for (unsigned i = 2; i < 4; i++) {
        check(research_console_block_put(block, &have, i, chunk, sizeof(chunk)) == 0, "remaining chunks accepted");
    }
    check(have == RESEARCH_SHA_BLOCK_LEN, "all 64 bytes present");

    research_console_request_t req;
    char err[128];
    check(parse("research block 0 000102030405060708090a0b0c0d0e0f", &req, err, sizeof(err)) == 0, "block command parses");
    check(req.command == RESEARCH_CONSOLE_BLOCK && req.block_index == 0 && req.block_len == 16, "block fields");
    check(parse("research trace 1", &req, err, sizeof(err)) == 0 && req.command == RESEARCH_CONSOLE_TRACE && req.rounds == 1 && req.block_len == 0,
          "a short number is a round count for the uploaded block");
    expect_error("research block 4 0011", "0..3", "block index above 3");
    check(parse("research block 0 0011", &req, err, sizeof(err)) == 0 && req.block_len == 2, "a short chunk is accepted; completeness is checked at trace time");
    check(parse("research trace", &req, err, sizeof(err)) == 0 && req.command == RESEARCH_CONSOLE_TRACE && req.block_len == 0, "bare trace uses the uploaded block");
}
static void test_garbage(void)
{
    printf("== garbage ==\n");
    research_console_request_t req;
    char err[128];
    expect_error("hello", "unknown command", "unprefixed line");
    expect_error("researchfoo status", "unknown command", "prefix without a separator");
    expect_error("research statuss", "unknown command", "misspelled command");
    check(parse("research trace", &req, err, sizeof(err)) == 0, "bare trace is a valid command shape");
    check(strstr(research_console_help(), "research trace") != NULL, "help mentions trace");
    check(strstr(research_console_help(), "research arm") != NULL, "help mentions arm");
}

int main(void)
{
    test_commands();
    test_arm();
    test_trace();
    test_digest();
    test_block_chunks();
    test_garbage();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
