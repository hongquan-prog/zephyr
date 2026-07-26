/*
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ES8311 low power mono audio codec driver.
 */

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/audio/codec.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(everest_es8311, CONFIG_AUDIO_CODEC_LOG_LEVEL);

#define DT_DRV_COMPAT everest_es8311

/* Register map (see ES8311 datasheet) */
#define ES8311_RESET_REG00       0x00 /* reset digital, csm, clock manager */
#define ES8311_CLK_MANAGER_REG01 0x01 /* clk src for mclk, enable codec clock */
#define ES8311_CLK_MANAGER_REG02 0x02 /* clk divider and clk multiplier */
#define ES8311_CLK_MANAGER_REG03 0x03 /* adc fsmode and osr */
#define ES8311_CLK_MANAGER_REG04 0x04 /* dac osr */
#define ES8311_CLK_MANAGER_REG05 0x05 /* clk divider for adc and dac */
#define ES8311_CLK_MANAGER_REG06 0x06 /* bclk inverter and divider */
#define ES8311_CLK_MANAGER_REG07 0x07 /* tri-state, lrck divider */
#define ES8311_CLK_MANAGER_REG08 0x08 /* lrck divider */
#define ES8311_SDPIN_REG09       0x09 /* dac serial digital port */
#define ES8311_SDPOUT_REG0A      0x0a /* adc serial digital port */
#define ES8311_SYSTEM_REG0B      0x0b
#define ES8311_SYSTEM_REG0C      0x0c
#define ES8311_SYSTEM_REG0D      0x0d /* power up/down */
#define ES8311_SYSTEM_REG0E      0x0e /* power up/down */
#define ES8311_SYSTEM_REG10      0x10
#define ES8311_SYSTEM_REG11      0x11
#define ES8311_SYSTEM_REG12      0x12 /* enable DAC */
#define ES8311_SYSTEM_REG13      0x13
#define ES8311_SYSTEM_REG14      0x14 /* select DMIC, analog pga gain */
#define ES8311_ADC_REG15         0x15 /* adc ramp rate, dmic sense */
#define ES8311_ADC_REG16         0x16 /* adc pga gain */
#define ES8311_ADC_REG17         0x17 /* adc volume */
#define ES8311_ADC_REG1B         0x1b /* alc automute, adc hpf s1 */
#define ES8311_ADC_REG1C         0x1c /* equalizer, hpf s2 */
#define ES8311_DAC_REG31         0x31 /* dac mute */
#define ES8311_DAC_REG32         0x32 /* dac volume */
#define ES8311_DAC_REG37         0x37 /* dac ramprate */
#define ES8311_GPIO_REG44        0x44 /* dac2adc test, internal reference */
#define ES8311_GP_REG45          0x45 /* GP control */
#define ES8311_CHD1_REGFD        0xfd /* chip ID1 */
#define ES8311_CHD2_REGFE        0xfe /* chip ID2 */

#define ES8311_CHIP_ID1 0x83
#define ES8311_CHIP_ID2 0x11

/* DAC volume register: 0x00 = -95.5 dB, 0xbf = 0 dB, 0xff = +32 dB */
#define ES8311_DAC_VOL_MAX 0xff

/* Clock coefficient structure for the codec HiFi MCLK clock divider */
struct es8311_coeff_div {
	uint32_t mclk;     /* mclk frequency */
	uint32_t rate;     /* sample rate */
	uint8_t pre_div;   /* pre divider, range 1 to 8 */
	uint8_t pre_multi; /* pre multiplier: 1, 2, 4 or 8 */
	uint8_t adc_div;   /* adcclk divider */
	uint8_t dac_div;   /* dacclk divider */
	uint8_t fs_mode;   /* 0: single speed, 1: double speed */
	uint8_t lrck_h;    /* adclrck and daclrck divider, high bits */
	uint8_t lrck_l;    /* adclrck and daclrck divider, low bits */
	uint8_t bclk_div;  /* sclk divider */
	uint8_t adc_osr;   /* adc osr */
	uint8_t dac_osr;   /* dac osr */
};

static const struct es8311_coeff_div es8311_coeff_div[] = {
	/* mclk      rate   pre_div mult adc_div dac_div fs lrch  lrcl  bckdiv osr   osr */
	/* 8k */
	{12288000, 8000, 0x06, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x20},
	{18432000, 8000, 0x03, 0x02, 0x03, 0x03, 0x00, 0x05, 0xff, 0x18, 0x10, 0x20},
	{16384000, 8000, 0x08, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x20},
	{8192000, 8000, 0x04, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x20},
	{6144000, 8000, 0x03, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x20},
	{4096000, 8000, 0x02, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x20},
	{3072000, 8000, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x20},
	{2048000, 8000, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x20},
	{1536000, 8000, 0x03, 0x04, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x20},
	{1024000, 8000, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x20},
	/* 11.025k */
	{11289600, 11025, 0x04, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x20},
	{5644800, 11025, 0x02, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x20},
	{2822400, 11025, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x20},
	{1411200, 11025, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x20},
	/* 12k */
	{12288000, 12000, 0x04, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x20},
	{6144000, 12000, 0x02, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x20},
	{3072000, 12000, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x20},
	{1536000, 12000, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x20},
	/* 16k */
	{12288000, 16000, 0x03, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x20},
	{18432000, 16000, 0x03, 0x02, 0x03, 0x03, 0x00, 0x02, 0xff, 0x0c, 0x10, 0x20},
	{16384000, 16000, 0x04, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x20},
	{8192000, 16000, 0x02, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x20},
	{6144000, 16000, 0x03, 0x02, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x20},
	{4096000, 16000, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x20},
	{3072000, 16000, 0x03, 0x04, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x20},
	{2048000, 16000, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x20},
	{1536000, 16000, 0x03, 0x08, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x20},
	{1024000, 16000, 0x01, 0x04, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x20},
	/* 22.05k */
	{11289600, 22050, 0x02, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	{5644800, 22050, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	{2822400, 22050, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	{1411200, 22050, 0x01, 0x04, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	/* 24k */
	{12288000, 24000, 0x02, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	{18432000, 24000, 0x03, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	{6144000, 24000, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	{3072000, 24000, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	{1536000, 24000, 0x01, 0x04, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	/* 32k */
	{12288000, 32000, 0x03, 0x02, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	{18432000, 32000, 0x03, 0x04, 0x03, 0x03, 0x00, 0x02, 0xff, 0x0c, 0x10, 0x10},
	{16384000, 32000, 0x02, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	{8192000, 32000, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	{6144000, 32000, 0x03, 0x04, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	{4096000, 32000, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	{3072000, 32000, 0x03, 0x08, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	{2048000, 32000, 0x01, 0x04, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	{1536000, 32000, 0x01, 0x08, 0x01, 0x01, 0x01, 0x00, 0x7f, 0x02, 0x10, 0x10},
	{1024000, 32000, 0x01, 0x08, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	/* 44.1k */
	{11289600, 44100, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	{5644800, 44100, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	{2822400, 44100, 0x01, 0x04, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	{1411200, 44100, 0x01, 0x08, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	/* 48k */
	{12288000, 48000, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	{18432000, 48000, 0x03, 0x02, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	{6144000, 48000, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	{3072000, 48000, 0x01, 0x04, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	{1536000, 48000, 0x01, 0x08, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	/* 64k */
	{12288000, 64000, 0x03, 0x04, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	{18432000, 64000, 0x03, 0x04, 0x03, 0x03, 0x01, 0x01, 0x7f, 0x06, 0x10, 0x10},
	{16384000, 64000, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	{8192000, 64000, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	{6144000, 64000, 0x01, 0x04, 0x03, 0x03, 0x01, 0x01, 0x7f, 0x06, 0x10, 0x10},
	{4096000, 64000, 0x01, 0x04, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	{3072000, 64000, 0x01, 0x08, 0x03, 0x03, 0x01, 0x01, 0x7f, 0x06, 0x10, 0x10},
	{2048000, 64000, 0x01, 0x08, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	{1536000, 64000, 0x01, 0x08, 0x01, 0x01, 0x01, 0x00, 0xbf, 0x03, 0x18, 0x18},
	{1024000, 64000, 0x01, 0x08, 0x01, 0x01, 0x01, 0x00, 0x7f, 0x02, 0x10, 0x10},
	/* 88.2k */
	{11289600, 88200, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	{5644800, 88200, 0x01, 0x04, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	{2822400, 88200, 0x01, 0x08, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	{1411200, 88200, 0x01, 0x08, 0x01, 0x01, 0x01, 0x00, 0x7f, 0x02, 0x10, 0x10},
	/* 96k */
	{24576000, 96000, 0x02, 0x02, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	{12288000, 96000, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	{18432000, 96000, 0x03, 0x04, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	{6144000, 96000, 0x01, 0x04, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	{3072000, 96000, 0x01, 0x08, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
	{1536000, 96000, 0x01, 0x08, 0x01, 0x01, 0x01, 0x00, 0x7f, 0x02, 0x10, 0x10},
};

struct es8311_config {
	struct i2c_dt_spec i2c;
	struct gpio_dt_spec pa_gpio;
	bool has_pa_gpio;
};

struct es8311_data {
	bool started;
};

static inline int es8311_write_reg(const struct es8311_config *cfg, uint8_t reg, uint8_t val)
{
	return i2c_reg_write_byte_dt(&cfg->i2c, reg, val);
}

static inline int es8311_read_reg(const struct es8311_config *cfg, uint8_t reg, uint8_t *val)
{
	return i2c_reg_read_byte_dt(&cfg->i2c, reg, val);
}

static const struct es8311_coeff_div *es8311_get_coeff(uint32_t mclk, uint32_t rate)
{
	for (int i = 0; i < ARRAY_SIZE(es8311_coeff_div); i++) {
		if (es8311_coeff_div[i].rate == rate && es8311_coeff_div[i].mclk == mclk) {
			return &es8311_coeff_div[i];
		}
	}
	return NULL;
}

static int es8311_config_sample(const struct device *dev, uint32_t mclk_freq, uint32_t sample_rate)
{
	const struct es8311_config *cfg = dev->config;
	const struct es8311_coeff_div *coeff;
	uint8_t regv, datmp;
	int ret;

	coeff = es8311_get_coeff(mclk_freq, sample_rate);
	if (coeff == NULL) {
		LOG_ERR("Unable to configure %u Hz sample rate with %u Hz MCLK", sample_rate,
			mclk_freq);
		return -ENOTSUP;
	}

	ret = es8311_read_reg(cfg, ES8311_CLK_MANAGER_REG02, &regv);
	if (ret) {
		return ret;
	}
	regv &= 0x07;
	regv |= (coeff->pre_div - 1) << 5;
	switch (coeff->pre_multi) {
	case 1:
		datmp = 0;
		break;
	case 2:
		datmp = 1;
		break;
	case 4:
		datmp = 2;
		break;
	case 8:
		datmp = 3;
		break;
	default:
		datmp = 0;
		break;
	}
	regv |= datmp << 3;
	ret = es8311_write_reg(cfg, ES8311_CLK_MANAGER_REG02, regv);

	regv = ((coeff->adc_div - 1) << 4) | (coeff->dac_div - 1);
	ret |= es8311_write_reg(cfg, ES8311_CLK_MANAGER_REG05, regv);

	ret |= es8311_read_reg(cfg, ES8311_CLK_MANAGER_REG03, &regv);
	if (ret) {
		return -EIO;
	}
	regv &= 0x80;
	regv |= (coeff->fs_mode << 6) | coeff->adc_osr;
	ret = es8311_write_reg(cfg, ES8311_CLK_MANAGER_REG03, regv);

	ret |= es8311_read_reg(cfg, ES8311_CLK_MANAGER_REG04, &regv);
	if (ret) {
		return -EIO;
	}
	regv &= 0x80;
	regv |= coeff->dac_osr;
	ret = es8311_write_reg(cfg, ES8311_CLK_MANAGER_REG04, regv);

	ret |= es8311_read_reg(cfg, ES8311_CLK_MANAGER_REG07, &regv);
	if (ret) {
		return -EIO;
	}
	regv &= 0xc0;
	regv |= coeff->lrck_h;
	ret = es8311_write_reg(cfg, ES8311_CLK_MANAGER_REG07, regv);

	ret |= es8311_write_reg(cfg, ES8311_CLK_MANAGER_REG08, coeff->lrck_l);

	ret |= es8311_read_reg(cfg, ES8311_CLK_MANAGER_REG06, &regv);
	if (ret) {
		return -EIO;
	}
	regv &= 0xe0;
	if (coeff->bclk_div < 19) {
		regv |= (coeff->bclk_div - 1);
	} else {
		regv |= coeff->bclk_div;
	}
	ret = es8311_write_reg(cfg, ES8311_CLK_MANAGER_REG06, regv);

	return ret ? -EIO : 0;
}

static int es8311_start_codec(const struct device *dev)
{
	const struct es8311_config *cfg = dev->config;
	struct es8311_data *data = dev->data;
	uint8_t dac_iface, adc_iface, regv;
	int ret;

	if (data->started) {
		return 0;
	}

	/* Slave mode, clock manager reset */
	ret = es8311_write_reg(cfg, ES8311_RESET_REG00, 0x80);
	/* MCLK from pin, not inverted, all codec clocks on */
	ret |= es8311_write_reg(cfg, ES8311_CLK_MANAGER_REG01, 0x3f);

	ret |= es8311_read_reg(cfg, ES8311_SDPIN_REG09, &dac_iface);
	ret |= es8311_read_reg(cfg, ES8311_SDPOUT_REG0A, &adc_iface);
	if (ret) {
		return -EIO;
	}
	/* Enable both DAC and ADC serial data ports (clear tri-state bit) */
	dac_iface &= 0xbf;
	adc_iface &= 0xbf;
	ret |= es8311_write_reg(cfg, ES8311_SDPIN_REG09, dac_iface);
	ret |= es8311_write_reg(cfg, ES8311_SDPOUT_REG0A, adc_iface);

	ret |= es8311_write_reg(cfg, ES8311_ADC_REG17, 0xbf); /* ADC volume 0 dB */
	ret |= es8311_write_reg(cfg, ES8311_SYSTEM_REG0E, 0x02);
	ret |= es8311_write_reg(cfg, ES8311_SYSTEM_REG12, 0x00);
	ret |= es8311_write_reg(cfg, ES8311_SYSTEM_REG14, 0x1a);

	/* Analog microphone (PDM DMIC disabled) */
	ret |= es8311_read_reg(cfg, ES8311_SYSTEM_REG14, &regv);
	if (ret) {
		return -EIO;
	}
	regv &= ~0x40;
	ret = es8311_write_reg(cfg, ES8311_SYSTEM_REG14, regv);

	ret |= es8311_write_reg(cfg, ES8311_SYSTEM_REG0D, 0x01);
	ret |= es8311_write_reg(cfg, ES8311_ADC_REG15, 0x40);
	ret |= es8311_write_reg(cfg, ES8311_DAC_REG37, 0x08);
	ret |= es8311_write_reg(cfg, ES8311_GP_REG45, 0x00);
	if (ret) {
		return -EIO;
	}

	if (cfg->has_pa_gpio) {
		gpio_pin_set_dt(&cfg->pa_gpio, 1);
	}
	data->started = true;
	return 0;
}

static int es8311_suspend_codec(const struct device *dev)
{
	const struct es8311_config *cfg = dev->config;
	struct es8311_data *data = dev->data;
	int ret;

	if (!data->started) {
		return 0;
	}

	if (cfg->has_pa_gpio) {
		gpio_pin_set_dt(&cfg->pa_gpio, 0);
	}

	/* Note: this resets the DAC volume register to 0 (-95.5 dB), so the
	 * output volume must be set again after the next start.
	 */
	ret = es8311_write_reg(cfg, ES8311_DAC_REG32, 0x00);
	ret |= es8311_write_reg(cfg, ES8311_ADC_REG17, 0x00);
	ret |= es8311_write_reg(cfg, ES8311_SYSTEM_REG0E, 0xff);
	ret |= es8311_write_reg(cfg, ES8311_SYSTEM_REG12, 0x02);
	ret |= es8311_write_reg(cfg, ES8311_SYSTEM_REG14, 0x00);
	ret |= es8311_write_reg(cfg, ES8311_SYSTEM_REG0D, 0xfa);
	ret |= es8311_write_reg(cfg, ES8311_ADC_REG15, 0x00);
	ret |= es8311_write_reg(cfg, ES8311_CLK_MANAGER_REG02, 0x10);
	ret |= es8311_write_reg(cfg, ES8311_RESET_REG00, 0x00);
	ret |= es8311_write_reg(cfg, ES8311_RESET_REG00, 0x1f);
	ret |= es8311_write_reg(cfg, ES8311_CLK_MANAGER_REG01, 0x30);
	ret |= es8311_write_reg(cfg, ES8311_CLK_MANAGER_REG01, 0x00);
	ret |= es8311_write_reg(cfg, ES8311_GP_REG45, 0x00);
	ret |= es8311_write_reg(cfg, ES8311_SYSTEM_REG0D, 0xfc);
	ret |= es8311_write_reg(cfg, ES8311_CLK_MANAGER_REG02, 0x00);
	if (ret) {
		return -EIO;
	}
	data->started = false;
	return 0;
}

static int es8311_configure(const struct device *dev, struct audio_codec_cfg *audiocfg)
{
	const struct es8311_config *cfg = dev->config;
	uint8_t dac_iface, adc_iface;
	int ret;

	if (audiocfg->dai_type != AUDIO_DAI_TYPE_I2S) {
		LOG_ERR("Only I2S DAI type is supported");
		return -ENOTSUP;
	}
	if (audiocfg->dai_route != AUDIO_ROUTE_PLAYBACK &&
	    audiocfg->dai_route != AUDIO_ROUTE_PLAYBACK_CAPTURE &&
	    audiocfg->dai_route != AUDIO_ROUTE_CAPTURE) {
		LOG_ERR("Unsupported DAI route %d", audiocfg->dai_route);
		return -ENOTSUP;
	}
	if ((audiocfg->dai_cfg.i2s.format & I2S_FMT_DATA_FORMAT_MASK) != I2S_FMT_DATA_FORMAT_I2S) {
		LOG_ERR("Only I2S (Philips) data format is supported");
		return -ENOTSUP;
	}

	ret = es8311_read_reg(cfg, ES8311_SDPIN_REG09, &dac_iface);
	ret |= es8311_read_reg(cfg, ES8311_SDPOUT_REG0A, &adc_iface);
	if (ret) {
		return -EIO;
	}

	/* I2S (Philips) format for both ports */
	dac_iface &= 0xfc;
	adc_iface &= 0xfc;

	/* Word length */
	switch (audiocfg->dai_cfg.i2s.word_size) {
	case AUDIO_PCM_WIDTH_16_BITS:
		dac_iface |= 0x0c;
		adc_iface |= 0x0c;
		break;
	case AUDIO_PCM_WIDTH_24_BITS:
		dac_iface &= ~0x1c;
		adc_iface &= ~0x1c;
		break;
	case AUDIO_PCM_WIDTH_32_BITS:
		dac_iface |= 0x10;
		adc_iface |= 0x10;
		break;
	default:
		LOG_ERR("Unsupported word size %u", audiocfg->dai_cfg.i2s.word_size);
		return -ENOTSUP;
	}

	ret |= es8311_write_reg(cfg, ES8311_SDPIN_REG09, dac_iface);
	ret |= es8311_write_reg(cfg, ES8311_SDPOUT_REG0A, adc_iface);
	if (ret) {
		return -EIO;
	}

	return es8311_config_sample(dev, audiocfg->mclk_freq, audiocfg->dai_cfg.i2s.frame_clk_freq);
}

static void es8311_start_output(const struct device *dev)
{
	int ret = es8311_start_codec(dev);

	if (ret) {
		LOG_ERR("Failed to start codec: %d", ret);
	}
}

static void es8311_stop_output(const struct device *dev)
{
	int ret = es8311_suspend_codec(dev);

	if (ret) {
		LOG_ERR("Failed to stop codec: %d", ret);
	}
}

static int es8311_apply_properties(const struct device *dev)
{
	return 0;
}

static int es8311_set_property(const struct device *dev, audio_property_t property,
			       audio_channel_t channel, audio_property_value_t val)
{
	const struct es8311_config *cfg = dev->config;
	uint8_t regv;
	int ret;

	switch (property) {
	case AUDIO_PROPERTY_OUTPUT_VOLUME:
		if (val.vol < 0 || val.vol > 100) {
			return -EINVAL;
		}
		regv = (uint8_t)(val.vol * ES8311_DAC_VOL_MAX / 100);
		ret = es8311_write_reg(cfg, ES8311_DAC_REG32, regv);
		break;
	case AUDIO_PROPERTY_OUTPUT_MUTE:
		ret = es8311_read_reg(cfg, ES8311_DAC_REG31, &regv);
		if (ret) {
			break;
		}
		regv &= 0x9f;
		if (val.mute) {
			regv |= 0x60;
		}
		ret = es8311_write_reg(cfg, ES8311_DAC_REG31, regv);
		break;
	case AUDIO_PROPERTY_INPUT_VOLUME:
		/* val.vol is the microphone PGA gain in dB, 0 to 42 */
		if (val.vol < 0 || val.vol > 42) {
			return -EINVAL;
		}
		regv = (uint8_t)(val.vol / 6); /* 6 dB steps, 0 dB floor */
		ret = es8311_write_reg(cfg, ES8311_ADC_REG16, regv);
		break;
	default:
		return -ENOTSUP;
	}

	return ret ? -EIO : 0;
}

static DEVICE_API(audio_codec, es8311_driver_api) = {
	.configure = es8311_configure,
	.start_output = es8311_start_output,
	.stop_output = es8311_stop_output,
	.set_property = es8311_set_property,
	.apply_properties = es8311_apply_properties,
};

static int es8311_init(const struct device *dev)
{
	const struct es8311_config *cfg = dev->config;
	uint8_t id1, id2, regv;
	int ret;

	if (!i2c_is_ready_dt(&cfg->i2c)) {
		LOG_ERR("I2C bus %s not ready", cfg->i2c.bus->name);
		return -ENODEV;
	}

	if (cfg->has_pa_gpio) {
		if (!gpio_is_ready_dt(&cfg->pa_gpio)) {
			LOG_ERR("PA GPIO not ready");
			return -ENODEV;
		}
		ret = gpio_pin_configure_dt(&cfg->pa_gpio, GPIO_OUTPUT_INACTIVE);
		if (ret) {
			LOG_ERR("Failed to configure PA GPIO: %d", ret);
			return ret;
		}
	}

	/* Enhance ES8311 I2C noise immunity; written twice because the first
	 * I2C write after power-up occasionally fails on this chip. Done before
	 * anything else so a failed first transaction cannot abort the probe.
	 */
	ret = es8311_write_reg(cfg, ES8311_GPIO_REG44, 0x08);
	ret |= es8311_write_reg(cfg, ES8311_GPIO_REG44, 0x08);

	ret |= es8311_read_reg(cfg, ES8311_CHD1_REGFD, &id1);
	ret |= es8311_read_reg(cfg, ES8311_CHD2_REGFE, &id2);
	if (ret) {
		LOG_ERR("Unable to read chip ID");
		return -ENODEV;
	}
	if (id1 != ES8311_CHIP_ID1 || id2 != ES8311_CHIP_ID2) {
		LOG_ERR("Wrong chip ID: %02x %02x", id1, id2);
		return -ENODEV;
	}

	ret |= es8311_write_reg(cfg, ES8311_CLK_MANAGER_REG01, 0x30);
	ret |= es8311_write_reg(cfg, ES8311_CLK_MANAGER_REG02, 0x00);
	ret |= es8311_write_reg(cfg, ES8311_CLK_MANAGER_REG03, 0x10);
	ret |= es8311_write_reg(cfg, ES8311_ADC_REG16, 0x24);
	ret |= es8311_write_reg(cfg, ES8311_CLK_MANAGER_REG04, 0x10);
	ret |= es8311_write_reg(cfg, ES8311_CLK_MANAGER_REG05, 0x00);
	ret |= es8311_write_reg(cfg, ES8311_SYSTEM_REG0B, 0x00);
	ret |= es8311_write_reg(cfg, ES8311_SYSTEM_REG0C, 0x00);
	ret |= es8311_write_reg(cfg, ES8311_SYSTEM_REG10, 0x1f);
	ret |= es8311_write_reg(cfg, ES8311_SYSTEM_REG11, 0x7f);
	ret |= es8311_write_reg(cfg, ES8311_RESET_REG00, 0x80);

	/* Slave mode: the SoC I2S peripheral is the bus master */
	ret |= es8311_read_reg(cfg, ES8311_RESET_REG00, &regv);
	regv &= 0xbf;
	ret |= es8311_write_reg(cfg, ES8311_RESET_REG00, regv);

	/* Clock from external MCLK pin, MCLK not inverted */
	ret |= es8311_write_reg(cfg, ES8311_CLK_MANAGER_REG01, 0x3f);

	/* SCLK not inverted */
	ret |= es8311_read_reg(cfg, ES8311_CLK_MANAGER_REG06, &regv);
	regv &= ~0x20;
	ret |= es8311_write_reg(cfg, ES8311_CLK_MANAGER_REG06, regv);

	ret |= es8311_write_reg(cfg, ES8311_SYSTEM_REG13, 0x10);
	ret |= es8311_write_reg(cfg, ES8311_ADC_REG1B, 0x0a);
	ret |= es8311_write_reg(cfg, ES8311_ADC_REG1C, 0x6a);
	/* ADCDAT_SEL (REG44 bits 6:4) stays at 0 so the ADC serial port
	 * outputs microphone data on both slots, same as the esp_codec_dev
	 * ES8311 driver writes it.
	 */
	ret |= es8311_write_reg(cfg, ES8311_GPIO_REG44, 0x08);
	if (ret) {
		LOG_ERR("Failed to initialize codec registers");
		return -EIO;
	}

	LOG_INF("Found ES8311 (id=%02x%02x)", id1, id2);
	return 0;
}

#define ES8311_INIT(inst)                                                                          \
	static const struct es8311_config es8311_config_##inst = {                                 \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                                 \
		.pa_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, pa_gpios, {0}),                          \
		.has_pa_gpio = DT_INST_NODE_HAS_PROP(inst, pa_gpios),                              \
	};                                                                                         \
	static struct es8311_data es8311_data_##inst;                                              \
	DEVICE_DT_INST_DEFINE(inst, es8311_init, NULL, &es8311_data_##inst, &es8311_config_##inst, \
			      POST_KERNEL, CONFIG_AUDIO_CODEC_INIT_PRIORITY, &es8311_driver_api);

DT_INST_FOREACH_STATUS_OKAY(ES8311_INIT)
