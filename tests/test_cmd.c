/* test_cmd.c — headless tests for command tokenizing and key-name parsing. */
#include "cmd.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do {                                   \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); failures++; }   \
    else         { printf("ok:   %s\n", (msg)); }               \
} while (0)

static void test_tokenize(void)
{
    char storage[128];
    char *argv[CMD_MAX_ARGS];
    int argc;

    argc = cmd_tokenize("split-window -h", storage, sizeof(storage), argv, CMD_MAX_ARGS);
    CHECK(argc == 2, "two tokens");
    CHECK(strcmp(argv[0], "split-window") == 0, "argv[0]");
    CHECK(strcmp(argv[1], "-h") == 0, "argv[1]");

    argc = cmd_tokenize("bind X new-window", storage, sizeof(storage), argv, CMD_MAX_ARGS);
    CHECK(argc == 3, "three tokens");
    CHECK(strcmp(argv[2], "new-window") == 0, "argv[2]");

    argc = cmd_tokenize("rename-window \"my win\"", storage, sizeof(storage), argv, CMD_MAX_ARGS);
    CHECK(argc == 2, "quoted arg counts as one token");
    CHECK(strcmp(argv[1], "my win") == 0, "quoted arg keeps its space");

    argc = cmd_tokenize("   ", storage, sizeof(storage), argv, CMD_MAX_ARGS);
    CHECK(argc == 0, "whitespace-only line -> no tokens");
}

static void test_keys(void)
{
    CHECK(cmd_parse_key("C-b") == 0x02, "C-b == Ctrl-B");
    CHECK(cmd_parse_key("^A") == 0x01, "^A == Ctrl-A");
    CHECK(cmd_parse_key("%") == '%', "single char key");
    CHECK(cmd_parse_key("Up") == KEY_UP, "Up arrow");
    CHECK(cmd_parse_key("Down") == KEY_DOWN, "Down arrow");
    CHECK(cmd_parse_key("Enter") == '\r', "Enter");
    CHECK(cmd_parse_key("Space") == ' ', "Space");
    CHECK(cmd_parse_key("nope!") == -1, "unknown key -> -1");

    {
        char buf[16];
        cmd_key_name(KEY_UP, buf, sizeof(buf));
        CHECK(strcmp(buf, "Up") == 0, "key name for Up");
        cmd_key_name('%', buf, sizeof(buf));
        CHECK(strcmp(buf, "%") == 0, "key name for '%'");
        cmd_key_name(0x02, buf, sizeof(buf));
        CHECK(strcmp(buf, "C-b") == 0, "key name for Ctrl-B");
    }
}

int main(void)
{
    test_tokenize();
    test_keys();

    if (failures == 0) { printf("\nALL PASSED\n"); return 0; }
    printf("\n%d FAILURE(S)\n", failures);
    return 1;
}
