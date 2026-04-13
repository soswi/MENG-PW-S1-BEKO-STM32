/*
 * crypto_processing.c
 *
 *  Created on: Mar 31, 2026
 *      Author: soswi
 */

#include "crypto_processing.h"

static uint8_t g_crypto_initialized = 0;

cmox_retval_t crypto_processing_init(void)
{
    cmox_init_arg_t init_target = { CMOX_INIT_TARGET_AUTO, NULL };

    if (g_crypto_initialized) {
        return CMOX_INIT_SUCCESS;
    }

    cmox_retval_t ret = cmox_initialize(&init_target);
    if (ret == CMOX_INIT_SUCCESS) {
        g_crypto_initialized = 1;
    }

    return ret;
}

void crypto_processing_deinit(void)
{
    if (g_crypto_initialized) {
        cmox_finalize(NULL);
        g_crypto_initialized = 0;
    }
}


