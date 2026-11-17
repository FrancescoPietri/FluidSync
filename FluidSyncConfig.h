//FluidSync - machine configuration (edited from the web page, saved in flash)
//Only data types here: the logic owns the config, the web layer parses/renders it, the HW layer stores the raw bytes.
#pragma once

#include <stdint.h>

#include "FluidSyncHW.h" //N_PUMPS

#define CFG_NAME_LEN    16  //including the terminator
#define CFG_N_PRESETS   8
#define CFG_MAGIC       0x46535943u //"FSYC"
#define CFG_VERSION     2           //bump when the layout below changes (and convert the old one in config_init)
#define CFG_MIN_ML      100         //volume of a preset, ml
#define CFG_MAX_ML      199

typedef struct {
    char name[CFG_NAME_LEN];
    uint32_t color;         //0xRRGGBB
} liquid_cfg_t;

typedef struct {
    char name[CFG_NAME_LEN];    //empty = free slot
    uint8_t pct[N_PUMPS];       //percentage of each liquid, sum = 100
    uint8_t volume_ml;          //CFG_MIN_ML..CFG_MAX_ML
} preset_cfg_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    liquid_cfg_t liquid[N_PUMPS];
    preset_cfg_t preset[CFG_N_PRESETS];
} machine_config_t;
