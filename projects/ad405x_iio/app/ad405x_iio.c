/***************************************************************************//**
 *   @file    ad405x_iio.c
 *   @brief   Implementation of AD405X IIO Application Interface
 *   @details This module acts as an interface for AD405X IIO device
********************************************************************************
 * Copyright (c) 2022-2025 Analog Devices, Inc.
 *
 * This software is proprietary to Analog Devices, Inc. and its licensors.
 * By using this software you agree to the terms of the associated
 * Analog Devices Software License Agreement.
*******************************************************************************/

/******************************************************************************/
/***************************** Include Files **********************************/
/******************************************************************************/
#include <string.h>
#include <math.h>

#include "app_config.h"
#include "ad405x.h"
#include "ad405x_iio.h"
#include "ad405x_user_config.h"
#include "common.h"
#include "no_os_delay.h"
#include "no_os_error.h"
#include "no_os_gpio.h"
#include "no_os_pwm.h"
#include "no_os_alloc.h"
#include "no_os_util.h"
#include "iio_trigger.h"

/******** Forward declaration of getter/setter functions ********/
static int iio_ad405x_attr_get(void *device,
			       char *buf,
			       uint32_t len,
			       const struct iio_ch_info *channel,
			       intptr_t priv);

static int iio_ad405x_attr_set(void *device,
			       char *buf,
			       uint32_t len,
			       const struct iio_ch_info *channel,
			       intptr_t priv);

static int iio_ad405x_attr_available_get(void *device,
		char *buf,
		uint32_t len,
		const struct iio_ch_info *channel,
		intptr_t priv);

static int iio_ad405x_attr_available_set(void *device,
		char *buf,
		uint32_t len,
		const struct iio_ch_info *channel,
		intptr_t priv);

/******************************************************************************/
/************************ Macros/Constants ************************************/
/******************************************************************************/
#define AD405X_CHN_ATTR(_name, _priv) {\
		.name = _name,\
		.priv = _priv,\
		.show = iio_ad405x_attr_get,\
		.store = iio_ad405x_attr_set\
}

#define AD405X_CHN_AVAIL_ATTR(_name, _priv) {\
	.name = _name,\
	.priv = _priv,\
	.show = iio_ad405x_attr_available_get,\
	.store = iio_ad405x_attr_available_set\
}

/* ADC data buffer size */
#if defined(USE_SDRAM)
#define adc_data_buffer				SDRAM_START_ADDRESS
#define DATA_BUFFER_SIZE			SDRAM_SIZE_BYTES
#else
#define DATA_BUFFER_SIZE			(131072)		// 128kbytes
static int8_t adc_data_buffer[DATA_BUFFER_SIZE];
#endif

/*	Number of IIO devices */
#define NUM_OF_IIO_DEVICES	         2

/* IIO trigger name */
#define AD405X_IIO_TRIGGER_NAME		ACTIVE_DEVICE_NAME"_iio_trigger"

/* Device names */
#define DEV_AD4050	"ad4050"
#define DEV_AD4052	"ad4052"

#define STORAGE_BITS_SAMPLE 16
#define AD4050_SAMPLE_RES   12
#define AD4052_SAMPLE_RES   16

#define STORAGE_BITS_AVG    32
#define AD4050_AVG_RES      14
#define AD4052_AVG_RES      20

/* Number of storage bytes for each sample */
#define BYTES_PER_SAMPLE(x)   (x/8)

/* Factor multiplied to calculated conversion time to ensure proper data capture */
#define COMPENSATION_FACTOR	1.1

/* Internal sampling frequency/period (2msps->500nsec) */
#define INTERNAL_SAMPLING_CLK_NS     500

/* Internal conversion time in nsec */
#define CONVERSION_TIME_NS           250

#define MAX_SAMPLING_TIME_NS          (((float)(1.0/SAMPLING_RATE) * 1000000000) / 2)

#define MAX_SAMPLING_PERIOD_NSEC		2500000

/* Converts pwm period in nanoseconds to sampling frequency in samples per second */
#define PWM_PERIOD_TO_FREQUENCY(x)       (1000000000.0 / x)

/* Timeout count to avoid stuck into potential infinite loop while checking
 * for new data whenever the BUSY pin goes low. The actual timeout factor is determined
 * through 'sampling_frequency' attribute of IIO app, but this period here makes sure
 * we are not stuck into a forever loop in case data capture is interrupted
 * or failed in between.
 * Note: This timeout factor is dependent upon the MCU clock frequency. Below timeout
 * is tested for SDP-K1 platform @180Mhz default core clock */
#define BUF_READ_TIMEOUT	0xffffffff

/* Number of samples after which the circular buffer indexes are
 * are updated for continuous mode with SPI DMA
 */
#define BUFFER_UPDATE_RATE  400

/* Maximum size of the local SRAM buffer */
#define MAX_LOCAL_BUF_SIZE	32000

/* Maximum value the DMA NDTR register can take */
#define MAX_DMA_NDTR		(no_os_min(65535, MAX_LOCAL_BUF_SIZE/2))

/******************************************************************************/
/*************************** Types Declarations *******************************/
/******************************************************************************/
/* Pointer to the struct representing the ad405x IIO device */
struct ad405x_dev *p_ad405x_dev = NULL;

/* IIO interface descriptor */
static struct iio_desc *p_ad405x_iio_desc;

/* ad405x IIO device descriptor */
struct iio_device *p_iio_ad405x_dev[NUM_OF_IIO_DEVICES];

/* ad405x IIO hw trigger descriptor */
static struct iio_hw_trig *ad405x_hw_trig_desc;

/* IIO interface init parameters */
static struct iio_init_param iio_init_params = {
	.phy_type = USE_UART,
};

/* Variable to store the sampling rate */
static uint32_t ad405x_sample_rate;

/* EVB HW validation status */
static bool hw_mezzanine_is_valid;

/* Selected operating mode. Default is sample mode */
enum ad405x_operation_mode ad405x_operating_mode = AD405X_ADC_MODE_OP;

/* Selected interface mode. Default is DMA mode */
enum ad405x_interface_modes ad405x_interface_mode = SPI_DMA;

/* Variable to store data ready status of ADC */
volatile bool data_ready = false;

/* Variable to store number of requested samples */
static uint32_t nb_of_samples;

/* Variable to store start of buffer address */
volatile uint8_t *buff_start_addr;

/* Local SRAM buffer */
uint8_t local_buf[MAX_LOCAL_BUF_SIZE];

/* Flag to indicate if size of the buffer is updated according to requested
 * number of samples for the multi-channel IIO buffer data alignment */
static volatile bool buf_size_updated = false;

/* Flag to indicate if DMA has been configured for windowed capture */
volatile bool dma_config_updated = false;

/* Variable to store the data sampling time period */
static volatile uint32_t pwm_period;

/* ad405x attribute unique IDs */
enum ad405x_attribute_ids {
	ADC_RAW,
	ADC_SCALE,
	ADC_OFFSET,
	ADC_OPERATING_MODE,
	ADC_SAMPLE_RATE,
	RESTART_IIO,
	ADC_BURST_SAMPLE_RATE,
	ADC_FILTER_LENGTH
};

/* IIOD channels configurations */
static struct scan_type ad405x_iio_scan_type = {
#if (ADC_DATA_FORMAT == STRAIGHT_BINARY)
	.sign = 'u',
#else
	.sign = 's',
#endif
	.shift = 0,
	.is_big_endian = false
};

/* Operating mode range values string representation */
static char *ad405x_op_mode_str[] = {
	"config_mode",
	"adc_mode",
	"averaging_mode",
	"burst_averaging_mode",
};

/* Averaging filter length values string representation */
static char *ad405x_avg_filter_str[] = {
	"2", "4", "8", "16", "32", "64", "128",
	"256", "512", "1024", "2048", "4096"
};

/* String representation of burst mode sample rates */
static char *ad405x_burst_sample_rates_str[] = {
	"2msps",
	"1msps",
	"300ksps",
	"100ksps",
	"33p3ksps",
	"10ksps",
	"3ksps",
	"1ksps",
	"500sps",
	"333sps",
	"250sps",
	"200sps",
	"166sps",
	"140sps",
	"125sps",
	"111sps"
};

/* Burst mode sample rates (in kHz) */
static float ad405x_burst_sample_rates[] = {
	2000,
	1000,
	300,
	100,
	33.3,
	10,
	3,
	1,
	0.5,
	0.333,
	0.25,
	0.2,
	0.166,
	0.14,
	0.125,
	0.111
};

/* ad405x channel specific attributes list */
static struct iio_attribute iio_ad405x_ch_attributes[] = {
	AD405X_CHN_ATTR("raw", ADC_RAW),
	AD405X_CHN_ATTR("scale", ADC_SCALE),
	AD405X_CHN_ATTR("offset", ADC_OFFSET),
	END_ATTRIBUTES_ARRAY,
};

/* ad405x device (global) specific system config attributes list */
static struct iio_attribute iio_ad405x_global_attributes_system_config[] = {
	AD405X_CHN_ATTR("operating_mode", ADC_OPERATING_MODE),
	AD405X_CHN_AVAIL_ATTR("operating_mode_available", ADC_OPERATING_MODE),
	AD405X_CHN_ATTR("reconfigure_system", RESTART_IIO),
	AD405X_CHN_ATTR("reconfigure_system_available", RESTART_IIO),
	END_ATTRIBUTES_ARRAY
};

/* ad405x device (global) specific sample mode attributes list */
static struct iio_attribute iio_ad405x_global_attributes_sample_mode[] = {
	AD405X_CHN_ATTR("sampling_frequency", ADC_SAMPLE_RATE),
	END_ATTRIBUTES_ARRAY
};

/* ad405x device (global) specific burst averaging mode attributes list */
static struct iio_attribute iio_ad405x_global_attributes_burst_averaging_mode[]
	= {
	AD405X_CHN_ATTR("burst_sample_rate", ADC_BURST_SAMPLE_RATE),
	AD405X_CHN_AVAIL_ATTR("burst_sample_rate_available", ADC_BURST_SAMPLE_RATE),
	AD405X_CHN_ATTR("avg_filter_length", ADC_FILTER_LENGTH),
	AD405X_CHN_AVAIL_ATTR("avg_filter_length_available", ADC_FILTER_LENGTH),
	AD405X_CHN_ATTR("sampling_frequency", ADC_SAMPLE_RATE),
	END_ATTRIBUTES_ARRAY
};

/* ad405x device (global) specific averaging mode attributes list */
static struct iio_attribute iio_ad405x_global_attributes_averaging_mode[] = {
	AD405X_CHN_ATTR("avg_filter_length", ADC_FILTER_LENGTH),
	AD405X_CHN_AVAIL_ATTR("avg_filter_length_available", ADC_FILTER_LENGTH),
	AD405X_CHN_ATTR("sampling_frequency", ADC_SAMPLE_RATE),
	END_ATTRIBUTES_ARRAY
};

/* IIO channels info */
static struct iio_channel iio_ad405x_channels[] = {
	{
		.name = "voltage0",
		.ch_type = IIO_VOLTAGE,
		.ch_out = false,
		.indexed = true,
		.channel = 0,
		.scan_index = 0,
		.scan_type = &ad405x_iio_scan_type,
		.attributes = iio_ad405x_ch_attributes
	}
};

/* ad405x IIOD debug attributes list */
static struct iio_attribute ad405x_debug_attributes[] = {
	END_ATTRIBUTES_ARRAY
};

/* Global Pointer for IIO Device Data */
volatile struct iio_device_data* iio_dev_data_g;

/* Global variable for number of samples */
uint32_t nb_of_bytes_g;

/* Global variable for data read from CB functions */
uint32_t data_read;

/* Variable to store ADC resolution based on device and mode */
static uint8_t resolution;

/* Variable to store storage bits based on device and mode */
static uint8_t storage_bits;

/* Variable to store bytes per sample based on device and mode */
static uint8_t bytes_per_sample;

/* Variable to store maximum count of the ADC based on device and mode*/
static uint32_t adc_max_count;

/* SPI Message */
struct no_os_spi_msg ad405x_spi_msg;

/* SPI init params */
struct stm32_spi_init_param* spi_init_param;

/* Restart IIO flag */
static bool restart_iio_flag = false;

/******************************************************************************/
/************************** Functions Declarations ****************************/
/******************************************************************************/

/******************************************************************************/
/************************ Functions Definitions *******************************/
/******************************************************************************/
/*!
 * @brief	Function to configure pwm period
 * @param	sampling_pwm_period[in] - Time period of sampling PWM.
 * @return	0 in case of success, negative error code otherwise
 */
static int configure_pwm_period(uint32_t sampling_pwm_period)
{
	int ret;

	if (ad405x_interface_mode == SPI_DMA) {
		spi_dma_pwm_init_params.period_ns = CONV_TRIGGER_PERIOD_NSEC(
				ad405x_sample_rate);

		cs_init_params.period_ns = CONV_TRIGGER_PERIOD_NSEC(ad405x_sample_rate);
		ret = init_pwm();
		if (ret) {
			return ret;
		}
	} else {
#if (ACTIVE_PLATFORM == MBED_PLATFORM)
		ret = no_os_pwm_enable(pwm_desc);
		if (ret) {
			return ret;
		}
#endif

		spi_intr_pwm_init_params.period_ns = CONV_TRIGGER_PERIOD_NSEC(
				ad405x_sample_rate);

		ret = no_os_pwm_set_period(pwm_desc,
					   sampling_pwm_period);
		if (ret) {
			return ret;
		}

		ret = no_os_pwm_set_duty_cycle(pwm_desc,
					       CONV_TRIGGER_DUTY_CYCLE_NSEC(sampling_pwm_period));
		if (ret) {
			return ret;
		}

#if (ACTIVE_PLATFORM == MBED_PLATFORM)
		ret = no_os_pwm_disable(pwm_desc);
		if (ret) {
			return ret;
		}
#endif
	}
	return 0;
}

/*!
 * @brief	Function to calculate max pwm period for a given custom attribute value
 * @param	attr_id[in]- Attribute private ID.
 * @param	attr_val[in]- Custom value of attr.
 * @param	configure_pwm[in]- Boolean check to set calculated period.
 * @return	0 in case of success, negative error code otherwise.
 */
static int calc_max_pwm_period(enum ad405x_attribute_ids attr_id,
			       uint8_t attr_val,
			       bool configure_pwm)
{
	/* Here calculate the min cnv_time */
	/* The conversion time is calculated using this formula:
		 * (M-1)*tOSC + tCONV + 24*tSCLK, where M stands for
		 * filter length, tOSC = internal sampling time, tCONV =
		 * ADC conversion time, tSCLK = SPI Clock */

	uint8_t avg_length;
	uint8_t fosc;
	uint64_t cnv_time;
	uint64_t temp_pwm_period;

	switch (attr_id) {
	case ADC_FILTER_LENGTH:
		avg_length = attr_val;
		fosc = p_ad405x_dev->rate;
		break;

	case ADC_BURST_SAMPLE_RATE:
		avg_length = p_ad405x_dev->filter_length;
		fosc = attr_val;
		break;

	default:
		avg_length = p_ad405x_dev->filter_length;
		fosc = p_ad405x_dev->rate;
		break;
	}

	cnv_time = (uint64_t)(((pow(2,
				    avg_length + 1) - 1) * (1000000 / ad405x_burst_sample_rates[fosc])
			       + CONVERSION_TIME_NS) * COMPENSATION_FACTOR);

	temp_pwm_period = no_os_max(cnv_time + MIN_DATA_CAPTURE_TIME_NS +
				    MIN_INTERRUPT_OVER_HEAD, CONV_TRIGGER_PERIOD_NSEC(SAMPLING_RATE_SPI_INTR));

	ad405x_sample_rate = PWM_PERIOD_TO_FREQUENCY(temp_pwm_period);

	if (configure_pwm) {
		return configure_pwm_period(temp_pwm_period);
	}

	return temp_pwm_period;
}

/*!
 * @brief	Function to determine closest supported attribute value
 * @param	attr_id[in]- Attribute private ID.
 * @param	attr_val[in]- Custom value of attr.
 * @return	0 in case of success, negative error code otherwise.
 */
static int calc_closest_burst_attr_val(enum ad405x_attribute_ids attr_id,
				       uint8_t *attr_val)
{
	uint64_t temp_pwm_period;
	int16_t closest_val = -1;
	uint8_t val;
	uint8_t lower_bound, upper_bound;

	switch (attr_id) {
	case ADC_FILTER_LENGTH:
		lower_bound = AD405X_LENGTH_2;
		if (p_ad405x_dev->active_device == ID_AD4050) {
			upper_bound = AD405X_LENGTH_256;
		} else {
			upper_bound = AD405X_LENGTH_4096;
		}
		break;

	case ADC_BURST_SAMPLE_RATE:
		lower_bound = AD405X_2_MSPS;
		upper_bound = AD405X_111_SPS;
		break;

	default:
		return -EINVAL;
	}

	/* Loop through the options - find a value for the attribute closest
	 * to the user-supplied value, for which the sampling rate is also
	 * supported */

	for (val = lower_bound; val <= upper_bound; val++) {
		temp_pwm_period = calc_max_pwm_period(attr_id, val, false);
		if (temp_pwm_period < MAX_SAMPLING_PERIOD_NSEC) {
			if (closest_val == -1
			    || (abs(*attr_val - closest_val) > abs(*attr_val - val)))
				closest_val = val;
		}
	}

	*attr_val = closest_val;

	return calc_max_pwm_period(attr_id, *attr_val, true);
}

/*!
 * @brief	Getter function for ad405x attributes
 * @param	device[in, out]- Pointer to IIO device instance.
 * @param	buf[in]- IIO input data buffer.
 * @param	len[in]- Number of expected bytes.
 * @param	channel[in] - input channel.
 * @param	priv[in] - Attribute private ID.
 * @return	len in case of success, negative error code otherwise
 */
static int iio_ad405x_attr_get(void *device,
			       char *buf,
			       uint32_t len,
			       const struct iio_ch_info *channel,
			       intptr_t priv)
{
	int ret;
	int32_t adc_raw_data;
	static int32_t offset = 0;
	float scale;
	uint8_t reg_data;
	uint32_t value;

	switch (priv) {
	case ADC_RAW:
		ret = no_os_gpio_remove(p_ad405x_dev->gpio_cnv);
		if (ret) {
			return ret;
		}

		ret = no_os_gpio_get(&p_ad405x_dev->gpio_cnv, ad405x_init_params.gpio_cnv);
		if (ret) {
			return ret;
		}

		ret = no_os_gpio_direction_output(p_ad405x_dev->gpio_cnv, NO_OS_GPIO_LOW);
		if (ret) {
			return ret;
		}

		ret = ad405x_set_operation_mode(p_ad405x_dev, ad405x_operating_mode);
		if (ret) {
			return ret;
		}

		ret = ad405x_read_val(p_ad405x_dev, &adc_raw_data);
		if (ret) {
			return ret;
		}

		ret = ad405x_exit_command(p_ad405x_dev);
		if (ret) {
			return ret;
		}

#if (ADC_DATA_FORMAT == TWOS_COMPLEMENT)
		if (adc_raw_data >= adc_max_count) {
			offset = -(NO_OS_BIT(resolution) - 1);
		} else {
			offset = 0;
		}
#endif
		return sprintf(buf, "%ld", adc_raw_data);

	case ADC_SCALE:
		scale = (((ADC_REF_VOLTAGE) / adc_max_count) * 1000);
		return sprintf(buf, "%g", scale);

	case ADC_OFFSET:
		return sprintf(buf, "%ld", offset);

	case ADC_OPERATING_MODE :
		return sprintf(buf, "%s", ad405x_op_mode_str[ad405x_operating_mode]);

	case ADC_BURST_SAMPLE_RATE:
		return sprintf(buf, "%s", ad405x_burst_sample_rates_str[p_ad405x_dev->rate]);

	case ADC_FILTER_LENGTH:
		ret = ad405x_read(p_ad405x_dev, AD405X_REG_AVG_CONFIG, &reg_data);
		if (ret) {
			return ret;
		}

		reg_data &= AD405X_AVG_WIN_LEN_MSK;
		if (reg_data != p_ad405x_dev->filter_length) {
			ret = ad405x_set_averaging_filter_length(p_ad405x_dev, reg_data);
			if (ret) {
				return ret;
			}
		}
		return sprintf(buf, "%s", ad405x_avg_filter_str[p_ad405x_dev->filter_length]);

	case ADC_SAMPLE_RATE:
		ret = no_os_pwm_get_period(pwm_desc, (uint32_t *)&pwm_period);
		if (ret) {
			return ret;
		}

		value = PWM_PERIOD_TO_FREQUENCY(pwm_period);

		if (ad405x_operating_mode == AD405X_AVERAGING_MODE_OP) {
			/* In Averaging Mode, the sampling rate is PWM frequency
			 * divided by the averaging length. */
			value /= (1 << (p_ad405x_dev->filter_length + 1));
		}

		return sprintf(buf, "%ld", value);

	case RESTART_IIO:
		return sprintf(buf, "%s", "enable");

	default:
		break;
	}

	return len;
}

/*!
 * @brief	Setter function for ad405x attributes.
 * @param	device[in, out]- Pointer to IIO device instance.
 * @param	buf[in]- IIO input data buffer.
 * @param	len[in]- Number of expected bytes.
 * @param	channel[in] - input channel.
 * @param	priv[in] - Attribute private ID.
 * @return	len in case of success, negative error code otherwise.
 */
static int iio_ad405x_attr_set(void *device,
			       char *buf,
			       uint32_t len,
			       const struct iio_ch_info *channel,
			       intptr_t priv)
{
	enum ad405x_operation_mode op_mode;
	enum ad405x_avg_filter_l filter_len;
	enum ad405x_sample_rate burst_rate;
	uint32_t requested_sampling_period;
	uint32_t requested_sampling_rate;
	int32_t ret;
	uint8_t value = 0;

	switch (priv) {
	case ADC_RAW:
	case ADC_SCALE:
	case ADC_OFFSET :
		/* These attributes are constant for the firmware
		* configuration and cannot be set during run time. */
		return len;

	case ADC_OPERATING_MODE :
		for (op_mode = AD405X_CONFIG_MODE_OP;
		     op_mode <= AD405X_BURST_AVERAGING_MODE_OP;
		     op_mode++) {
			if (!strncmp(buf, ad405x_op_mode_str[op_mode], strlen(buf))) {
				value = op_mode;
				break;
			}
		}

		ret = ad405x_set_operation_mode(p_ad405x_dev, value);
		if (ret) {
			return ret;
		}

		ad405x_operating_mode = value;

		/* Choose SPI DMA interface mode when in sample mode */
		if (ad405x_operating_mode == AD405X_ADC_MODE_OP) {
			ad405x_interface_mode = SPI_DMA;
		} else {
			ad405x_interface_mode = SPI_INTR;
		}

		return len;

	case ADC_BURST_SAMPLE_RATE:
		for (burst_rate = AD405X_2_MSPS; burst_rate <= AD405X_111_SPS; burst_rate++) {
			if (!strncmp(buf, ad405x_burst_sample_rates_str[burst_rate], strlen(buf))) {
				value = burst_rate;
				break;
			}
		}

		/* Find closest supported val */
		ret = calc_closest_burst_attr_val(ADC_BURST_SAMPLE_RATE,
						  &value);
		if (ret) {
			return -EINVAL;
		}

		ret = ad405x_set_sample_rate(p_ad405x_dev, value);
		if (ret) {
			return ret;
		}

		return len;

	case ADC_FILTER_LENGTH:
		for (filter_len = AD405X_LENGTH_2; filter_len <= AD405X_LENGTH_4096;
		     filter_len++) {
			if (!strncmp(buf, ad405x_avg_filter_str[filter_len], strlen(buf))) {
				value = filter_len;
				break;
			}
		}

		if (ad405x_operating_mode == AD405X_BURST_AVERAGING_MODE_OP) {
			/* Find closest supported val */
			ret = calc_closest_burst_attr_val(ADC_FILTER_LENGTH,
							  &value);
			if (ret) {
				return -EINVAL;
			}
		}

		ret = ad405x_set_averaging_filter_length(p_ad405x_dev, value);
		if (ret) {
			return ret;
		}

		return len;

	case ADC_SAMPLE_RATE:
		requested_sampling_rate = no_os_str_to_uint32(buf);
		if (requested_sampling_rate == 0) {
			return -EINVAL;
		}
		requested_sampling_period = CONV_TRIGGER_PERIOD_NSEC(requested_sampling_rate);

		if (ad405x_operating_mode == AD405X_ADC_MODE_OP) {
			ad405x_sample_rate = no_os_str_to_uint32(buf);
			if (((ad405x_interface_mode == SPI_DMA) &
			     (ad405x_sample_rate > SAMPLING_RATE_SPI_DMA)) ||
			    ((ad405x_interface_mode == SPI_INTR) &
			     (ad405x_sample_rate > SAMPLING_RATE_SPI_INTR)) ||
			    !ad405x_sample_rate) {
				return len;
			}
		} else if (ad405x_operating_mode == AD405X_BURST_AVERAGING_MODE_OP) {
			if (calc_max_pwm_period(ADC_SAMPLE_RATE,
						0,
						false) > requested_sampling_period) {
				return -EINVAL;
			}
			ad405x_sample_rate = no_os_str_to_uint32(buf);
		} else {
			ad405x_sample_rate = no_os_str_to_uint32(buf);
			if (ad405x_sample_rate > (SAMPLING_RATE_SPI_INTR / (1 <<
						  (p_ad405x_dev->filter_length + 1)))) {
				return len;
			}

			requested_sampling_period /= (1 << (p_ad405x_dev->filter_length + 1));
			ad405x_sample_rate = PWM_PERIOD_TO_FREQUENCY(requested_sampling_period);
		}

		ret = configure_pwm_period(requested_sampling_period);
		if (ret) {
			return ret;
		}

		return len;

	case RESTART_IIO:
		/* Set flag to true */
		restart_iio_flag = true;

	default :
		break;
	}

	return len;
}

/*!
 * @brief	Attribute available getter function for ad405x attributes.
 * @param	device[in, out]- Pointer to IIO device instance.
 * @param	buf[in]- IIO input data buffer.
 * @param	len[in]- Number of input bytes.
 * @param	channel[in] - input channel.
 * @param	priv[in] - Attribute private ID.
 * @return	len in case of success, negative error code otherwise.
 */
static int iio_ad405x_attr_available_get(void *device,
		char *buf,
		uint32_t len,
		const struct iio_ch_info *channel,
		intptr_t priv)
{
	switch (priv) {
	case ADC_OPERATING_MODE :
		return sprintf(buf,
			       "%s %s",
			       ad405x_op_mode_str[1],
			       ad405x_op_mode_str[3]);

	case ADC_BURST_SAMPLE_RATE:
		return sprintf(buf,
			       "%s %s %s %s %s %s %s %s %s %s %s %s %s %s %s %s",
			       ad405x_burst_sample_rates_str[0],
			       ad405x_burst_sample_rates_str[1],
			       ad405x_burst_sample_rates_str[2],
			       ad405x_burst_sample_rates_str[3],
			       ad405x_burst_sample_rates_str[4],
			       ad405x_burst_sample_rates_str[5],
			       ad405x_burst_sample_rates_str[6],
			       ad405x_burst_sample_rates_str[7],
			       ad405x_burst_sample_rates_str[8],
			       ad405x_burst_sample_rates_str[9],
			       ad405x_burst_sample_rates_str[10],
			       ad405x_burst_sample_rates_str[11],
			       ad405x_burst_sample_rates_str[12],
			       ad405x_burst_sample_rates_str[13],
			       ad405x_burst_sample_rates_str[14],
			       ad405x_burst_sample_rates_str[15]);

	case ADC_FILTER_LENGTH:
		if (p_ad405x_dev->active_device == ID_AD4050) {
			return sprintf(buf,
				       "%s %s %s %s %s %s %s %s",
				       ad405x_avg_filter_str[0],
				       ad405x_avg_filter_str[1],
				       ad405x_avg_filter_str[2],
				       ad405x_avg_filter_str[3],
				       ad405x_avg_filter_str[4],
				       ad405x_avg_filter_str[5],
				       ad405x_avg_filter_str[6],
				       ad405x_avg_filter_str[7]);
		} else {
			return sprintf(buf,
				       "%s %s %s %s %s %s %s %s %s %s %s %s",
				       ad405x_avg_filter_str[0],
				       ad405x_avg_filter_str[1],
				       ad405x_avg_filter_str[2],
				       ad405x_avg_filter_str[3],
				       ad405x_avg_filter_str[4],
				       ad405x_avg_filter_str[5],
				       ad405x_avg_filter_str[6],
				       ad405x_avg_filter_str[7],
				       ad405x_avg_filter_str[8],
				       ad405x_avg_filter_str[9],
				       ad405x_avg_filter_str[10],
				       ad405x_avg_filter_str[11]);
		}

	case RESTART_IIO:
		return sprintf(buf, "%s", "enable");

	default:
		break;
	}

	return len;
}

/*!
 * @brief	Attribute available setter function for ad405x attributes
 * @param	device[in, out]- Pointer to IIO device instance
 * @param	buf[in]- IIO input data buffer
 * @param	len[in]- Number of input bytes
 * @param	channel[in] - input channel
 * @param	priv[in] - Attribute private ID
 * @return	len in case of success, negative error code otherwise
 */
static int iio_ad405x_attr_available_set(void *device,
		char *buf,
		uint32_t len,
		const struct iio_ch_info *channel,
		intptr_t priv)
{
	return len;
}

/*!
 * @brief	Start a data capture in continuous/burst mode
 * @return	0 in case of success, negative error code otherwise
 */
static int32_t ad405x_start_data_capture(void)
{
	int ret;

	ret = ad405x_set_operation_mode(p_ad405x_dev, ad405x_operating_mode);
	if (ret) {
		return ret;
	}

	if (ad405x_interface_mode == SPI_INTR) {
		ret = init_pwm();
		if (ret) {
			return ret;
		}

		ret = no_os_pwm_enable(pwm_desc);
		if (ret) {
			return ret;
		}

#if (APP_CAPTURE_MODE == CONTINUOUS_DATA_CAPTURE)
		/* Clear any pending event that occurs from a unintended
		* falling edge of busy pin before enabling the interrupt */
		ret = no_os_irq_clear_pending(ad405x_hw_trig_desc->irq_ctrl,
					      ad405x_hw_trig_desc->irq_id);
		if (ret) {
			return ret;
		}

		ret = iio_trig_enable(ad405x_hw_trig_desc);
		if (ret) {
			return ret;
		}
#else
		/* Clear any pending event that occurs from a unintended
		* falling edge of busy pin before enabling the interrupt */
		ret = no_os_irq_clear_pending(trigger_irq_desc, TRIGGER_INT_ID);
		if (ret) {
			return ret;
		}

		ret = no_os_irq_enable(trigger_irq_desc, TRIGGER_INT_ID);
		if (ret) {
			return ret;
		}
#endif
	}

	return 0;
}

/*!
 * @brief	Stop a data capture from continuous/burst mode
 * @return	0 in case of success, negative error code otherwise
 */
int32_t ad405x_stop_data_capture(void)
{
	int ret;

	if (ad405x_interface_mode == SPI_INTR) {
		ret = no_os_pwm_disable(pwm_desc);
		if (ret) {
			return ret;
		}

#if (APP_CAPTURE_MODE == CONTINUOUS_DATA_CAPTURE)
		ret = iio_trig_disable(ad405x_hw_trig_desc);
		if (ret) {
			return ret;
		}
#else
		ret = no_os_irq_disable(trigger_irq_desc, TRIGGER_INT_ID);
		if (ret) {
			return ret;
		}
#endif
	}

	if (ad405x_interface_mode == SPI_DMA) {
		/* Abort DMA and Timers and configure CS as GPIO */
		stm32_timer_stop();
		stm32_abort_dma_transfer();
		stm32_cs_output_gpio_config(true);

		spi_init_param = ad405x_init_params.spi_init->extra;
		spi_init_param->dma_init = NULL;
		ad405x_init_params.spi_init->max_speed_hz = MAX_SPI_SCLK;
		ret = no_os_spi_init(&p_ad405x_dev->spi_desc, ad405x_init_params.spi_init);
		if (ret) {
			return ret;
		}

		dma_config_updated = false;
	}

	buf_size_updated = false;

	ret = ad405x_exit_command(p_ad405x_dev);
	if (ret) {
		return ret;
	}

	return 0;
}

/**
 * @brief  Prepares the device for data transfer.
 * @param  dev[in, out]- Application descriptor.
 * @param  mask[in]- Number of bytes to transfer.
 * @return 0 in case of success, error code otherwise.
 */
static int32_t iio_ad405x_prepare_transfer(void *dev, uint32_t mask)
{
	int ret;

	if (APP_CAPTURE_MODE == CONTINUOUS_DATA_CAPTURE
	    && ad405x_interface_mode == SPI_INTR) {
		return ad405x_start_data_capture();
	}

	if (ad405x_interface_mode == SPI_DMA) {
		ret = ad405x_set_adc_mode(p_ad405x_dev);
		if (ret) {
			return ret;
		}

		/* Switch to faster SPI SCLK and
		 * initialize Chip Select PWMs and DMA descriptors */
		ad405x_init_params.spi_init->max_speed_hz = MAX_SPI_SCLK_45MHz;
		spi_init_param = ad405x_init_params.spi_init->extra;
		spi_init_param->pwm_init = (const struct no_os_pwm_init_param *)&cs_init_params;
		spi_init_param->dma_init = &ad405x_dma_init_param;
		spi_init_param->irq_num = Rx_DMA_IRQ_ID;
		spi_init_param->rxdma_ch = &rxdma_channel;
		spi_init_param->txdma_ch = &txdma_channel;

		ret = no_os_spi_init(&p_ad405x_dev->spi_desc, ad405x_init_params.spi_init);
		if (ret) {
			return ret;
		}

		/* Use 16-bit SPI Data Frame Format during data capture */
		stm32_config_spi_data_frame_format(true);

		/* Configure CS gpio for alternate functionality as
		 * Timer PWM outputs */
		stm32_cs_output_gpio_config(false);

		ret = init_pwm();
		if (ret) {
			return ret;
		}
	}

	return 0;
}

/**
 * @brief  Terminate current data transfer.
 * @param  dev[in, out]- Application descriptor.
 * @return 0 in case of success, negative error code otherwise.
 */
static int32_t iio_ad405x_end_transfer(void *dev)
{
	if (ad405x_interface_mode == SPI_DMA) {
		/* Revert to 8-bit SPI Data Frame Format after data capture */
		stm32_config_spi_data_frame_format(false);

		return ad405x_stop_data_capture();
	} else {
#if (APP_CAPTURE_MODE == CONTINUOUS_DATA_CAPTURE)
		return ad405x_stop_data_capture();
#endif
	}

	return 0;
}

/**
 * @brief Writes all the samples from the ADC buffer into the
		  IIO buffer.
 * @param iio_dev_data[in] - IIO device data instance.
 * @return Number of samples read.
 */
static int32_t iio_ad405x_submit_samples(struct iio_device_data *iio_dev_data)
{
	uint32_t timeout = BUF_READ_TIMEOUT;
	uint32_t sample_index = 0;
	int32_t adc_data;
	int32_t ret;
	uint16_t spirxdma_ndtr;

	data_ready = false;
	nb_of_samples = iio_dev_data->buffer->size / bytes_per_sample;
	nb_of_bytes_g = nb_of_samples * bytes_per_sample;
	iio_dev_data_g = iio_dev_data;

	if (!buf_size_updated) {
		/* Update total buffer size according to bytes per scan for proper
		 * alignment of multi-channel IIO buffer data */
		iio_dev_data->buffer->buf->size = iio_dev_data->buffer->size;
		buf_size_updated = true;
	}

	if (ad405x_interface_mode == SPI_INTR) {
		ret = ad405x_start_data_capture();
		if (ret) {
			return ret;
		}

		while (sample_index < nb_of_samples) {
			while (data_ready != true && timeout > 0) {
				timeout--;
			}

			if (!timeout) {
				return -EIO;
			}

			ret = ad405x_spi_data_read(p_ad405x_dev, &adc_data);
			if (ret) {
				return ret;
			}

			ret = no_os_cb_write(iio_dev_data->buffer->buf, &adc_data, bytes_per_sample);
			if (ret) {
				return ret;
			}

			sample_index++;
			data_ready = false;
		}

		ret = ad405x_stop_data_capture();
		if (ret) {
			return ret;
		}
	} else {
#if (APP_CAPTURE_MODE == WINDOWED_DATA_CAPTURE)
		ret = no_os_cb_prepare_async_write(iio_dev_data->buffer->buf,
						   nb_of_samples * (bytes_per_sample),
						   (void **) &buff_start_addr,
						   &data_read);
		if (ret) {
			return ret;
		}

		if (!dma_config_updated) {
			/* Cap SPI RX DMA NDTR to MAX_DMA_NDTR. */
			spirxdma_ndtr = no_os_min(MAX_DMA_NDTR, nb_of_samples);
			rxdma_ndtr = spirxdma_ndtr;

			/* Register half complete callback, for ping-pong buffers implementation. */
			HAL_DMA_RegisterCallback(&hdma_spi1_rx,
						 HAL_DMA_XFER_HALFCPLT_CB_ID,
						 halfcmplt_callback);

			struct no_os_spi_msg ad405x_spi_msg = {
				.tx_buff = NULL,
				.rx_buff = local_buf,
				.bytes_number = spirxdma_ndtr
			};

			struct stm32_spi_desc* sdesc = p_ad405x_dev->spi_desc->extra;
			ret = no_os_spi_transfer_dma_async(p_ad405x_dev->spi_desc,
							   &ad405x_spi_msg,
							   1,
							   NULL,
							   NULL);
			if (ret) {
				return ret;
			}

			/* Disable CS PWM and reset the counters */
			no_os_pwm_disable(sdesc->pwm_desc); // CS PWM
			htim2.Instance->CNT = 0;
			htim1.Instance->CNT = 0;
			TIM8->CNT = 0;

			dma_config_updated = true;
		}

		ad405x_conversion_flag = false;

		dma_cycle_count = ((nb_of_samples) / rxdma_ndtr) + 1;

		/* Set the callback count to twice the number of DMA cycles */
		callback_count = dma_cycle_count * 2;

		update_buff(local_buf, (uint8_t *)buff_start_addr);
		stm32_timer_enable();

		while (ad405x_conversion_flag != true && timeout > 0) {
			timeout--;
		}

		if (!timeout) {
			return -EIO;
		}

		no_os_cb_end_async_write(iio_dev_data->buffer->buf);
#else
		if (!dma_config_updated) {
			ret = no_os_cb_prepare_async_write(iio_dev_data->buffer->buf,
							   nb_of_samples * (bytes_per_sample),
							   (void **) &buff_start_addr,
							   &data_read);
			if (ret) {
				return ret;
			}

			struct no_os_spi_msg ad405x_spi_msg = {
				.tx_buff = NULL,
				.rx_buff = buff_start_addr,
				.bytes_number = spirxdma_ndtr
			};

			ret = no_os_spi_transfer_dma_async(p_ad405x_dev->spi_desc,
							   &ad405x_spi_msg,
							   1,
							   NULL,
							   NULL);
			if (ret) {
				return ret;
			}
			struct stm32_spi_desc* sdesc = p_ad405x_dev->spi_desc->extra;
			no_os_pwm_disable(sdesc->pwm_desc); // CS PWM
			htim2.Instance->CNT = 0;
			htim1.Instance->CNT = 0;
			TIM8->CNT = 0;
			dma_config_updated = true;

			stm32_timer_enable();
		}

#endif
	}
	return 0;
}

/**
 * @brief	Reads data from the ADC and pushes it into IIO buffer when the
			IRQ is triggered.
 * @param	iio_dev_data[in] - IIO device data instance.
 * @return	0 in case of success or negative value otherwise.
 */
static int32_t ad405x_trigger_handler(struct iio_device_data *iio_dev_data)
{
	int32_t ret;
	int32_t adc_data;

	if (!buf_size_updated) {
		/* Update total buffer size according to bytes per scan for proper
		 * alignment of multi-channel IIO buffer data */
		iio_dev_data->buffer->buf->size = ((uint32_t)(DATA_BUFFER_SIZE /
						   iio_dev_data->buffer->bytes_per_scan)) * iio_dev_data->buffer->bytes_per_scan;
		buf_size_updated = true;
	}

	/* Read the sample for channel which has been sampled recently */
	ret = ad405x_spi_data_read(p_ad405x_dev, &adc_data);
	if (ret) {
		return ret;
	}

	return no_os_cb_write(iio_dev_data->buffer->buf, &adc_data, bytes_per_sample);
}

/*!
 * @brief Interrupt Service Routine to monitor data ready event.
 * @param context[in] - Callback context (unused)
 * @return none
 */
void data_capture_callback(void *context)
{
	data_ready = true;
}

/*!
 * @brief	Read the debug register value
 * @param	dev[in, out]- Pointer to IIO device instance
 * @param	reg[in]- Register address to read from
 * @param	readval[out]- Pointer to variable to read data into
 * @return	0 in case of success, negative value otherwise
 */
static int32_t iio_ad405x_debug_reg_read(void *dev,
		uint32_t reg,
		uint32_t *readval)
{
	int32_t ret;

	if (!dev || !readval) {
		return -EINVAL;
	}

	ret = ad405x_read(p_ad405x_dev, (uint8_t)reg, (uint8_t *)readval);
	if (NO_OS_IS_ERR_VALUE(ret)) {
		return ret;
	}

	return 0;
}

/*
 * @brief	Write the debug register value
 * @param	dev[in, out]- Pointer to IIO device instance
 * @param	reg[in]- Register address to write data to
 * @param	writeval[out]- Pointer to variable to write data from
 * @return	0 in case of success, negative value otherwise
 */
static int32_t iio_ad405x_debug_reg_write(void *dev,
		uint32_t reg,
		uint32_t writeval)
{
	int32_t ret;

	if (!dev) {
		return -EINVAL;
	}

	ret = ad405x_write(p_ad405x_dev, reg, writeval);
	if (NO_OS_IS_ERR_VALUE(ret)) {
		return ret;
	}

	return 0;
}

/**
 * @brief Assign device name and resolution
 * @param dev_type[in] - The device type
 * @param dev_name[out] - The device name
 * @return 0 in case of success, negative error code otherwise.
 */
static int32_t ad405x_assign_device(enum ad405x_device_type dev_type,
				    char** dev_name)
{
	switch (dev_type) {
	case ID_AD4050:
		ad405x_init_params.active_device = ID_AD4050;
		*dev_name = DEV_AD4050;
		if (ad405x_operating_mode == AD405X_ADC_MODE_OP) {
			resolution = AD4050_SAMPLE_RES;
			storage_bits = STORAGE_BITS_SAMPLE;
		} else {
			resolution = AD4050_AVG_RES;
			storage_bits = STORAGE_BITS_AVG;
		}
		break;

	case ID_AD4052:
		ad405x_init_params.active_device = ID_AD4052;
		*dev_name = DEV_AD4052;
		if (ad405x_operating_mode == AD405X_ADC_MODE_OP) {
			resolution = AD4052_SAMPLE_RES;
			storage_bits = STORAGE_BITS_SAMPLE;
		} else {
			resolution = AD4052_AVG_RES;
			storage_bits = STORAGE_BITS_AVG;
		}
		break;

	default:
		return -EINVAL;
	}

	bytes_per_sample = BYTES_PER_SAMPLE(storage_bits);

#if (ADC_DATA_FORMAT == STRAIGHT_BINARY)
	adc_max_count = (uint32_t)(1 << (resolution));
#else
	adc_max_count = (uint32_t)(1 << (resolution - 1));
#endif

	return 0;
}

/**
* @brief	Init for reading/writing and parameterization of a
* 			ad405x IIO device
* @param 	desc[in,out] - IIO device descriptor
* @return	0 in case of success, negative error code otherwise
*/
static int32_t iio_ad405x_init(struct iio_device **desc)
{
	struct iio_device *iio_ad405x_inst;

	iio_ad405x_inst = calloc(1, sizeof(struct iio_device));
	if (!iio_ad405x_inst) {
		return -EINVAL;
	}

	/* Resolution is assigned to the IIO channel */
	iio_ad405x_channels[0].scan_type->realbits = resolution;

	/* Storage bits is assigned to the IIO channel */
	iio_ad405x_channels[0].scan_type->storagebits = storage_bits;

	iio_ad405x_inst->num_ch = NO_OS_ARRAY_SIZE(iio_ad405x_channels);
	iio_ad405x_inst->channels = iio_ad405x_channels;
	iio_ad405x_inst->debug_attributes = ad405x_debug_attributes;
	if (ad405x_operating_mode == AD405X_ADC_MODE_OP) {
		iio_ad405x_inst->attributes = iio_ad405x_global_attributes_sample_mode;
	} else if (ad405x_operating_mode == AD405X_BURST_AVERAGING_MODE_OP) {
		iio_ad405x_inst->attributes = iio_ad405x_global_attributes_burst_averaging_mode;
	} else {
		iio_ad405x_inst->attributes = iio_ad405x_global_attributes_averaging_mode;
	}

	iio_ad405x_inst->submit = iio_ad405x_submit_samples;
	iio_ad405x_inst->pre_enable = iio_ad405x_prepare_transfer;
	iio_ad405x_inst->post_disable = iio_ad405x_end_transfer;
	iio_ad405x_inst->read_dev = NULL;
	iio_ad405x_inst->write_dev = NULL;
	iio_ad405x_inst->debug_reg_read = iio_ad405x_debug_reg_read;
	iio_ad405x_inst->debug_reg_write = iio_ad405x_debug_reg_write;
	iio_ad405x_inst->trigger_handler = ad405x_trigger_handler;

	*desc = iio_ad405x_inst;

	return 0;
}

/**
 * @brief	Initialization of AD405X IIO hardware trigger specific parameters
 * @param 	desc[in,out] - IIO hardware trigger descriptor
 * @return	0 in case of success, negative error code otherwise
 */
static int32_t ad405x_iio_trigger_param_init(struct iio_hw_trig **desc)
{
	int32_t ret;
	struct iio_hw_trig_init_param ad405x_hw_trig_init_params;
	struct iio_hw_trig *hw_trig_desc;

	hw_trig_desc = calloc(1, sizeof(struct iio_hw_trig));
	if (!hw_trig_desc) {
		return -ENOMEM;
	}

	ad405x_hw_trig_init_params.irq_id = TRIGGER_INT_ID;
	ad405x_hw_trig_init_params.name = AD405X_IIO_TRIGGER_NAME;
	ad405x_hw_trig_init_params.irq_trig_lvl = NO_OS_IRQ_EDGE_FALLING;
	ad405x_hw_trig_init_params.irq_ctrl = trigger_irq_desc;
	ad405x_hw_trig_init_params.iio_desc = p_ad405x_iio_desc;
	ad405x_hw_trig_init_params.cb_info.event = NO_OS_EVT_GPIO;
	ad405x_hw_trig_init_params.cb_info.peripheral = NO_OS_GPIO_IRQ;
	ad405x_hw_trig_init_params.cb_info.handle = trigger_gpio_handle;

	/* Initialize hardware trigger */
	ret = iio_hw_trig_init(&hw_trig_desc, &ad405x_hw_trig_init_params);
	if (ret) {
		no_os_free(hw_trig_desc);
		return ret;
	}

	*desc = hw_trig_desc;

	return 0;
}

/**
 * @brief   Initialize the ad405x Board Init Params
 * @param   desc[in,out] - IIO Device Descriptor
 * @return  0 in case of success, negative value otherwise
 */
static int board_iio_params_init(struct iio_device** desc)
{
	struct iio_device* iio_dev;

	if (!desc) {
		return -EINVAL;
	}

	iio_dev = calloc(1, sizeof(*iio_dev));
	if (!iio_dev) {
		return -ENOMEM;
	}

	iio_dev->num_ch = 0;
	iio_dev->attributes = iio_ad405x_global_attributes_system_config;

	*desc = iio_dev;

	return 0;
}

/**
 * @brief	DeInitialize the IIO params.
 * @return	0 in case of success, negative error code otherwise
 */
int iio_params_deinit(void)
{
	uint8_t indx = 0;

	for (indx = 0 ; indx < iio_init_params.nb_devs; indx++) {
		if (p_iio_ad405x_dev[indx] != NULL) {
			no_os_free(p_iio_ad405x_dev[indx]);
			p_iio_ad405x_dev[indx] = NULL;
		}
	}

	iio_init_params.nb_devs = 0;

	return 0;
}

/**
 * @brief	Initialize the IIO interface for AD405X IIO device
 * @return	0 in case of success,negative error code otherwise
 */
int32_t iio_ad405x_initialize(void)
{
	int32_t init_status;
	enum ad405x_device_type dev_type;
	uint8_t indx;
	static bool entered =
		false;  // Flag to control the execution of system initialization

#if	(APP_CAPTURE_MODE == CONTINUOUS_DATA_CAPTURE)
	static struct iio_trigger ad405x_iio_trig_desc = {
		.is_synchronous = true,
		.enable = NULL,
		.disable = NULL
	};

	static struct iio_trigger_init iio_trigger_init_params = {
		.descriptor = &ad405x_iio_trig_desc,
		.name = AD405X_IIO_TRIGGER_NAME,
	};
#endif

#if (APP_CAPTURE_MODE == CONTINUOUS_DATA_CAPTURE)
	if (ad405x_interface_mode == SPI_INTR) {
		iio_init_params.trigs = &iio_trigger_init_params;
	}
#endif

	/* IIOD init parameters */
	static struct iio_device_init iio_device_init_params[NUM_OF_IIO_DEVICES];

	if (!entered) {
		entered = true;

		/* Add a fixed delay of 1 sec before system init for the PoR sequence to get completed */
		no_os_udelay(1000000);

		init_status = init_system();
		if (init_status) {
			return init_status;
		}

		/* Add delay between the i2c init and the eeprom read */
		no_os_udelay(1000000);
	}

	/* Read context attributes */
	static const char *mezzanine_names[] = {
		"EVAL-AD4050-ARDZ",
		"EVAL-AD4052-ARDZ"
	};

	/* Iterate twice to detect the correct attached board */
	for (indx = 0; indx < NO_OS_ARRAY_SIZE(mezzanine_names); indx++) {
		init_status = get_iio_context_attributes(&iio_init_params.ctx_attrs,
				&iio_init_params.nb_ctx_attr,
				eeprom_desc,
				mezzanine_names[indx],
				STR(HW_CARRIER_NAME),
				&hw_mezzanine_is_valid);
		if (init_status) {
			return init_status;
		}

		if (hw_mezzanine_is_valid) {
			dev_type = indx;
			break;
		}

		if (indx != NO_OS_ARRAY_SIZE(mezzanine_names) - 1) {
			init_status = remove_iio_context_attributes(iio_init_params.ctx_attrs);
			if (init_status) {
				return init_status;
			}
		}
	}

	/* Initialize board IIO paramaters */
	init_status = board_iio_params_init(&p_iio_ad405x_dev[iio_init_params.nb_devs]);
	if (init_status) {
		return init_status;
	}

	iio_device_init_params[iio_init_params.nb_devs].name = "system_config";
	iio_device_init_params[iio_init_params.nb_devs].dev_descriptor =
		p_iio_ad405x_dev[iio_init_params.nb_devs];
	iio_init_params.nb_devs++;

	if (hw_mezzanine_is_valid) {
		/* Initialize AD405X device and peripheral interface */
		init_status = ad405x_assign_device(dev_type,
						   &iio_device_init_params[iio_init_params.nb_devs].name);
		if (init_status) {
			return init_status;
		}

		init_status = ad405x_init(&p_ad405x_dev, ad405x_init_params);
		if (init_status) {
			return init_status;
		}

		init_status = ad405x_set_gp_mode(p_ad405x_dev,
						 AD405X_GP_1,
						 AD405X_GP_MODE_DRDY);
		if (init_status) {
			return init_status;
		}
#if (ADC_DATA_FORMAT == STRAIGHT_BINARY)
		init_status = ad405x_set_data_format(p_ad405x_dev, AD405X_STRAIGHT_BINARY);
#else
		init_status = ad405x_set_data_format(p_ad405x_dev, AD405X_TWOS_COMPLEMENT);
#endif
		if (init_status) {
			return init_status;
		}

		init_status = iio_ad405x_init(&p_iio_ad405x_dev[iio_init_params.nb_devs]);
		if (init_status) {
			return init_status;
		}

		/* Initialize the IIO interface */
		iio_device_init_params[iio_init_params.nb_devs].raw_buf = adc_data_buffer;
		iio_device_init_params[iio_init_params.nb_devs].raw_buf_len = DATA_BUFFER_SIZE;

		iio_device_init_params[iio_init_params.nb_devs].dev = p_ad405x_dev;
		iio_device_init_params[iio_init_params.nb_devs].dev_descriptor =
			p_iio_ad405x_dev[iio_init_params.nb_devs];

		if (APP_CAPTURE_MODE == CONTINUOUS_DATA_CAPTURE
		    && ad405x_interface_mode == SPI_INTR) {
			iio_device_init_params[iio_init_params.nb_devs].trigger_id = "trigger0";
			iio_init_params.nb_trigs++;
		}

		iio_init_params.nb_devs++;
	}

	/* Initialize the IIO interface */
	iio_init_params.uart_desc = uart_iio_com_desc;
	iio_init_params.devs = iio_device_init_params;
	init_status = iio_init(&p_ad405x_iio_desc, &iio_init_params);
	if (init_status) {
		return init_status;
	}

	if (APP_CAPTURE_MODE == CONTINUOUS_DATA_CAPTURE
	    && ad405x_interface_mode == SPI_INTR) {
		init_status = ad405x_iio_trigger_param_init(&ad405x_hw_trig_desc);
		if (init_status) {
			return init_status;
		}
	}

	if (ad405x_interface_mode == SPI_DMA) {
		ad405x_sample_rate = SAMPLING_RATE_SPI_DMA;
	} else {
		ad405x_sample_rate = SAMPLING_RATE_SPI_INTR;
	}

	init_status = init_pwm();
	if (init_status) {
		return init_status;
	}

	return 0;
}

/**
 * @brief 	Run the AD405X IIO event handler
 * @return	none
 * @details	This function monitors the new IIO client event
 */
void iio_ad405x_event_handler(void)
{
	if (restart_iio_flag) {
		/* Remove and free the pointers allocated during IIO init */
#if (APP_CAPTURE_MODE == CONTINUOUS_DATA_CAPTURE)
		iio_hw_trig_remove(ad405x_hw_trig_desc);
#endif

		ad405x_remove(p_ad405x_dev);

		iio_params_deinit();

		remove_iio_context_attributes(iio_init_params.ctx_attrs);

		iio_remove(p_ad405x_iio_desc);

		/* Reset the restart_iio flag */
		restart_iio_flag = false;

		iio_ad405x_initialize();
	}

	iio_step(p_ad405x_iio_desc);
}
