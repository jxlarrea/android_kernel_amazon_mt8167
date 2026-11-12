/*
 * octave.c  --  octave ALSA SoC machine driver
 *
 * Copyright (c) 2016 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <sound/soc.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <sound/pcm_params.h>
#include <sound/external_amp_mute.h>
#include "../../codecs/tlv320aic3101.h"
#include "../../codecs/tlv320aic32x4.h"
#include "../../codecs/tlv320adc5140.h"

#define ENUM_TO_STR(enum) #enum

enum capture_configs {
	CAPTURE_CONFIG_TDM_ADC3101 = 0,
	CAPTURE_CONFIG_SPI_ADC3101,
	CAPTURE_CONFIG_TDM_AD5140,
	CAPTURE_CONFIG_SPI_ADC5140,
};

enum woofer_fault_state {
	NOTIFY_WOOF_FAULT = 0,
	NOTIFY_WOOF_OTW,
	NOTIFY_WOOF_OTE,
	WOOF_UNKNOWN,
};

/* enum uevent index */
enum uevent_string_index {
	NOTIFY_UEVENT_WOOFER_STATE = 0,
	NOTIFY_UEVENT_FAULT_PIN,
	NOTIFY_UEVENT_OTW_PIN,
	NOTIFY_UEVENT_KEY_MAX,
};

/* uevent strings */
static const char * const woofer_fault_str[] = {
	"WOOF_FAULT",
	"WOOF_OTW",
	"WOOF_OTE",
	"WOOF_UNKNOWN",
};

static char env_data[NOTIFY_UEVENT_KEY_MAX][32];

static uint32_t capture_cfg;

#define LINEIN_ADC_DIS 0
static int linein_adc_enab = LINEIN_ADC_DIS;

#define AMP_SHDN_DELAY_USEC 10000

/* IRQ value for gpio pin*/
static int woofer_fault_irq, woofer_otw_irq;
/* GPIO pin connected to Woofer Amp */
static int woofer_fault_gpiopin = -1, woofer_otw_gpiopin = -1;
/* Interrupt Debounce */
static int woofer_fault_debounce, woofer_otw_debounce;
/* Woofer Fault enabled */
static int woofer_fault_enabled;

/* HP Ext Amp Switch */
static const struct snd_kcontrol_new octave_hp_ext_amp_switch_ctrl =
	SOC_DAPM_SINGLE_VIRT("Switch", 1);

/* HP Spk Amp */
static int octave_hp_spk_amp_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;

	dev_info(card->dev, "%s snd, event %d\n", __func__, event);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		/* mt8167_evb_ext_hp_amp_turn_on(card); */
		break;
	case SND_SOC_DAPM_PRE_PMD:
		/* mt8167_evb_ext_hp_amp_turn_off(card); */
		break;
	default:
		break;
	}

	return 0;
}

/* Ext Spk Amp */
static int octave_ext_spk_amp_wevent(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;

	dev_info(card->dev, "%s, event %d\n", __func__, event);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		/* mt8167_evb_ext_spk_amp_turn_on(card); */
		break;
	case SND_SOC_DAPM_PRE_PMD:
		/* mt8167_evb_ext_spk_amp_turn_off(card); */
		break;
	default:
		break;
	}

	return 0;
}

enum PINCTRL_PIN_STATE {
	PIN_STATE_DEFAULT = 0,
	PIN_STATE_EXTAMP_ON,
	PIN_STATE_EXTAMP_OFF,
	PIN_STATE_EXTAMP_UNMUTE,
	PIN_STATE_EXTAMP_MUTE,
	PIN_STATE_AUXAUDIO_ON,
	PIN_STATE_AUXAUDIO_OFF,
	PIN_STATE_TDM_MIC_OFF,
	PIN_STATE_MAX
};

static const char * const octave_pinctrl_pin_str[PIN_STATE_MAX] = {
	"default",
	"extamp-on",
	"extamp-off",
	"extamp-unmute",
	"extamp-mute",
	"auxaudio-on",
	"auxaudio-off",
	"tdm-mic-off",
};

struct octave_priv {
	struct pinctrl *pinctrl;
	struct pinctrl_state *pin_states[PIN_STATE_MAX];
	struct regulator *amp_regulator;
	struct snd_soc_card *card;
	uint32_t ext_speaker_amp_on; /* kcontrol: Ext_Speaker_Amp_Switch */
	uint32_t ext_speaker_amp_mute; /* kcontrol: Ext_Speaker_Amp_Mute */
	uint32_t aux_audio_enable; /* kcontrol: Aux_Audio_Enable */
	bool linein_active; /* Linein is active */
	struct workqueue_struct *woofer_wq; /* Woofer Fault work queue */
	struct work_struct woofer_work; /* Woofer Fault handler */
};

struct octave_priv *octave_card_data;

static int __octave_ext_speaker_amp_mute_set(struct snd_soc_card *card,
	int ext_speaker_amp_mute);

static int tlv3101_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd;
	struct snd_soc_dai *codec_dai;
	unsigned int width, channels, rate, bclk;

	pr_info("aic:%s\n", __func__);
	if (substream == NULL) {
		pr_err("aic:%s invalid stream parameter\n", __func__);
		return -EINVAL;
	}

	rtd = substream->private_data;
	if (rtd == NULL) {
		pr_err("aic:%s invalid runtime parameter\n", __func__);
		return -EINVAL;
	}

	codec_dai = rtd->codec_dai;
	if (codec_dai == NULL) {
		pr_err("aic:%s invalid dai parameter\n", __func__);
		return -EINVAL;
	}

	rate = params_rate(params);
	channels = params_channels(params);
	width = params_width(params);

	pr_info("aic:%s rate=%u ch=%u width=%u\n", __func__,
		rate, channels, width);

	if (capture_cfg == CAPTURE_CONFIG_SPI_ADC3101) {
		/* ADCs output 8 channels */
		snd_soc_dai_set_bclk_ratio(codec_dai, 8*width);

		if (rate == 16000)
			snd_soc_dai_set_pll(codec_dai, AIC3101_PLL_BCLK,
				AIC3101_PLL_ADC_FS_CLKIN_MCLK,
				AIC31XX_FREQ_24576000, rate);
		else
			snd_soc_dai_set_pll(codec_dai, AIC3101_PLL_BCLK,
				AIC3101_PLL_ADC_FS_CLKIN_PLL_MCLK,
				AIC31XX_FREQ_24576000, rate);

		/* Set Codec Master in SPI Mode */
		snd_soc_dai_set_fmt(codec_dai, SND_SOC_DAIFMT_CBM_CFM |
				SND_SOC_DAIFMT_DSP_B | SND_SOC_DAIFMT_NB_NF);
	} else {
		bclk = width * channels * rate;
		pr_info("aic:%s bclk=%u\n", __func__, bclk);

		snd_soc_dai_set_pll(codec_dai, 0,
			AIC3101_ADC_FS_CLKIN_BCLK,
			bclk, params_rate(params));

		/* Set Codec Slave in TDM Mode */
		snd_soc_dai_set_fmt(codec_dai, SND_SOC_DAIFMT_CBS_CFS |
				SND_SOC_DAIFMT_DSP_B | SND_SOC_DAIFMT_NB_NF);
	}

	/* If line in is enabled at adc send new mask. New value only enables
	 * second channel from the 4th adc to propagate.
	 */
	if (linein_adc_enab)
		snd_soc_dai_set_tdm_slot(codec_dai, 0x00, 0x80,
					params_channels(params), 0);
	else
		snd_soc_dai_set_tdm_slot(codec_dai, 0x00, 0xff,
				params_channels(params),
				snd_pcm_format_width(params_format(params)));

	return 0;
}

static int tlv5140_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd;
	struct snd_soc_dai *codec_dai;
	int i;

	pr_info("%s\n", __func__);

	if (substream == NULL) {
		pr_err("%s invalid stream parameter\n", __func__);
		return -EINVAL;
	}

	rtd = substream->private_data;
	if (rtd == NULL) {
		pr_err("%s invalid runtime parameter\n", __func__);
		return -EINVAL;
	}

	for (i = 0; i < rtd->num_codecs; i++) {
		codec_dai = rtd->codec_dais[i];
		if (codec_dai == NULL) {
			pr_err("%s invalid dai parameter. i %d\n", __func__, i);
			return -EINVAL;
		}

		if (i == 0 && capture_cfg == CAPTURE_CONFIG_SPI_ADC5140) {
			/* Set MCLK rate in codec */
			snd_soc_dai_set_sysclk(codec_dai, 0,
				ADC5140_FREQ_12288000, SND_SOC_CLOCK_OUT);

			/* In SPI Config, make ADC_A Master */
			snd_soc_dai_set_fmt(codec_dai, SND_SOC_DAIFMT_CBM_CFM |
				SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF);
		} else
			snd_soc_dai_set_fmt(codec_dai, SND_SOC_DAIFMT_CBS_CFS |
				SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF);
	}

	return 0;
}

static int tlv5140_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd;
	struct snd_soc_dai *codec_dai;
	int i;

	pr_info("%s\n", __func__);

	if (substream == NULL) {
		pr_err("%s invalid stream parameter\n", __func__);
		return -EINVAL;
	}

	rtd = substream->private_data;
	if (rtd == NULL) {
		pr_err("%s invalid runtime parameter\n", __func__);
		return -EINVAL;
	}

	/* Need to power down slave before master */
	for (i = rtd->num_codecs - 1; i >= 0; i--) {
		codec_dai = rtd->codec_dais[i];
		if (codec_dai == NULL) {
			pr_err("%s invalid dai parameter. i %d\n", __func__, i);
			return -EINVAL;
		}

		adc5140_power_down(codec_dai);
	}

	return 0;
}

static int cs42526_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	pr_info("%s dir %d\n", __func__, substream->stream);

	if (substream == NULL) {
		pr_err("%s invalid stream parameter\n", __func__);
		return -EINVAL;
	}

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		octave_card_data->linein_active = true;

	return 0;
}


static int cs42526_hw_free(struct snd_pcm_substream *substream)
{
	pr_info("%s\n", __func__);

	if (substream == NULL) {
		pr_err("%s invalid stream parameter\n", __func__);
		return -EINVAL;
	}

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		octave_card_data->linein_active = false;

	return 0;
}

static struct snd_soc_ops tlv3101_machine_ops = {
	.hw_params = tlv3101_hw_params,
};

static struct snd_soc_ops tlv5140_machine_ops = {
	.hw_params = tlv5140_hw_params,
	.hw_free    = tlv5140_hw_free,
};

static struct snd_soc_ops cs42526_machine_ops = {
	.hw_params = cs42526_hw_params,
	.hw_free    = cs42526_hw_free,
};

static struct snd_soc_dai_link_component tlv320adc5140_codecs[] = {
	{
		.dai_name = "tlv320adc5140-codec-a",
	},
	{
		.dai_name = "tlv320adc5140-codec-b",
	},
};

/* Digital audio interface glue - connects codec <---> CPU */
static struct snd_soc_dai_link octave_dais[] = {
	/* Front End DAI links */
	{
		.name = "HDMI",
		.stream_name = "HMDI_PLayback",
		.cpu_dai_name = "HDMI",
		.codec_name = "snd-soc-dummy",
		.codec_dai_name = "snd-soc-dummy-dai",
		.trigger = {
			SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST
		},
		.dynamic = 1,
		.dpcm_playback = 1,
	},
	{
		.name = "TDM Capture",
		.stream_name = "TDM_Capture",
		.cpu_dai_name = "TDM_IN",
		.codec_name = "tlv320aic3101.0-0018",
		.codec_dai_name = "tlv320aic3101-codec",
		.dai_fmt = SND_SOC_DAIFMT_DSP_B | SND_SOC_DAIFMT_NB_NF |
			SND_SOC_DAIFMT_CBS_CFS,
		.trigger = {
			SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST
		},
		.ops = &tlv3101_machine_ops,
		.dynamic = 1,
		.dpcm_capture = 1,
	},
	{
		.name = "MultiMedia1 Capture",
		.stream_name = "MultiMedia1_Capture",
		.cpu_dai_name = "VUL",
		.codec_name = "cs42526.2-004c",
		.codec_dai_name = "cs42526-dai",
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
		SND_SOC_DAIFMT_CBS_CFS,
		.trigger = {
			SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST
		},
		.ops = &cs42526_machine_ops,
		.dynamic = 1,
		.dpcm_capture = 1,
	},
	{
		.name = "AWB Capture",
		.stream_name = "AWB_Record",
		.cpu_dai_name = "AWB",
		.codec_name = "snd-soc-dummy",
		.codec_dai_name = "snd-soc-dummy-dai",
		.trigger = {
			SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST
		},
		.dynamic = 1,
		.dpcm_capture = 1,
	},
#ifdef CONFIG_MTK_BTCVSD_ALSA
	{
		.name = "BTCVSD_RX",
		.stream_name = "BTCVSD_Capture",
		.cpu_dai_name = "snd-soc-dummy-dai",
		.platform_name = "mt-soc-btcvsd-rx-pcm",
		.codec_name = "snd-soc-dummy",
		.codec_dai_name = "snd-soc-dummy-dai",
	},
	{
		.name = "BTCVSD_TX",
		.stream_name = "BTCVSD_Playback",
		.cpu_dai_name = "snd-soc-dummy-dai",
		.platform_name = "mt-soc-btcvsd-tx-pcm",
		.codec_name = "snd-soc-dummy",
		.codec_dai_name = "snd-soc-dummy-dai",
	},
#endif
	{
		.name = "DL1 Playback",
		.stream_name = "DL1_Playback",
		.cpu_dai_name = "DL1",
		.codec_name = "snd-soc-dummy",
		.codec_dai_name = "snd-soc-dummy-dai",
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
		SND_SOC_DAIFMT_CBS_CFS,
		.trigger = {
			SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST
		},
		.dynamic = 1,
		.dpcm_playback = 1,
	},
	{
		.name = "Ref In Capture",
		.stream_name = "DL1_AWB_Record",
		.cpu_dai_name = "AWB",
		.codec_name = "snd-soc-dummy",
		.codec_dai_name = "snd-soc-dummy-dai",
		.trigger = {
			SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST
		},
		.dynamic = 1,
		.dpcm_capture = 1,
	},
	{
		.name = "DL2 Playback",
		.stream_name = "MultiMedia2_PLayback",
		.cpu_dai_name = "DL2",
		.codec_name = "snd-soc-dummy",
		.codec_dai_name = "snd-soc-dummy-dai",
		.trigger = {
			SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST
		},
		.dynamic = 1,
		.dpcm_playback = 1,
	},
	{
		.name = "DAI Capture",
		.stream_name = "VOIP_Call_BT_Capture",
		.cpu_dai_name = "DAI",
		.codec_name = "snd-soc-dummy",
		.codec_dai_name = "snd-soc-dummy-dai",
		.trigger = {
			SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST
		},
		.dynamic = 1,
		.dpcm_capture = 1,
	},
	{
		.name = "ULM FE",
		.stream_name = "ULM_Capture",
		.cpu_dai_name = "ULM",
		.codec_name = "snd-soc-dummy",
		.codec_dai_name = "snd-soc-dummy-dai",
		.trigger = {
			SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST
		},
		.dynamic = 1,
		.dpcm_capture = 1,
	},
	/* Backend End DAI links */
	{
		.name = "HDMI BE",
		.cpu_dai_name = "HDMIO",
		.no_pcm = 1,
		.codec_name = "cs42526.2-004c",
		.codec_dai_name = "cs42526-dai",
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
			   SND_SOC_DAIFMT_CBS_CFS,
		.dpcm_playback = 1,
	},
	{
		.name = "2ND EXT Codec",
		.cpu_dai_name = "2ND I2S",
		.no_pcm = 1,
		.codec_name = "linux_bt_sco_codec",
		.codec_dai_name = "bt-sco-pcm-wb",
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
			   SND_SOC_DAIFMT_CBS_CFS,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
	},
	{
		.name = "MTK Codec",
		.cpu_dai_name = "INT ADDA",
		.no_pcm = 1,
		.codec_name = "snd-soc-dummy",
		.codec_dai_name = "snd-soc-dummy-dai",
		.dpcm_playback = 1,
		.dpcm_capture = 1,
	},
	{
		.name = "TDM IN BE",
		.cpu_dai_name = "TDM_IN_IO",
		.no_pcm = 1,
		.codec_name = "snd-soc-dummy",
		.codec_dai_name = "snd-soc-dummy-dai",
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
				SND_SOC_DAIFMT_CBS_CFS,
		.dpcm_capture = 1,
	},
	{
		.name = "I2S BE",
		.cpu_dai_name = "I2S",
		.no_pcm = 1,
		.codec_name = "snd-soc-dummy",
		.codec_dai_name = "snd-soc-dummy-dai",
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
		SND_SOC_DAIFMT_CBS_CFS,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
	},
	{
		.name = "DL BE",
		.cpu_dai_name = "DL Input",
		.no_pcm = 1,
		.codec_name = "snd-soc-dummy",
		.codec_dai_name = "snd-soc-dummy-dai",
		.dpcm_capture = 1,
	},
	{
		.name = "HW Gain1 BE",
		.cpu_dai_name = "HW_GAIN1",
		.no_pcm = 1,
		.codec_name = "snd-soc-dummy",
		.codec_dai_name = "snd-soc-dummy-dai",
		.dpcm_capture = 1,
	},
	{
		.name = "INTDIR BE",
		.cpu_dai_name = "INTDIR_IO",
		.no_pcm = 1,
		.codec_name = "snd-soc-dummy",
		.codec_dai_name = "snd-soc-dummy-dai",
		.dpcm_capture = 1,
	},
};

static struct snd_soc_dai_link octave_dais_alt[] = {
	{
		.name = "TDM Capture",
		.stream_name = "TDM_Capture",
		.cpu_dai_name = "amzn-mt-spi-pcm",
		.platform_name = "amzn-mt-spi-pcm",
		.codec_name = "tlv320aic3101.0-0018",
		.codec_dai_name = "tlv320aic3101-codec",
		.dai_fmt = SND_SOC_DAIFMT_DSP_B | SND_SOC_DAIFMT_NB_NF |
			SND_SOC_DAIFMT_CBM_CFM,
		.trigger = {
			SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST
		},
		.ops = &tlv3101_machine_ops,
		.dpcm_capture = 1,
	},
	{
		.name = "TDM Capture",
		.stream_name = "TDM_Capture",
		.cpu_dai_name = "TDM_IN",
		.codecs = tlv320adc5140_codecs,
		.num_codecs = 2,
		.ops = &tlv5140_machine_ops,
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
			SND_SOC_DAIFMT_CBS_CFS,
		.trigger = {
			SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST
		},
		.dynamic = 1,
		.dpcm_capture = 1,
	},
	{
		.name = "TDM Capture",
		.stream_name = "TDM_Capture",
		.cpu_dai_name = "amzn-mt-spi-pcm",
		.platform_name = "amzn-mt-spi-pcm",
		.codecs = tlv320adc5140_codecs,
		.num_codecs = 2,
		.ops = &tlv5140_machine_ops,
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
			SND_SOC_DAIFMT_CBM_CFM,
		.trigger = {
			SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST
		},
		.capture_only = 1,
	},
};

static const struct snd_soc_dapm_widget octave_dapm_widgets[] = {
	SND_SOC_DAPM_OUTPUT("External Line Out"),
	SND_SOC_DAPM_INPUT("External Line In"),
	SND_SOC_DAPM_INPUT("Mic Array In"),
	SND_SOC_DAPM_INPUT("SPDIF In"),

	SND_SOC_DAPM_SPK("HP Spk Amp", octave_hp_spk_amp_event),
	SND_SOC_DAPM_SWITCH("HP Ext Amp",
		SND_SOC_NOPM, 0, 0, &octave_hp_ext_amp_switch_ctrl),

	SND_SOC_DAPM_SPK("Ext Spk Amp", octave_ext_spk_amp_wevent),
};

static const struct snd_soc_dapm_route octave_audio_map[] = {
	{"External Line Out", NULL, "HDMIO Playback"},
	{"I2S Capture", NULL, "External Line In"},
	{"TDM_IN", NULL, "Mic Array In"},
	{"INTDIR Capture", NULL, "SPDIF In"},

	/* use external spk amp via AU_LOL */
	{"Ext Spk Amp", NULL, "LINEOUT Ext Amp"},

	{"HP Spk Amp", NULL, "HP Ext Amp"},
	{"HP Spk Amp", NULL, "HP Ext Amp"},

	/* ADDA clock - Uplink */
	{"AIF TX", NULL, "AFE_CLK"},
	{"AIF TX", NULL, "AD_CLK"},

	/* ADDA clock - Downlink */
	{"AIF RX", NULL, "AFE_CLK"},
	{"AIF RX", NULL, "DA_CLK"},
};

static int __octave_ext_speaker_amp_get(struct snd_soc_card *card)
{
	struct octave_priv *card_data = snd_soc_card_get_drvdata(card);

	return card_data->ext_speaker_amp_on;
}

/* Ext Speaker Amp Switch */
static int octave_ext_speaker_amp_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);

	ucontrol->value.integer.value[0] = __octave_ext_speaker_amp_get(card);

	return 0;
}

static int __octave_ext_speaker_amp_set(struct snd_soc_card *card,
				       int ext_speaker_amp_on)
{
	int retval = 0;
	struct octave_priv *card_data = snd_soc_card_get_drvdata(card);

	dev_info(card->dev, "%s value %d\n", __func__, ext_speaker_amp_on);

	if (ext_speaker_amp_on == 1) {
		if (!IS_ERR(card_data->pin_states[PIN_STATE_EXTAMP_ON])) {
			retval = pinctrl_select_state(card_data->pinctrl,
				card_data->pin_states[PIN_STATE_EXTAMP_ON]);
			if (retval) {
				dev_err(card->dev,
					"could not set pin_states[PIN_STATE_EXTAMP_ON] pins\n");
			} else {
				card_data->ext_speaker_amp_on = 1;
			}
		}
	} else {
		if (!IS_ERR(card_data->pin_states[PIN_STATE_EXTAMP_OFF])) {
			retval = pinctrl_select_state(card_data->pinctrl,
				card_data->pin_states[PIN_STATE_EXTAMP_OFF]);
			if (retval) {
				dev_err(card->dev,
					"could not set pin_states[PIN_STATE_EXTAMP_OFF] pins\n");
			} else {
				card_data->ext_speaker_amp_on = 0;
			}
		}
	}
	return retval;
}

static int octave_ext_speaker_amp_set(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	uint32_t ext_speaker_amp_on = ucontrol->value.integer.value[0];

	return __octave_ext_speaker_amp_set(card, ext_speaker_amp_on);
}

static int octave_linein_adc_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = linein_adc_enab;
	return 0;
}

static int octave_linein_adc_set(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	uint32_t linein = ucontrol->value.enumerated.item[0];

	linein_adc_enab = linein ? 1 : 0;
	dev_info(card->dev, "%s value %d\n", __func__, linein_adc_enab);
	return 0;
}

/* Ext Speaker Amp Switch */
static int __octave_ext_speaker_amp_mute_get(struct snd_soc_card *card)
{
	struct octave_priv *card_data = snd_soc_card_get_drvdata(card);

	return card_data->ext_speaker_amp_mute;
}

static int octave_ext_speaker_amp_mute_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);

	ucontrol->value.integer.value[0] =
		__octave_ext_speaker_amp_mute_get(card);
	return 0;
}

static int __octave_ext_speaker_amp_mute_set(struct snd_soc_card *card,
	int ext_speaker_amp_mute)
{
	int retval = 0;
	struct octave_priv *card_data = snd_soc_card_get_drvdata(card);

	dev_info(card->dev, "%s value %d\n", __func__, ext_speaker_amp_mute);

	if (ext_speaker_amp_mute == 1) {
		if (!IS_ERR(card_data->pin_states[PIN_STATE_EXTAMP_MUTE])) {
			retval = pinctrl_select_state(card_data->pinctrl,
				card_data->pin_states[PIN_STATE_EXTAMP_MUTE]);
			if (retval) {
				dev_err(card->dev,
					"could not set pin_states[PIN_STATE_EXTAMP_ON] pins\n");
			} else {
				card_data->ext_speaker_amp_mute = 1;
			}
		}
	} else {
		if (!IS_ERR(card_data->pin_states[PIN_STATE_EXTAMP_UNMUTE])) {
			retval = pinctrl_select_state(card_data->pinctrl,
				card_data->pin_states[PIN_STATE_EXTAMP_UNMUTE]);
			if (retval) {
				dev_err(card->dev,
					"could not set pin_states[PIN_STATE_EXTAMP_OFF] pins\n");
			} else {
				card_data->ext_speaker_amp_mute = 0;
			}
		}
	}
	return retval;
}

static int octave_ext_speaker_amp_mute_set(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	uint32_t ext_speaker_amp_mute = ucontrol->value.integer.value[0];

	return __octave_ext_speaker_amp_mute_set(card, ext_speaker_amp_mute);
}

void external_amp_mute(bool mute)
{
	if (octave_card_data == NULL) {
		pr_err("%s: octave driver not initialized\n", __func__);
		return;
	}

	if (octave_card_data->linein_active == false && mute)
		pr_info("%s: Ignoring. linein_active %d mute %d\n",
			__func__, octave_card_data->linein_active, mute);
	else
		__octave_ext_speaker_amp_mute_set(octave_card_data->card, mute);
}
EXPORT_SYMBOL(external_amp_mute);

/* Aux Audio Input Enable */
static int octave_aux_audio_enable_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct octave_priv *card_data = snd_soc_card_get_drvdata(card);

	ucontrol->value.integer.value[0] = card_data->aux_audio_enable;
	return 0;
}

static int octave_aux_audio_enable_set(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	int retval = 0;
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct octave_priv *card_data = snd_soc_card_get_drvdata(card);
	uint32_t aux_audio = ucontrol->value.integer.value[0];

	dev_info(card->dev, "%s value %d\n", __func__, aux_audio);

	if (aux_audio) {
		if (!IS_ERR(card_data->pin_states[PIN_STATE_AUXAUDIO_ON])) {
			retval = pinctrl_select_state(card_data->pinctrl,
				card_data->pin_states[PIN_STATE_AUXAUDIO_ON]);
			if (retval) {
				dev_err(card->dev,
					"could not set pin_states[PIN_STATE_AUXAUDIO_ON] pins\n");
			} else {
				card_data->aux_audio_enable = 1;
			}
		}
	} else {
		if (!IS_ERR(card_data->pin_states[PIN_STATE_AUXAUDIO_OFF])) {
			retval = pinctrl_select_state(card_data->pinctrl,
				card_data->pin_states[PIN_STATE_AUXAUDIO_OFF]);
			if (retval) {
				dev_err(card->dev,
					"could not set pin_states[PIN_STATE_AUXAUDIO_OFF] pins\n");
			} else {
				card_data->aux_audio_enable = 0;
			}
		}
	}
	return retval;
}

/***********************************************************************
 * __octave_amp_reset
 * Reset Amplifer to clear FAULT
 * *********************************************************************/
static void __octave_amp_reset(struct snd_soc_card *card)
{
	int ret;

	dev_info(card->dev, "%s\n", __func__);

	/* Mute Amp */
	ret = __octave_ext_speaker_amp_mute_set(card, 1);
	if (ret)
		dev_err(card->dev, "%s failed to mute amp(%d)",
			__func__, ret);

	/* Turn Off Amp */
	ret = __octave_ext_speaker_amp_set(card, 0);
	if (ret)
		dev_err(card->dev, "%s failed to turn off amp(%d)",
			__func__, ret);

	udelay(AMP_SHDN_DELAY_USEC);

	/* Turn On Amp */
	ret = __octave_ext_speaker_amp_set(card, 1);
	if (ret)
		dev_err(card->dev, "%s failed to turn off amp(%d)",
			__func__, ret);

	/* Unmute Amp */
	ret = __octave_ext_speaker_amp_mute_set(card, 0);
	if (ret)
		dev_err(card->dev, "%s failed to mute amp(%d)",
			__func__, ret);
}

static int octave_woofer_fault_enable_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	pr_info("%s: woofer_fault_enab = %d\n", __func__, woofer_fault_enabled);

	ucontrol->value.integer.value[0] = woofer_fault_enabled;
	return 0;
}

static int octave_woofer_fault_enable_set(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	woofer_fault_enabled = ucontrol->value.integer.value[0];

	pr_info("%s: woofer_fault_enab = %d\n", __func__,
			woofer_fault_enabled);
	return 0;
}

static int octave_woofer_otw_status_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	pr_info("%s: woofer_otw_gpiopin=%d, woofer_fault_enab=%d\n",
		__func__, woofer_otw_gpiopin, woofer_fault_enabled);

	if (woofer_otw_gpiopin < 0) {
		pr_err("%s: woofer_otw_gpiopin pin=%d. Cannot proceed!\n",
			__func__, woofer_otw_gpiopin);
		return -1;
	}

	ucontrol->value.integer.value[0] =
		gpio_get_value(woofer_otw_gpiopin) ? 0:1;

	return 0;
}

static int octave_woofer_otw_status_set(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	int reset_status = ucontrol->value.integer.value[0];
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);

	pr_info("%s: reset = %d\n", __func__,
			reset_status);

	if (reset_status)
		__octave_amp_reset(card);

	return 0;
}

static const struct snd_kcontrol_new octave_controls[] = {
    /* Ext Spk Amp Switch On/Off */
	SOC_SINGLE_BOOL_EXT("Ext_Speaker_Amp_Switch",
		0,
		octave_ext_speaker_amp_get,
		octave_ext_speaker_amp_set),
	/* Ext Spk Amp Mute On/Off */
	SOC_SINGLE_BOOL_EXT("Ext_Speaker_Amp_Mute",
		0,
		octave_ext_speaker_amp_mute_get,
		octave_ext_speaker_amp_mute_set),
	/* Aux Audio Input Enable */
	SOC_SINGLE_BOOL_EXT("Aux_Audio_Enable",
		0,
		octave_aux_audio_enable_get,
		octave_aux_audio_enable_set),
	SOC_SINGLE_BOOL_EXT("LineIn ADC",
		0,
		octave_linein_adc_get,
		octave_linein_adc_set),
	/* Woofer error handling */
	SOC_SINGLE_BOOL_EXT("Woofer Fault Handler",
		0,
		octave_woofer_fault_enable_get,
		octave_woofer_fault_enable_set),
	/* Woofer OTW Status */
	SOC_SINGLE_BOOL_EXT("Woofer OTW",
		0,
		octave_woofer_otw_status_get,
		octave_woofer_otw_status_set),
};

static int octave_suspend(struct snd_soc_card *card)
{
	struct octave_priv *card_data;
	int ret = 0;

	card_data = snd_soc_card_get_drvdata(card);

	/* Step 1: mute amp */
	ret = __octave_ext_speaker_amp_mute_set(card, 0);
	if (ret) {
		dev_err(card->dev, "%s failed to mute amp(%d)",
			__func__, ret);
		return ret;
	}

	/* Step 2: turn off amp */
	ret = __octave_ext_speaker_amp_set(card, 0);
	if (ret) {
		dev_err(card->dev, "%s failed to turn off amp(%d)",
			__func__, ret);
		return ret;
	}

	/* Step 3: turn off amp power */
	if (!IS_ERR(card_data->amp_regulator)) {
		ret = regulator_disable(card_data->amp_regulator);
		if (ret) {
			dev_err(card->dev, "%s failed to disable amp regulator(%d)",
				__func__, ret);
			return ret;
		}
	}
	return ret;
}

static int octave_resume(struct snd_soc_card *card)
{
	struct octave_priv *card_data;
	int ret = 0;

	card_data = snd_soc_card_get_drvdata(card);

	/*
	 * Turn on amp power only
	 * Audioserver restarts on resume and will turn on/unmute amp
	 * when needed
	 */
	if (!IS_ERR(card_data->amp_regulator)) {
		ret = regulator_enable(card_data->amp_regulator);
		if (ret)
			dev_err(card->dev, "%s failed to enable amp regulator(%d)",
				__func__, ret);
	}

	return ret;
}

static struct snd_soc_card octave_card = {
	.name = "mt-snd-card",
	.owner = THIS_MODULE,
	.dai_link = octave_dais,
	.num_links = ARRAY_SIZE(octave_dais),
	.controls = octave_controls,
	.num_controls = ARRAY_SIZE(octave_controls),
	.dapm_widgets = octave_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(octave_dapm_widgets),
	.dapm_routes = octave_audio_map,
	.num_dapm_routes = ARRAY_SIZE(octave_audio_map),
	.suspend_pre = octave_suspend,
	.resume_post = octave_resume,
};

static const struct of_device_id octave_dt_match[] = {
	{ .compatible = "mediatek,mt8516-soc-octave-card", },
	{ }
};
MODULE_DEVICE_TABLE(of, octave_dt_match);

/***********************************************************************
 * octave_woofer_fault_work
 * Handle Woofer FAULT and OTW after reading
 * *********************************************************************/

static void octave_woofer_fault_work(struct work_struct *work)
{
	int fault_status = 0, otw_status = 0;
	bool report_uevent = true;
	char *envp[NOTIFY_UEVENT_KEY_MAX + 1] = { NULL };
	struct octave_priv *card_data = container_of(work, struct octave_priv,
								woofer_work);

	if (!woofer_fault_enabled) {
		pr_err("%s: woofer_fault not enabled!\n", __func__);
		return;
	}

	fault_status = gpio_get_value(woofer_fault_gpiopin);
	otw_status = gpio_get_value(woofer_otw_gpiopin);

	pr_err("%s: fault_status=%d otw_status=%d\n", __func__, fault_status,
			otw_status);

	snprintf(env_data[NOTIFY_UEVENT_FAULT_PIN],
		sizeof(env_data[NOTIFY_UEVENT_FAULT_PIN]),
		"FAULT=%d", fault_status);
	snprintf(env_data[NOTIFY_UEVENT_OTW_PIN],
		sizeof(env_data[NOTIFY_UEVENT_OTW_PIN]),
		"OTW_CLIP=%d", otw_status);

	envp[NOTIFY_UEVENT_WOOFER_STATE] = env_data[NOTIFY_UEVENT_WOOFER_STATE];
	envp[NOTIFY_UEVENT_FAULT_PIN] = env_data[NOTIFY_UEVENT_FAULT_PIN];
	envp[NOTIFY_UEVENT_OTW_PIN] = env_data[NOTIFY_UEVENT_OTW_PIN];

	if (fault_status == 0 && otw_status == 0) {
		/* Critical Over termperature error. Amp in fault state */
		pr_err("%s: Woofer Over Temperature Error. Amplifier Reset!\n",
			   __func__);

		snprintf(env_data[NOTIFY_UEVENT_WOOFER_STATE],
			sizeof(env_data[NOTIFY_UEVENT_WOOFER_STATE]), "%s",
			woofer_fault_str[NOTIFY_WOOF_OTE]);

		kobject_uevent_env(&card_data->card->dev->kobj,
			KOBJ_CHANGE, envp);
		report_uevent = false; /* Prevent double reporting */

		/* TODO: Let userspace reset Amp here*/
		__octave_amp_reset(card_data->card);

	} else if (otw_status == 0) {
		/* Warning for over termperature */
		pr_err("%s: Woofer Over Temperature Warning. Action needed!\n",
			   __func__);

		snprintf(env_data[NOTIFY_UEVENT_WOOFER_STATE],
			sizeof(env_data[NOTIFY_UEVENT_WOOFER_STATE]), "%s",
			woofer_fault_str[NOTIFY_WOOF_OTW]);

	}  else if (fault_status == 0) {
		/* Generic Amp Fault without temperature */
		pr_err("%s: Woofer Generic Fault detected. Amplifier Reseting!\n",
					   __func__);
		/* Reset Amplifier */
		__octave_amp_reset(card_data->card);

		snprintf(env_data[NOTIFY_UEVENT_WOOFER_STATE],
			sizeof(env_data[NOTIFY_UEVENT_WOOFER_STATE]), "%s",
			woofer_fault_str[NOTIFY_WOOF_FAULT]);

	} else {
		pr_err("%s: unknown state. fault status=%d  otw status=%d!\n",
			__func__, fault_status, otw_status);
		report_uevent = false;
	}

	if (report_uevent)
		kobject_uevent_env(&card_data->card->dev->kobj,
				KOBJ_CHANGE, envp);

	enable_irq(woofer_fault_gpiopin);
	enable_irq(woofer_otw_gpiopin);
}

/***********************************************************************
 * octave_woofer_irq_callback
 * Interrupt callback for Woofer Amplifier FAULT and OTW
 * *********************************************************************/
static irqreturn_t octave_woofer_irq_callback(int irq, void *dev)
{
	struct octave_priv *card_data;

	pr_info("%s\n", __func__);

	if (!woofer_fault_enabled || woofer_otw_gpiopin < 0 ||
		woofer_fault_gpiopin < 0) {
		pr_err("%s: enabled=%d. woofer_fault=%d  woof_otw=%d Cannot proceed!\n",
			__func__, woofer_fault_enabled,
			woofer_fault_gpiopin, woofer_otw_gpiopin);
		return IRQ_HANDLED;
	}

	card_data = snd_soc_card_get_drvdata((struct snd_soc_card *)dev);

	disable_irq_nosync(woofer_fault_gpiopin);
	disable_irq_nosync(woofer_otw_gpiopin);

	queue_work(card_data->woofer_wq, &(card_data->woofer_work));

	return IRQ_HANDLED;
}

static int octave_irq_probe(struct snd_soc_card *card)
{
	struct octave_priv *card_data;
	struct device_node *node = NULL;
	int ret = 0;

	dev_info(card->dev, "%s\n", __func__);

	node = of_find_matching_node(node, octave_dt_match);
	if (!node) {
		pr_err("%s: device tree not found!\n", __func__);
		return -1;
	}

	card_data = snd_soc_card_get_drvdata(card);

	/* Setup Woofer fault if gpio is provided */
	woofer_fault_gpiopin = of_get_named_gpio(node, "woofer-fault-gpio",
				0);
	if (woofer_fault_gpiopin < 0) {
		pr_err("%s: woofer_fault_gpio pin info not found!\n",
			__func__);
		return -1;
	}

	/* Setup Woofer OTW if gpio is provided */
	woofer_otw_gpiopin = of_get_named_gpio(node, "woofer-otw-gpio",
				0);
	if (woofer_otw_gpiopin < 0) {
		pr_err("%s: woofer_otw_gpio pin info not found!\n",
			__func__);
		return -1;
	}

	of_property_read_u32(node, "woofer-fault-debounce",
		&woofer_fault_debounce);

	ret = devm_gpio_request(card->dev, woofer_fault_gpiopin,
		"woof-fault-gpio");
	if (ret) {
		pr_err("%s: woofer_fault pin=%d. gpio_request failed. Error=%d\n",
			__func__, woofer_fault_gpiopin, ret);
		return ret;
	}

	gpio_direction_input(woofer_fault_gpiopin);
	gpio_set_debounce(woofer_fault_gpiopin, woofer_fault_debounce);

	woofer_fault_irq = gpio_to_irq(woofer_fault_gpiopin);

	ret = request_irq(woofer_fault_irq, octave_woofer_irq_callback,
		IRQF_TRIGGER_FALLING, "wooffault-eint", card);
	if (ret) {
		pr_err("%s: woof_fault request_irq failed. Error=%d\n",
			__func__, ret);
		return ret;
	}

	pr_info("%s: woofer_fault pin=%d irq=%d, deb=%d\n",
		__func__, woofer_fault_gpiopin, woofer_fault_irq,
		woofer_fault_debounce);

	of_property_read_u32(node, "woofer-otw-debounce",
		&woofer_otw_debounce);

	ret = devm_gpio_request(card->dev, woofer_otw_gpiopin, "woof-otw-gpio");
	if (ret) {
		pr_err("%s: woofer_otw pin=%d. gpio_request failed. Error=%d\n",
			__func__, woofer_otw_gpiopin, ret);
		return ret;
	}

	gpio_direction_input(woofer_otw_gpiopin);
	gpio_set_debounce(woofer_otw_gpiopin, woofer_otw_debounce);

	woofer_otw_irq = gpio_to_irq(woofer_otw_gpiopin);

	ret = request_irq(woofer_otw_irq, octave_woofer_irq_callback,
		IRQF_TRIGGER_FALLING, "woofotw-eint", card);
	if (ret) {
		pr_err("%s: woof_otw request_irq failed. Error=%d\n",
			__func__, ret);
		return ret;
	}

	card_data->woofer_wq = alloc_workqueue("octave_woofer_fault_wq",
				WQ_MEM_RECLAIM, 0);
	if (card_data->woofer_wq == NULL) {
		pr_err("%s:Couldn't allocate Woofer WQ\n", __func__);
		return -ENOMEM;
	}

	INIT_WORK(&(card_data->woofer_work), octave_woofer_fault_work);

	pr_info("%s: woofer_otw pin=%d irq=%d, deb=%d\n",
		__func__, woofer_otw_gpiopin, woofer_otw_irq,
		woofer_otw_debounce);

	return ret;
}

static int octave_regulator_probe(struct snd_soc_card *card)
{
	struct octave_priv *card_data;
	int ret = 0;

	dev_info(card->dev, "%s+ snd\n", __func__);

	card_data = snd_soc_card_get_drvdata(card);

	card_data->amp_regulator = devm_regulator_get(card->dev,
					"amp-regulator");
	if (IS_ERR(card_data->amp_regulator)) {
		dev_warn(card->dev, "failed to get amp regulator\n");
	} else {
		ret = regulator_enable(card_data->amp_regulator);
		if (ret) {
			dev_err(card->dev,
				"failed to enable regulator(%d)\n", ret);
			return ret;
		}
	}
	return ret;
}

static int octave_gpio_probe(struct snd_soc_card *card)
{
	struct octave_priv *card_data;
	int ret = 0;
	int i;

	dev_info(card->dev, "%s+ snd\n", __func__);

	card_data = snd_soc_card_get_drvdata(card);

	card_data->pinctrl = devm_pinctrl_get(card->dev);
	if (IS_ERR(card_data->pinctrl)) {
		ret = PTR_ERR(card_data->pinctrl);
		dev_err(card->dev, "%s pinctrl_get failed %d\n",
			__func__, ret);
		goto exit;
	}

	for (i = 0 ; i < PIN_STATE_MAX ; i++) {
		card_data->pin_states[i] =
			pinctrl_lookup_state(card_data->pinctrl,
				octave_pinctrl_pin_str[i]);
		if (IS_ERR(card_data->pin_states[i])) {
			ret = PTR_ERR(card_data->pin_states[i]);
			dev_warn(card->dev, "%s Can't find pinctrl state %s %d\n",
				__func__, octave_pinctrl_pin_str[i], ret);
		}
	}

	/* default state */
	if (!IS_ERR(card_data->pin_states[PIN_STATE_DEFAULT])) {
		ret = pinctrl_select_state(card_data->pinctrl,
				card_data->pin_states[PIN_STATE_DEFAULT]);
		if (ret) {
			dev_err(card->dev, "%s failed to Default state %d\n",
				__func__, ret);
			goto exit;
		}
	}

	/* Turn off TDM Input if SPI is enabled */
	if (capture_cfg == CAPTURE_CONFIG_SPI_ADC3101 ||
	    capture_cfg == CAPTURE_CONFIG_SPI_ADC5140) {
		if (!IS_ERR(card_data->pin_states[PIN_STATE_TDM_MIC_OFF])) {
			ret = pinctrl_select_state(card_data->pinctrl,
				card_data->pin_states[PIN_STATE_TDM_MIC_OFF]);
			if (ret) {
				dev_err(card->dev, "%s failed to TDM Mic Off state %d\n",
					__func__, ret);
				goto exit;
			}
		}
	}

	/*
	 * Default state (Ext AMP Off), Ext_Speaker_Amp_Switch
	 * Defined in dts by aud_pins_extamp_default
	 */
	card_data->ext_speaker_amp_on = 0;
	/*
	 * Default state (Ext AMP Mute), Ext_Speaker_Amp_Mute
	 * Defined in dts by aud_pins_extamp_default
	 */
	card_data->ext_speaker_amp_mute = 1;
    /*
     * Default state (Aux_Audio_Enable AMP off), Aux_Audio_Enable
     * This enables uart debugging.
     * Defined in dts by aud_pins_dac_mux_default
     */
	card_data->aux_audio_enable = 0;
exit:

	dev_info(card->dev, "%s- snd %d\n", __func__, ret);
	return ret;
}

void octave_gpio_release(struct snd_soc_card *card)
{
	struct octave_priv *card_data;

	dev_info(card->dev, "%s\n", __func__);

	card_data = snd_soc_card_get_drvdata(card);

	if (card_data->woofer_wq) {
		destroy_workqueue(card_data->woofer_wq);
		card_data->woofer_wq = NULL;
	}

	if (IS_ERR(card_data->pinctrl)) {
		dev_err(card->dev, "%s pinctrl not initialized\n",
			__func__);
		return;
	}

	devm_pinctrl_put(card_data->pinctrl);
}


static int octave_dev_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &octave_card;
	struct device_node *platform_node;
	struct device_node *codec_node;
	int ret, i;
	struct octave_priv *card_data;

	dev_info(&pdev->dev, "%s+ snd\n", __func__);

	if (of_property_read_u32(pdev->dev.of_node, "amzn,capture-config",
				&capture_cfg))
		capture_cfg = CAPTURE_CONFIG_TDM_ADC3101;

	if (capture_cfg != CAPTURE_CONFIG_TDM_ADC3101) {
		for (i = 0; i < card->num_links; i++) {
			if (!strcmp(octave_dais[i].name, "TDM Capture")) {
				dev_info(&pdev->dev, "%s capture-config %d. Insert at %d\n",
					__func__, capture_cfg, i);
				/* index is 1 less than mode */
				octave_dais[i] = octave_dais_alt[capture_cfg-1];
				break;
			}
		}
	} else
		dev_info(&pdev->dev, "%s capture-config not set. Using TDM with adc3101\n",
			__func__);

	platform_node = of_parse_phandle(pdev->dev.of_node,
					 "mediatek,platform", 0);
	if (!platform_node) {
		dev_err(&pdev->dev, "Property 'platform' missing or invalid\n");
		return -EINVAL;
	}

	for (i = 0; i < card->num_links; i++) {
		if (octave_dais[i].platform_name)
			continue;
		octave_dais[i].platform_of_node = platform_node;
	}

	if (capture_cfg == CAPTURE_CONFIG_TDM_AD5140 ||
		capture_cfg == CAPTURE_CONFIG_SPI_ADC5140) {
		tlv320adc5140_codecs[0].of_node =
			of_parse_phandle(pdev->dev.of_node,
						"mediatek,audio-codec", 0);
		if (!tlv320adc5140_codecs[0].of_node) {
			dev_err(&pdev->dev,
				"Property 'audio-codec':0 missing or invalid\n");
			return -EINVAL;
		}
		tlv320adc5140_codecs[1].of_node =
			of_parse_phandle(pdev->dev.of_node,
						"mediatek,audio-codec", 1);
		if (!tlv320adc5140_codecs[1].of_node) {
			dev_err(&pdev->dev,
				"Property 'audio-codec':1 missing or invalid\n");
			return -EINVAL;
		}
	}

	codec_node = of_parse_phandle(pdev->dev.of_node,
					 "mediatek,audio-codec", 2);
	if (codec_node) {
		for (i = 0; i < card->num_links; i++) {
			if (octave_dais[i].codec_name ||
				octave_dais[i].num_codecs)
				continue;

			octave_dais[i].codec_of_node = codec_node;
			dev_info(&pdev->dev, "%s codec node matched d=%d\n",
				__func__, i);
			break;
		}
	}

	card->dev = &pdev->dev;
	platform_set_drvdata(pdev, card);

	card_data = devm_kzalloc(&pdev->dev,
		sizeof(struct octave_priv), GFP_KERNEL);
	if (!card_data) {
		ret = -ENOMEM;
		dev_err(&pdev->dev,
			"%s allocate card private data fail %d\n",
			__func__, ret);
		return ret;
	}

	snd_soc_card_set_drvdata(card, card_data);

	card_data->card = card;

	ret = octave_regulator_probe(card);
	if (ret) {
		dev_err(&pdev->dev, "%s octave_regulator_probe failed %d\n",
		__func__, ret);
		goto gpio_fail;
	}

	ret = octave_irq_probe(card);
	if (ret) {
		dev_err(&pdev->dev, "%s octave_irq_probe failed %d\n",
		__func__, ret);
		goto gpio_fail;
	}

	ret = octave_gpio_probe(card);
	if (ret) {
		dev_err(&pdev->dev, "%s octave_gpio_probe failed %d\n",
		__func__, ret);
		goto gpio_fail;
	}

	ret = devm_snd_soc_register_card(&pdev->dev, card);
	if (ret) {
		dev_err(&pdev->dev, "%s snd_soc_register_card fail %d\n",
		__func__, ret);
		octave_gpio_release(card);
	}

	octave_card_data = card_data;

gpio_fail:
	dev_info(card->dev, "%s- snd %d\n", __func__, ret);

	return ret;
}

static int octave_dev_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);

	octave_gpio_release(card);

	return 0;
}

static struct platform_driver octave_mach_driver = {
	.driver = {
		   .name = "mt8516-soc-octave-card",
		   .of_match_table = octave_dt_match,
#ifdef CONFIG_PM
		   .pm = &snd_soc_pm_ops,
#endif
	},
	.probe = octave_dev_probe,
	.remove = octave_dev_remove,
};

module_platform_driver(octave_mach_driver);

/* Module information */
MODULE_DESCRIPTION("MT8516 octave ALSA SoC machine driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:mt8516-soc-card");

