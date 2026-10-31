static const uint8_t jpeg[22] = {0xff, 0xd8};
static const uint8_t png[22] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
static const uint8_t pjpg[22] = "_PJPG__";
static const uint8_t sp[22] = "_SJPG__";
static const uint8_t qoi[22] = "qoif";
static int failures, total;
static bool header_cache_hit;

static void check(const char *label, const char *path, const uint8_t *data,
                  int kind, bool variable, int fault)
{
    file_data = data; file_length = fault == 5 ? 2 : fault == 6 ? 0 : 22;
    fail_open = fail_read = fail_seek = fail_decode = false;
    opens = closes = active_files = bad_accesses = decode_calls = decoder_kind = probe_left_open = 0;
    lv_image_dsc_t image = {.data_size = 22, .data = data};

    lv_image_decoder_dsc_t dsc = {.src_type = variable ? LV_IMAGE_SRC_VARIABLE : LV_IMAGE_SRC_FILE,
                                .src = variable ? (const void*)&image : (const void*)path,
                                .args.no_cache = true};
    if (!variable && !header_cache_hit) {
        assert(lv_fs_open(&dsc.file, path, LV_FS_MODE_RD) == LV_FS_RES_OK);
        assert(lv_fs_close(&dsc.file) == LV_FS_RES_OK);
    }
    fail_open = fault == 1; fail_read = fault == 2; fail_decode = fault == 4;
    int actual = decoder_open(NULL, &dsc);
    bool direct_pjpg = !variable && strcmp(lv_fs_get_ext(path), "pjpg") == 0;
    bool bug_path = EXPECT_BUG && !variable && !direct_pjpg;
    int expected_kind = !ESP_LV_ENABLE_PJPG && kind == DECODE_PJPG ? DECODE_NONE : kind;
    bool error = (fault > 0 && fault < 5) || bug_path || !expected_kind;
    int expected = error ? LV_RESULT_INVALID : LV_RESULT_OK;
    bool pass = actual == expected && active_files == 0 && probe_left_open == 0 && opens == closes;
    if (fault == 0 || fault >= 5) pass = pass && decode_calls == (bug_path || !expected_kind ? 0 : 1) &&
                                  (bug_path || decoder_kind == expected_kind);
    if (fault == 1 || fault == 2) pass = pass && decode_calls == 0;
    if (fault == 4 && !bug_path) pass = pass && decode_calls == 1;
    if (!EXPECT_BUG) pass = pass && bad_accesses == 0;
    printf("%s %-24s cache=%s result=%s decode=%d kind=%d open/close=%d/%d closed_access=%d\n",
           pass ? "PASS" : "FAIL", label, header_cache_hit ? "hit" : "miss", actual == LV_RESULT_OK ? "OK" : "INVALID",
           decode_calls, decoder_kind, opens, closes, bad_accesses);
    total++; failures += !pass;
}
int main(void)
{
    for (int hit = 0; hit <= 1; ++hit) {
        header_cache_hit = hit != 0;
        check("file JPG", "A:/a.jpg", jpeg, DECODE_JPEG, false, 0);
        check("file PNG", "A:/a.png", png, DECODE_PNG, false, 0);
        check("file QOI", "A:/a.qoi", qoi, DECODE_QOI, false, 0);
        check("file PJPG extension", "A:/a.pjpg", pjpg, DECODE_PJPG, false, 0);
        check("file PJPG magic", "A:/a.bin", pjpg, DECODE_PJPG, false, 0);
        check("file SP extension", "A:/a.sjpg", sp, DECODE_SP, false, 0);
        check("file SP magic", "A:/a.bin", sp, DECODE_SP, false, 0);
        check("variable JPG", NULL, jpeg, DECODE_JPEG, true, 0);
        check("variable PNG", NULL, png, DECODE_PNG, true, 0);
        check("variable QOI", NULL, qoi, DECODE_QOI, true, 0);
        check("variable PJPG", NULL, pjpg, DECODE_PJPG, true, 0);
        check("variable SP", NULL, sp, DECODE_SP, true, 0);
        check("file open failure", "A:/a.bin", pjpg, DECODE_PJPG, false, 1);
        check("file read failure", "A:/a.bin", pjpg, DECODE_PJPG, false, 2);
        check("file decode failure", "A:/a.jpg", jpeg, DECODE_JPEG, false, 4);

        check("short JPG header", "A:/a.jpg", jpeg, DECODE_JPEG, false, 5);
        check("short PJPG signature", "A:/a.bin", pjpg, DECODE_NONE, false, 5);
        check("empty unknown header", "A:/a.bin", sp, DECODE_NONE, false, 6);
    }
    printf("%d cases, %d failures, PJPG=%d mode=%s\n", total, failures, ESP_LV_ENABLE_PJPG, EXPECT_BUG ? "original bug" : "fixed");
    return failures ? 1 : 0;
}
