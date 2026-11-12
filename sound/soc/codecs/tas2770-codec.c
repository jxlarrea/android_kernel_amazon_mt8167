/*
** =============================================================================
** Copyright (c) 2016  Texas Instruments Inc.
**
** This program is free software; you can redistribute it and/or modify it under
** the terms of the GNU General Public License as published by the Free Software
** Foundation; version 2.
**
** This program is distributed in the hope that it will be useful, but WITHOUT
** ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
** FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
**
** File:
**     tas2770-codec.c
**
** Description:
**     ALSA SoC driver for Texas Instruments TAS2770 High Performance 4W Smart Amplifier
**
** =============================================================================
*/

#ifdef CONFIG_TAS2770_CODEC

#define DEBUG
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/i2c.h>
#include <linux/gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/firmware.h>
#include <linux/regmap.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/slab.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/initval.h>
#include <sound/tlv.h>

#include "tas2770.h"


#define TAS2770_MDELAY 0xFFFFFFFE
//#define KCONTROL_CODEC

#define DAPM_TO_CODEC
static unsigned int tas2770_codec_read(struct snd_soc_codec *codec,  unsigned int reg)
{
	struct tas2770_priv *pTAS2770 = snd_soc_codec_get_drvdata(codec);
	int nResult = 0;
	unsigned int value = 0;

	mutex_lock(&pTAS2770->dev_lock);

	nResult = regmap_read(pTAS2770->regmap, reg, &value);

	if (nResult < 0)
		dev_err(pTAS2770->dev, "%s, ERROR, reg=0x%x, E=%d\n",
			__func__, reg, nResult);
	else
		dev_dbg(pTAS2770->dev, "%s, reg: 0x%x, value: 0x%x\n", __func__, reg, value);

	mutex_unlock(&pTAS2770->dev_lock);
	if(nResult >= 0)
		return value;
	else
		return nResult;
}


static int tas2770_codec_write(struct snd_soc_codec *codec, unsigned int reg,
	unsigned int value)
{
	struct tas2770_priv *pTAS2770 = snd_soc_codec_get_drvdata(codec);

	int nResult = 0;

	mutex_lock(&pTAS2770->dev_lock);

	nResult = regmap_write(pTAS2770->regmap, reg, value);
	if(nResult < 0)
		dev_err(pTAS2770->dev, "%s, ERROR, reg=0x%x, E=%d\n",
			__func__, reg, nResult);
	else
		dev_dbg(pTAS2770->dev, "%s, reg: 0x%x, 0x%x\n", __func__, reg, value);

	mutex_unlock(&pTAS2770->dev_lock);

	return nResult;

}


static int tas2770_codec_suspend(struct snd_soc_codec *codec)
{
	struct tas2770_priv *pTAS2770 = snd_soc_codec_get_drvdata(codec);
	int ret = 0;

	mutex_lock(&pTAS2770->codec_lock);

	dev_dbg(pTAS2770->dev, "%s\n", __func__);
	pTAS2770->runtime_suspend(pTAS2770);

	mutex_unlock(&pTAS2770->codec_lock);
	return ret;
}

static int tas2770_codec_resume(struct snd_soc_codec *codec)
{
	struct tas2770_priv *pTAS2770 = snd_soc_codec_get_drvdata(codec);
	int ret = 0;

	mutex_lock(&pTAS2770->codec_lock);

	dev_dbg(pTAS2770->dev, "%s\n", __func__);
	pTAS2770->runtime_resume(pTAS2770);

	mutex_unlock(&pTAS2770->codec_lock);
	return ret;
}

static const struct snd_kcontrol_new tas2770_asi_controls[] = {
	SOC_DAPM_SINGLE("Left", TAS2770_TDMConfigurationReg2,
		4, 1, 0),
	SOC_DAPM_SINGLE("Right", TAS2770_TDMConfigurationReg2,
		4, 2, 0),
	SOC_DAPM_SINGLE("LeftRightDiv2", TAS2770_TDMConfigurationReg2,
		4, 3, 0),
};


static int tas2770_dac_event(struct snd_soc_dapm_widget *w,
			struct snd_kcontrol *kcontrol, int event)
{
#ifdef DAPM_TO_CODEC
	struct snd_soc_codec *codec = snd_soc_dapm_to_codec(w->dapm);
#else
	struct snd_soc_codec *codec = w->codec;
#endif

	/* struct tas2770_priv *pTAS2770 = snd_soc_codec_get_drvdata(codec); */
	pr_info("%s", __func__);
	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		snd_soc_update_bits(codec, TAS2770_PowerControl,
			TAS2770_PowerControl_OperationalMode10_Mask,
			TAS2770_PowerControl_OperationalMode10_Active);
		break;
	case SND_SOC_DAPM_PRE_PMD:
		snd_soc_update_bits(codec, TAS2770_PowerControl,
			TAS2770_PowerControl_OperationalMode10_Mask,
			TAS2770_PowerControl_OperationalMode10_Shutdown);
		break;

	}
	return 0;
}

static const struct snd_soc_dapm_widget tas2770_dapm_widgets[] = {
	SND_SOC_DAPM_AIF_IN("ASI1", "ASI1 Playback", 0, SND_SOC_NOPM, 0, 0),
#if 0
	SND_SOC_DAPM_MIXER("ASI1 Sel",
		TAS2770_TDMConfigurationReg2, 4, 0,
		&tas2770_asi_controls[0],
		ARRAY_SIZE(tas2770_asi_controls)),
#endif
	SND_SOC_DAPM_DAC_E("DAC", NULL, SND_SOC_NOPM, 0, 0,
		tas2770_dac_event, SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_OUTPUT("OUT")

};

static const struct snd_soc_dapm_route tas2770_audio_map[] = {
#if 0
	{"ASI1 Sel", "Left", "ASI1"},
	{"ASI1 Sel", "Right", "ASI1"},
	{"ASI1 Sel", "LeftRightDiv2", "ASI1"},
	{"DAC", NULL, "ASI1 Sel"},
#else
	{"DAC", NULL, "ASI1"},
#endif
	{"OUT", NULL, "DAC"},
};


static int tas2770_mute(struct snd_soc_dai *dai, int mute)
{
	struct snd_soc_codec *codec = dai->codec;
	struct tas2770_priv *pTAS2770 = snd_soc_codec_get_drvdata(codec);

	dev_err(pTAS2770->dev, "%s mute %d",__func__, mute);

	mutex_lock(&pTAS2770->codec_lock);
	if(mute) {
		snd_soc_update_bits(codec, TAS2770_PowerControl,
			TAS2770_PowerControl_OperationalMode10_Mask,
			TAS2770_PowerControl_OperationalMode10_Mute);
	} else {
		snd_soc_update_bits(codec, TAS2770_PowerControl,
			TAS2770_PowerControl_OperationalMode10_Mask,
			TAS2770_PowerControl_OperationalMode10_Active);
	}
	mutex_unlock(&pTAS2770->codec_lock);
	return 0;
}



static int tas2770_hw_params(struct snd_pcm_substream *substream,
		struct snd_pcm_hw_params *params,
		struct snd_soc_dai *dai)
{
	struct snd_soc_codec *codec = dai->codec;
	struct tas2770_priv *pTAS2770 = snd_soc_codec_get_drvdata(codec);
	int ret = 0;
	dev_err(pTAS2770->dev, "%s",__func__);
	dev_dbg(pTAS2770->dev, "%s, format: %d\n", __func__, params_format(params));

	switch(params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		snd_soc_update_bits(codec,
			TAS2770_TDMConfigurationReg2,
			TAS2770_TDMConfigurationReg2_RXWLEN32_Mask |
			TAS2770_TDMConfigurationReg2_RXSLEN10_Mask,
			TAS2770_TDMConfigurationReg2_RXWLEN32_16Bits |
			TAS2770_TDMConfigurationReg2_RXSLEN10_32Bits);
			pTAS2770->ch_size = 16;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
			snd_soc_update_bits(codec,
			TAS2770_TDMConfigurationReg2,
			TAS2770_TDMConfigurationReg2_RXWLEN32_Mask |
			TAS2770_TDMConfigurationReg2_RXSLEN10_Mask,
			TAS2770_TDMConfigurationReg2_RXWLEN32_24Bits  |
			TAS2770_TDMConfigurationReg2_RXSLEN10_24Bits);
			pTAS2770->ch_size = 24;
		break;
	case SNDRV_PCM_FORMAT_S32_LE:
			snd_soc_update_bits(codec,
			TAS2770_TDMConfigurationReg2,
			TAS2770_TDMConfigurationReg2_RXWLEN32_Mask |
			TAS2770_TDMConfigurationReg2_RXSLEN10_Mask,
			TAS2770_TDMConfigurationReg2_RXWLEN32_32Bits |
			TAS2770_TDMConfigurationReg2_RXSLEN10_32Bits);
			pTAS2770->ch_size = 32;
		break;

	}

	snd_soc_update_bits(codec,
		TAS2770_TDMConfigurationReg2,
		TAS2770_TDMConfigurationReg2_RXSCFG54_Mask,
		TAS2770_TDMConfigurationReg2_RXSCFG54_Stereo_DownMix);

	snd_soc_update_bits(codec,
		TAS2770_TDMConfigurationReg0,
		TAS2770_TDMConfigurationReg0_DETECTSAMPRATE_Mask,
		TAS2770_TDMConfigurationReg0_DETECTSAMPRATE_Disabled);

	dev_dbg(pTAS2770->dev, "%s, sample rate: %d\n", __func__, params_rate(params));
	switch(params_rate(params))
	{
		case 48000:
			snd_soc_update_bits(codec,
				TAS2770_TDMConfigurationReg0,
				TAS2770_TDMConfigurationReg0_SAMPRATERAMP_Mask,
				TAS2770_TDMConfigurationReg0_SAMPRATERAMP_48KHz);
			snd_soc_update_bits(codec,
				TAS2770_TDMConfigurationReg0,
				TAS2770_TDMConfigurationReg0_SAMPRATE31_Mask,
				TAS2770_TDMConfigurationReg0_SAMPRATE31_44_1_48kHz);
			break;
		case 44100:
			snd_soc_update_bits(codec,
				TAS2770_TDMConfigurationReg0,
				TAS2770_TDMConfigurationReg0_SAMPRATERAMP_Mask,
				TAS2770_TDMConfigurationReg0_SAMPRATERAMP_44_1KHz);
			snd_soc_update_bits(codec,
				TAS2770_TDMConfigurationReg0,
				TAS2770_TDMConfigurationReg0_SAMPRATE31_Mask,
				TAS2770_TDMConfigurationReg0_SAMPRATE31_44_1_48kHz);
			break;
		case 96000:
			snd_soc_update_bits(codec,
				TAS2770_TDMConfigurationReg0,
				TAS2770_TDMConfigurationReg0_SAMPRATERAMP_Mask,
				TAS2770_TDMConfigurationReg0_SAMPRATERAMP_48KHz);
			snd_soc_update_bits(codec,
				TAS2770_TDMConfigurationReg0,
				TAS2770_TDMConfigurationReg0_SAMPRATE31_Mask,
				TAS2770_TDMConfigurationReg0_SAMPRATE31_88_2_96kHz);
			break;
		case 88200:
			snd_soc_update_bits(codec,
				TAS2770_TDMConfigurationReg0,
				TAS2770_TDMConfigurationReg0_SAMPRATERAMP_Mask,
				TAS2770_TDMConfigurationReg0_SAMPRATERAMP_44_1KHz);
			snd_soc_update_bits(codec,
				TAS2770_TDMConfigurationReg0,
				TAS2770_TDMConfigurationReg0_SAMPRATE31_Mask,
				TAS2770_TDMConfigurationReg0_SAMPRATE31_88_2_96kHz);
			break;
		case 19200:
			snd_soc_update_bits(codec,
				TAS2770_TDMConfigurationReg0,
				TAS2770_TDMConfigurationReg0_SAMPRATERAMP_Mask,
				TAS2770_TDMConfigurationReg0_SAMPRATERAMP_48KHz);
			snd_soc_update_bits(codec,
				TAS2770_TDMConfigurationReg0,
				TAS2770_TDMConfigurationReg0_SAMPRATE31_Mask,
				TAS2770_TDMConfigurationReg0_SAMPRATE31_176_4_192kHz);
			break;
		case 17640:
			snd_soc_update_bits(codec,
				TAS2770_TDMConfigurationReg0,
				TAS2770_TDMConfigurationReg0_SAMPRATERAMP_Mask,
				TAS2770_TDMConfigurationReg0_SAMPRATERAMP_44_1KHz);
			snd_soc_update_bits(codec,
				TAS2770_TDMConfigurationReg0,
				TAS2770_TDMConfigurationReg0_SAMPRATE31_Mask,
				TAS2770_TDMConfigurationReg0_SAMPRATE31_176_4_192kHz);
			break;
		default:
			dev_dbg(pTAS2770->dev, "%s, unsupported sample rate\n", __func__);

	}
	snd_soc_update_bits(codec,
		TAS2770_ClockConfiguration,
		TAS2770_ClockConfiguration_DISCLKRateDetect10_Mask |
		TAS2770_ClockConfiguration_SBCLKtoFS52_Mask,
		TAS2770_ClockConfiguration_DISCLKRateDetect10_Disabled |
		TAS2770_ClockConfiguration_SBCLKtoFS52_64);
	return ret;
}

static int tas2770_set_dai_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	u8 tdm_rx_start_slot = 0, asi_cfg_1 = 0;
	struct snd_soc_codec *codec = dai->codec;
	struct tas2770_priv *pTAS2770 = snd_soc_codec_get_drvdata(codec);
	int ret = 0;

	dev_dbg(pTAS2770->dev, "%s, format=0x%x\n", __func__, fmt);

#if 1
	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBS_CFS:
		asi_cfg_1 = 0x00;
		break;
	default:
		dev_err(pTAS2770->dev, "ASI format master is not found\n");
		ret = -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		dev_dbg(pTAS2770->dev, "INV format: NBNF\n");
		asi_cfg_1 |= TAS2770_TDMConfigurationReg1_RXEDGE_Rising;
		break;
	case SND_SOC_DAIFMT_IB_NF:
		dev_dbg(pTAS2770->dev, "INV format: IBNF\n");
		asi_cfg_1 |= TAS2770_TDMConfigurationReg1_RXEDGE_Falling;
		break;
	default:
		dev_err(pTAS2770->dev, "ASI format Inverse is not found\n");
		ret = -EINVAL;
	}

	snd_soc_update_bits(codec, TAS2770_TDMConfigurationReg1,
		TAS2770_TDMConfigurationReg1_RXEDGE_Mask,
		asi_cfg_1);

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case (SND_SOC_DAIFMT_I2S):
		tdm_rx_start_slot = 1;
		break;
	case (SND_SOC_DAIFMT_DSP_A):
	case (SND_SOC_DAIFMT_DSP_B):
		tdm_rx_start_slot = 1;
		break;
	case (SND_SOC_DAIFMT_LEFT_J):
		tdm_rx_start_slot = 0;
		break;
	default:
		dev_err(pTAS2770->dev, "DAI Format is not found, fmt=0x%x\n", fmt);
		ret = -EINVAL;
		break;
	}

	snd_soc_update_bits(codec, TAS2770_TDMConfigurationReg1,
		TAS2770_TDMConfigurationReg1_RXOFFSET51_Mask,
		(tdm_rx_start_slot << TAS2770_TDMConfigurationReg1_RXOFFSET51_Shift));
#endif
	return ret;
}

static struct snd_soc_dai_ops tas2770_dai_ops = {
	.digital_mute = tas2770_mute,
	.hw_params  = tas2770_hw_params,
	.set_fmt    = tas2770_set_dai_fmt,
};

#define TAS2770_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S20_3LE |\
		SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S32_LE)

#define TAS2770_RATES (SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 \
						SNDRV_PCM_RATE_88200 |\
						SNDRV_PCM_RATE_96000 |\
						SNDRV_PCM_RATE_176400 |\
						SNDRV_PCM_RATE_192000\
						)

static struct snd_soc_dai_driver tas2770_dai_driver[] = {
	{
		.name = "tas2770 ASI1",
		.id = 0,
		.playback = {
			.stream_name    = "ASI1 Playback",
			.channels_min   = 2,
			.channels_max   = 2,
			.rates      = SNDRV_PCM_RATE_8000_192000,
			.formats    = TAS2770_FORMATS,
		},
		.capture = {
			.stream_name    = "ASI1 Capture",
			.channels_min   = 0,
			.channels_max   = 2,
			.rates          = SNDRV_PCM_RATE_8000_192000,
			.formats    = TAS2770_FORMATS,
		},
		.ops = &tas2770_dai_ops,
		.symmetric_rates = 1,
	},
};

static int tas2770_codec_probe(struct snd_soc_codec *codec)
{
	struct tas2770_priv *pTAS2770 = snd_soc_codec_get_drvdata(codec);

	dev_err(pTAS2770->dev, "%s\n", __func__);

	if (gpio_is_valid(pTAS2770->mnResetGPIO)) {
		gpio_set_value(pTAS2770->mnResetGPIO, 0);
		msleep(1);
		gpio_set_value(pTAS2770->mnResetGPIO, 1);
		gpio_direction_output(pTAS2770->mnResetGPIO, 1);
	}

	return 0;
}

static int tas2770_codec_remove(struct snd_soc_codec *codec)
{
	return 0;
}

/*
 * DAC digital volumes. From 11 dB to 21 dB in 0.5 dB steps
 */
static DECLARE_TLV_DB_SCALE(dac_tlv, 1100, 50, 0);

/*
 * DAC PCM volumes. -127.50 dB, 0.5dB steps
 */
static DECLARE_TLV_DB_SCALE(pcm_tlv, -12750, 50, 0);

static const struct snd_kcontrol_new tas2770_snd_controls[] = {
	SOC_SINGLE_TLV("DAC Playback Volume", TAS2770_PlaybackConfigurationReg0,
			0, 0x14, 0, dac_tlv),
	SOC_SINGLE_TLV("PCM Playback Volume", TAS2770_PlaybackConfigurationReg2,
			0, 0xff, 1, pcm_tlv),
//	SOC_SINGLE_EXT("TAS2770 PowerCtrl", SND_SOC_NOPM, 0, 0x0001, 0,
//			tas2770_power_ctrl_get, tas2770_power_ctrl_put),
};

static struct snd_soc_codec_driver soc_codec_driver_tas2770 = {
	.probe			= tas2770_codec_probe,
	.remove			= tas2770_codec_remove,
	.read			= tas2770_codec_read,
	.write			= tas2770_codec_write,
	.suspend		= tas2770_codec_suspend,
	.resume			= tas2770_codec_resume,
	.component_driver = {
		.controls		= tas2770_snd_controls,
		.num_controls		= ARRAY_SIZE(tas2770_snd_controls),
		.dapm_widgets		= tas2770_dapm_widgets,
		.num_dapm_widgets	= ARRAY_SIZE(tas2770_dapm_widgets),
		.dapm_routes		= tas2770_audio_map,
		.num_dapm_routes	= ARRAY_SIZE(tas2770_audio_map),
	},
};

int tas2770_register_codec(struct tas2770_priv *pTAS2770)
{
	int nResult = 0;

	dev_info(pTAS2770->dev, "%s, enter\n", __func__);
	nResult = snd_soc_register_codec(pTAS2770->dev,
		&soc_codec_driver_tas2770,
		tas2770_dai_driver, ARRAY_SIZE(tas2770_dai_driver));
        dev_info(pTAS2770->dev, "%s, enter result %d\n", __func__,nResult);
	return nResult;
}

int tas2770_deregister_codec(struct tas2770_priv *pTAS2770)
{
	snd_soc_unregister_codec(pTAS2770->dev);

	return 0;
}

MODULE_AUTHOR("Texas Instruments Inc.");
MODULE_DESCRIPTION("TAS2770 ALSA SOC Smart Amplifier driver");
MODULE_LICENSE("GPL v2");
#endif /* CONFIG_TAS2770_CODEC */
