/*
 * Copyright (C) 2016 Unwired Devices [info@unwds.com]
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @defgroup
 * @ingroup
 * @brief
 * @{
 * @file
 * @brief
 * @author      Eugene Ponomarev
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>

#include "nvram.h"
#include "checksum/crc16_ccitt.h"

#include "config.h"

static nvram_config_t config;
static bool config_valid = false;

static bool key_valid = false;

static config_eui64_t eui64;
static bool eui64_valid = false;

static nvram_t *nv;

uint16_t get_crc(uint8_t *buf, size_t size)
{
    return crc16_ccitt_calc(buf, size);
}

bool check_crc_config(nvram_config_t *cfg, uint32_t crc)
{
    uint16_t actual_crc = get_crc((uint8_t *) cfg, CONFIG_SIZE - 4);

    return actual_crc == crc;
}

bool check_crc_eui64(config_eui64_t *cfg, uint32_t crc)
{
    uint16_t actual_crc = get_crc((uint8_t *) cfg, CONFIG_EUI64_SIZE);

    return actual_crc == crc;
}

void config_reset_nvram(nvram_t *nvram)
{
    nv = nvram;

    memset(&config, 0, sizeof(nvram_config_t));
    config.magic = CONFIG_MAGIC;
    config.version = CONFIG_FORMAT_VER;

    config_valid = false;

    save_config_nvram(nvram);
}

bool load_config_nvram(nvram_t *nvram)
{
    nv = nvram;

    nvram_config_t temp_config;
    memset(&temp_config, 0, sizeof(nvram_config_t));

    if (nvram->read(nvram, (uint8_t *) &temp_config, CONFIG_ADDR, CONFIG_SIZE)) {
        /* Check magic */
        if (temp_config.magic != CONFIG_MAGIC) {
            puts("Magic is wrong");
            return false;
        }

        /* Check CRC */
        if (!check_crc_config(&temp_config, temp_config.cfg_crc)) {
            /* let's check if it's an old config version */
            
            nvram_old_config_t old_config;
            if (nvram->read(nvram, (uint8_t *) &old_config, CONFIG_ADDR, CONFIG_SIZE - 4)) {
                uint16_t actual_crc = get_crc((uint8_t *) &old_config, CONFIG_SIZE - 4 - 4);
                if (actual_crc != old_config.cfg_crc) {
                    puts("CRC is wrong");
                    return false;
                } else {
                    puts ("Converting old config to a new one");
                    memcpy(&config, &temp_config, sizeof(nvram_config_t));
                    save_config_nvram(nvram);
                }
            }
        }

        memcpy(&config, &temp_config, sizeof(nvram_config_t));
		
		for (int i=0; i<16; i++) {
			if (config.nwk_key[i] > 0) {
				key_valid = true;
			}
		}
		
        config_valid = true;

        return true;
    }

    return false;
}

bool save_eui64_nvram(nvram_t *nvram)
{
    /* Calculate checksum */
    eui64.crc = get_crc((uint8_t *) &eui64, CONFIG_EUI64_SIZE);

    /* Write to NVRAM */
    return (nvram->write(nvram, (uint8_t *) &eui64, CONFIG_EUI64_ADDR, sizeof(config_eui64_t)) > 0);
}

bool save_config_nvram(nvram_t *nvram)
{
    config.magic = CONFIG_MAGIC;
    config.version = CONFIG_FORMAT_VER;

    /* Calculate checksum excluding old CRC field at the end*/
    config.cfg_crc = get_crc((uint8_t *) &config, CONFIG_SIZE - 4);

    /* Write to NVRAM */
    return (nvram->write(nvram, (uint8_t *) &config, CONFIG_ADDR, sizeof(nvram_config_t)) > 0);
}

bool clear_nvram(void)
{
    return nv->clear(nv) > 0;
}

bool clear_nvram_modules(int modid)
{
    if (modid == 0) {
        /* to fix later: clear till last module or end of EEPROM */
        return nv->clearpart(nv, UNWDS_CONFIG_BASE_ADDR, 50*UNWDS_CONFIG_BLOCK_SIZE_BYTES) > 0;
    }
    
    return true;
}

config_role_t config_get_role(void)
{
	if (!eui64_valid) {
        return ROLE_NO_EUI64;
    }

    if (!config_valid) {
    	return ROLE_NO_CFG;
    }

	if (!key_valid) {
		return ROLE_EMPTY_KEY;
	}

    if (config_valid) {
        return ROLE_NODE;
    }

    return ROLE_NO_CFG;
}

bool config_write_main_block(uint64_t appid64, uint8_t joinkey[16], uint32_t devnonce)
{
	config.dev_nonce = devnonce;
    config.appid64 = appid64;
    memcpy(config.nwk_key, joinkey, 16);

    return save_config_nvram(nv);
}

bool load_eui64_nvram(nvram_t *nvram)
{
    nv = nvram;
    
    config_eui64_t temp_eui64;

    if (nvram->read(nvram, (uint8_t *) &temp_eui64, CONFIG_EUI64_ADDR, sizeof(config_eui64_t))) {
        /* Check CRC */
        if (!check_crc_eui64(&temp_eui64, temp_eui64.crc)) {
            puts("EUI64 CRC check failed");
            return false;
        }

        memcpy(&eui64, &temp_eui64, sizeof(config_eui64_t));
        eui64_valid = true;

        return true;
    }

    return false;
}

bool write_eui64_nvram(uint64_t eui)
{
    eui64.eui64 = eui;

    return save_eui64_nvram(nv);
}

uint64_t config_get_nodeid(void)
{
    return eui64.eui64;
}

uint64_t config_get_appid(void)
{
    return config.appid64;
}

uint8_t *config_get_joinkey(void)
{
    return config.nwk_key;
}

uint32_t config_get_devnonce(void)
{
	return config.dev_nonce;
}

bool config_write_role_block(uint8_t *buf, size_t size)
{
    if (size > ROLE_CONFIG_SIZE) {
        return false;
    }

    memcpy(config.role_config, buf, size);

    return save_config_nvram(nv);
}

bool config_read_role_block(uint8_t *buf, size_t size)
{
    if (size > ROLE_CONFIG_SIZE) {
        return false;
    }

    memcpy(buf, config.role_config, size);
    return true;
}

nvram_t *config_get_nvram(void) {
	return nv;
}

#ifdef __cplusplus
}
#endif
