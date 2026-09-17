/* ledctl.c - control board LEDs via /sys/class/leds/
 * Usage: ledctl [-d <led>] <verb> [args...]
 */
#include <linux/limits.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>

#define EXIT_USAGE   2
#define EXIT_RUNTIME 1

static const char *g_sysfs_root; /* $LEDCTL_SYSFS 或 /sys/class/leds */

/* ---- sysfs 基本操作 ---- */

/* 讀 <led_path>/<attr>，去掉結尾換行放進 buf。成功回 0，失敗回 -1（errno 已設） */
static bool read_attr(const char *led_path, const char *attr, char *buf, size_t bufsz) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", led_path, attr);

    FILE *f = fopen(path, "r");
    if (f == NULL) return false;

    if (fgets(buf, bufsz, f) == NULL){
        fclose(f);
        return false;
    }
    fclose(f);
    
    char *nl = strchr(buf, '\n');
    if (nl != NULL){
        *nl = '\0';
    }

    return true;
}

/* 把 value + "\n" 寫進 <led_path>/<attr>。成功回 0，失敗回 -1 */
static bool write_attr(const char *led_path, const char *attr, const char *value) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", led_path, attr);

    FILE *f = fopen(path, "w");
    if (f == NULL) return false;

    fprintf(f, "%s\n", value);
    fclose(f);

    return true;
}

/* <led_path>/<attr> 存在嗎（給 delay_on/off 這種動態出現的檔用） */
static bool attr_exists(const char *led_path, const char *attr) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", led_path, attr);

    return access(path, F_OK) == 0;
}

/* 讀 trigger 檔，取出 [方括號] 裡目前值放進 buf */
static bool current_trigger(const char *led_path, char *buf, size_t bufsz) {
    char line[256];
    if (read_attr(led_path, "trigger", line, sizeof(line)) != true) return false;

    char *open = strchr(line, '[');
    if (open == NULL) return false;
    char *close = strchr(open, ']'); // 從 open 接著找比較快
    if (close == NULL) return false;

    size_t len = close - open - 1;
    if (len >= bufsz) len = bufsz - 1;

    memcpy(buf, open + 1, len);
    buf[len] = '\0';

    return true;
}

/* name 是否在 trigger 檔那串空白分隔的清單裡（含目前值） */
static bool trigger_supported(const char *led_path, const char *name) {
    /* TODO: read_attr 整行 -> strtok(" ") -> 逐個比對（比對前先去掉 [ ]） */
    char line[256];
    if (read_attr(led_path, "trigger", line, sizeof(line)) != true) return false;

    // 替換掉 [ ]
    for (char *p = line; *p != '\0'; p++){
        if (*p == '[' || *p == ']') *p = ' ';
    }

    char *tok = strtok(line, " ");
    while (tok != NULL) {
        if (strcmp(tok, name) == 0) return true; // strcmp 才能比對指標指向的位置的值, ==比對的是是不是指向同一個位置
        tok = strtok(NULL, " "); // 接著往後切不要開新的 tok
    }

    return false;
}

/* ---- LED 解析 ---- */

/* 把 hint（子字串，可為 NULL）解析成 g_sysfs_root 下唯一一個 LED 目錄，
 * 完整路徑寫進 out。成功回 0；命中 0 或多個時印錯誤（提示 ledctl list）回 -1 */
static bool resolve_led(const char *hint, char *out, size_t outsz) {
    /* TODO: opendir(g_sysfs_root) -> readdir 逐一比對 -> 計數 -> 唯一才成功 */
    DIR *d = opendir(g_sysfs_root);
    if (d == NULL) return false;

    struct dirent *entry;
    int n = 0;
    while((entry = readdir(d)) != NULL){
        if (strcmp(entry -> d_name, ".") == 0 || strcmp(entry -> d_name, "..") == 0) continue;
        if (hint == NULL || strstr(entry -> d_name, hint)) {
            snprintf(out, outsz, "%s/%s", g_sysfs_root, entry -> d_name);
            n++;
        }
    }

    closedir(d);

    return n == 1;
}

/* ---- verbs ---- */

static int cmd_list(void) {
    /* TODO: 對 g_sysfs_root 下每個 LED：印名字 + current_trigger + brightness/max */
    DIR *d = opendir(g_sysfs_root);
    if (d == NULL) return EXIT_RUNTIME;

    struct dirent *entry;
    while((entry = readdir(d)) != NULL){
        if (strcmp(entry -> d_name, ".") == 0 || strcmp(entry -> d_name, "..") == 0) continue;

        char led_path[PATH_MAX];
        snprintf(led_path, sizeof(led_path), "%s/%s", g_sysfs_root, entry -> d_name);

        char trig[64] = "";
        current_trigger(led_path, trig, sizeof(trig));
        
        char brightness[16] = "";
        read_attr(led_path, "brightness", brightness, sizeof(brightness));

        char maxb[16] = "";
        read_attr(led_path, "max_brightness", maxb, sizeof(maxb));

        printf("%-20s: trigger=%s brightness=%s/%s\n", entry->d_name, trig, brightness, maxb);
    }

    closedir(d);
    return 0;
}

static int cmd_status(const char *led_path) {
    char trig[64] = "";
    if (!current_trigger(led_path, trig, sizeof(trig))) {
        return EXIT_RUNTIME;
    }

    char brightness[16] = "";
    char maxb[16] = "";
    read_attr(led_path, "brightness", brightness, sizeof(brightness));
    read_attr(led_path, "max_brightness", maxb, sizeof(maxb));

    printf("trigger: %s\n", trig);
    printf("brightness: %s/%s\n", brightness, maxb);

    if (strcmp(trig, "timer") == 0) {
        char delay_on[16] = "", delay_off[16] = "";
        read_attr(led_path, "delay_on", delay_on, sizeof(delay_on));
        read_attr(led_path, "delay_off", delay_off, sizeof(delay_off));
        printf("delay_on: %s ms\n", delay_on);
        printf("delay_off: %s ms\n", delay_off);
    } else if (strcmp(trig, "heartbeat") == 0) {
        char invert[16] = "";
        read_attr(led_path, "invert", invert, sizeof(invert));
        printf("invert: %s\n", invert);
    }

    return 0;
}

static int cmd_on(const char *led_path) {
    /* 先關掉自動控制，不然 heartbeat/timer 會馬上把亮度改回去 */
    if (!write_attr(led_path, "trigger", "none")) {
        return EXIT_RUNTIME;
    }

    /* 讀「這顆 LED 最亮能到多少」，而不是寫死 255（有些 LED 的 max_brightness 不是 255） */
    char maxb[16];
    if (!read_attr(led_path, "max_brightness", maxb, sizeof(maxb))) {
        return EXIT_RUNTIME;
    }

    if (!write_attr(led_path, "brightness", maxb)) {
        return EXIT_RUNTIME;
    }

    return 0;
}

static int cmd_off(const char *led_path) {
    if (!write_attr(led_path, "trigger", "none")){
        return EXIT_RUNTIME;
    }

    if (!write_attr(led_path, "brightness", "0")){
        return EXIT_RUNTIME;
    }
    
    return 0;
}

static int cmd_blink(const char *led_path, const char *on_ms, const char *off_ms) {
    char *end;

    long on_val = strtol(on_ms, &end, 10);
    if (*end != '\0' || on_val < 0) {
        fprintf(stderr, "blink: on_ms 必須是非負整數\n");
        return EXIT_USAGE;
    }

    long off_val = strtol(off_ms, &end, 10);
    if (*end != '\0' || off_val < 0) {
        fprintf(stderr, "blink: off_ms 必須是非負整數\n");
        return EXIT_USAGE;
    }

    if (!trigger_supported(led_path, "timer")) {
        fprintf(stderr, "blink: 這顆 LED 不支援 timer trigger\n");
        return EXIT_RUNTIME;
    }

    if (!write_attr(led_path, "trigger", "timer")) {
        return EXIT_RUNTIME;
    }

    if (!attr_exists(led_path, "delay_on") || !attr_exists(led_path, "delay_off")) {
        return EXIT_RUNTIME;
    }

    char on_str[24], off_str[24];
    snprintf(on_str, sizeof(on_str), "%ld", on_val);
    snprintf(off_str, sizeof(off_str), "%ld", off_val);

    if (!write_attr(led_path, "delay_on", on_str)) {
        return EXIT_RUNTIME;
    }
    if (!write_attr(led_path, "delay_off", off_str)) {
        return EXIT_RUNTIME;
    }

    return 0;
}

static int cmd_heartbeat(const char *led_path) {
    /* TODO: trigger_supported("heartbeat") -> write_attr(trigger,"heartbeat") */
    if (!trigger_supported(led_path, "heartbeat")){
        return EXIT_RUNTIME;
    }

    if (!write_attr(led_path, "trigger", "heartbeat")){
        return EXIT_RUNTIME;
    }

    return 0;
}

static int cmd_trigger(const char *led_path, const char *name) {
    if (!trigger_supported(led_path, name)){
        return EXIT_RUNTIME;
    }

    if (!write_attr(led_path, "trigger", name)){
        return EXIT_RUNTIME;
    }

    return 0;
}

/* ---- help ---- */

static void usage(void) {
    fprintf(stderr,
        "usage: ledctl [-d <led>] <verb> [args...]\n"
        "verbs: list status on off blink heartbeat trigger\n"
        "run 'ledctl <verb> -h' for details on one verb\n");
}

static void usage_verb(const char *verb) {
    if (strcmp(verb, "list") == 0) {
        fprintf(stderr, "usage: ledctl list\n  列出所有 LED 及狀態摘要\n");
    } else if (strcmp(verb, "status") == 0) {
        fprintf(stderr, "usage: ledctl [-d <led>] status\n  顯示這顆 LED 的完整狀態\n");
    } else if (strcmp(verb, "on") == 0) {
        fprintf(stderr, "usage: ledctl [-d <led>] on\n  恆亮\n");
    } else if (strcmp(verb, "off") == 0) {
        fprintf(stderr, "usage: ledctl [-d <led>] off\n  熄滅\n");
    } else if (strcmp(verb, "blink") == 0) {
        fprintf(stderr, "usage: ledctl [-d <led>] blink <on_ms> <off_ms>\n"
                         "  固定頻率閃爍\n"
                         "  例: ledctl blink 200 800\n");
    } else if (strcmp(verb, "heartbeat") == 0) {
        fprintf(stderr, "usage: ledctl [-d <led>] heartbeat\n  心跳模式\n");
    } else if (strcmp(verb, "trigger") == 0) {
        fprintf(stderr, "usage: ledctl [-d <led>] trigger <name>\n"
                         "  直接掛任意 trigger（逃生門）\n"
                         "  例: ledctl trigger mmc0\n");
    } else {
        usage();
    }
}

/* ---- main ---- */

int main(int argc, char **argv) {

    g_sysfs_root = getenv("LEDCTL_SYSFS");
    if (!g_sysfs_root) g_sysfs_root = "/sys/class/leds";

    const char *led_hint = NULL;
    int argi = 1;

    if (argc >= 2 && strcmp(argv[1], "-d") == 0) {
        if (argc < 3) { usage(); return EXIT_USAGE; }
        led_hint = argv[2];
        argi = 3;
    }

    if (argi >= argc || strcmp(argv[argi], "-h") == 0 || strcmp(argv[argi], "--help") == 0) {
        usage();
        return 0;
    }

    const char *verb = argv[argi++];

    for (int i = argi; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0) {
            usage_verb(verb);
            return 0;
        }
    }

    if (strcmp(verb, "list") == 0) {
        return cmd_list();
    }

    char led_path[PATH_MAX];
    if (!resolve_led(led_hint, led_path, sizeof led_path)) {
        return EXIT_USAGE;
    }

    if (strcmp(verb, "status") == 0) {
        return cmd_status(led_path);
    } else if (strcmp(verb, "on") == 0) {
        return cmd_on(led_path);
    } else if (strcmp(verb, "off") == 0) {
        return cmd_off(led_path);
    } else if (strcmp(verb, "blink") == 0) {
        if (argi + 2 > argc) { usage_verb("blink"); return EXIT_USAGE; }
        return cmd_blink(led_path, argv[argi], argv[argi + 1]);
    } else if (strcmp(verb, "heartbeat") == 0) {
        return cmd_heartbeat(led_path);
    } else if (strcmp(verb, "trigger") == 0) {
        if (argi + 1 > argc) { usage_verb("trigger"); return EXIT_USAGE; }
        return cmd_trigger(led_path, argv[argi]);
    }

    usage();
    return EXIT_USAGE;
}
