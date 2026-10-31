/** SPDX-License-Identifier: GPL-2.0-only */
/** Shared factory diagnostic results; RESERVED denotes an unavailable board feature. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Factory diagnostic outcome, including unavailable features. */
typedef enum {
    FACTORY_TEST_RESULT_PENDING = 0, /**< Not run yet in this session. */
    FACTORY_TEST_RESULT_RUNNING,     /**< Started, result not in yet. */
    FACTORY_TEST_RESULT_PASS,
    FACTORY_TEST_RESULT_FAIL,
    FACTORY_TEST_RESULT_RESERVED,    /**< Board cannot run it; never counted as fail. */
} factory_test_result_t;

/** @brief Return the text label for a diagnostic result. */
static inline const char *factory_test_result_name(factory_test_result_t result)
{
    switch (result) {
    case FACTORY_TEST_RESULT_RUNNING:
        return "running";
    case FACTORY_TEST_RESULT_PASS:
        return "pass";
    case FACTORY_TEST_RESULT_FAIL:
        return "fail";
    case FACTORY_TEST_RESULT_RESERVED:
        return "reserved";
    case FACTORY_TEST_RESULT_PENDING:
    default:
        return "pending";
    }
}

#ifdef __cplusplus
}
#endif
