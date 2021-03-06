/***************************************************************************//**
 *   @file   dac_core.c
 *   @brief  Implementation of DAC Core Driver.
 *   @author DBogdan (dragos.bogdan@analog.com)
********************************************************************************
 * Copyright 2013(c) Analog Devices, Inc.
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *  - Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  - Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *  - Neither the name of Analog Devices, Inc. nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *  - The use of this software may or may not infringe the patent rights
 *    of one or more patent holders.  This license does not release you
 *    from the requirement that you obtain separate licenses from these
 *    patent holders to use this software.
 *  - Use of the software either in source or binary form, must be run
 *    on or directly connected to an Analog Devices Inc. component.
 *
 * THIS SOFTWARE IS PROVIDED BY ANALOG DEVICES "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, NON-INFRINGEMENT,
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL ANALOG DEVICES BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, INTELLECTUAL PROPERTY RIGHTS, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*******************************************************************************/

/******************************************************************************/
/***************************** Include Files **********************************/
/******************************************************************************/
#include <stdint.h>
#include "adc_core.h"
#include "dac_core.h"
#include "parameters.h"
#include "../util.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

/******************************************************************************/
/************************ Variables Definitions *******************************/
/******************************************************************************/
struct dds_state dds_st[2];
#ifdef DMA_UIO
int tx_dma_uio_fd;
void *tx_dma_uio_addr;
uint32_t tx_buff_mem_size;
uint32_t tx_buff_mem_addr;
#endif

/******************************************************************************/
/********************** Macros and Constants Definitions **********************/
/******************************************************************************/
const uint16_t sine_lut[32] = {
		0x000, 0x031, 0x061, 0x08D, 0x0B4, 0x0D4, 0x0EC, 0x0FA,
		0x0FF, 0x0FA, 0x0EC, 0x0D4, 0x0B4, 0x08D, 0x061, 0x031,
		0x000, 0xFCE, 0xF9E, 0xF72, 0xF4B, 0xF2B, 0xF13, 0xF05,
		0xF00, 0xF05, 0xF13, 0xF2B, 0xF4B, 0xF72, 0xF9E, 0xFCE
};

/***************************************************************************//**
 * @brief dac_read
*******************************************************************************/
void dac_read(struct ad9361_rf_phy *phy, uint32_t regAddr, uint32_t *data)
{
	adc_read(phy, regAddr + 0x4000, data);
}

/***************************************************************************//**
 * @brief dac_write
*******************************************************************************/
void dac_write(struct ad9361_rf_phy *phy, uint32_t regAddr, uint32_t data)
{
	adc_write(phy, regAddr + 0x4000, data);
}

/***************************************************************************//**
 * @brief dac_dma_read
*******************************************************************************/
void dac_dma_read(uint32_t regAddr, uint32_t *data)
{
#ifdef DMA_UIO
	*data = (*((unsigned *) (tx_dma_uio_addr + regAddr)));
#else
	*data = 0;
#endif
}

/***************************************************************************//**
 * @brief dac_dma_write
*******************************************************************************/
void dac_dma_write(uint32_t regAddr, uint32_t data)
{
#ifdef DMA_UIO
	*((unsigned *) (tx_dma_uio_addr + regAddr)) = data;
#endif
}

/***************************************************************************//**
 * @brief dds_default_setup
*******************************************************************************/
static int dds_default_setup(struct ad9361_rf_phy *phy,
							 uint32_t chan, uint32_t phase,
							 uint32_t freq, int32_t scale)
{
	dds_set_phase(phy, chan, phase);
	dds_set_frequency(phy, chan, freq);
	dds_set_scale(phy, chan, scale);
	dds_st[phy->id_no].cached_freq[chan] = freq;
	dds_st[phy->id_no].cached_phase[chan] = phase;
	dds_st[phy->id_no].cached_scale[chan] = scale;

	return 0;
}

/***************************************************************************//**
 * @brief dac_stop
*******************************************************************************/
void dac_stop(struct ad9361_rf_phy *phy)
{
	if (PCORE_VERSION_MAJOR(dds_st[phy->id_no].pcore_version) < 8)
	{
		dac_write(phy, DAC_REG_CNTRL_1, 0);
	}
}

/***************************************************************************//**
 * @brief dac_start_sync
*******************************************************************************/
void dac_start_sync(struct ad9361_rf_phy *phy, bool force_on)
{
	if (PCORE_VERSION_MAJOR(dds_st[phy->id_no].pcore_version) < 8)
	{
		dac_write(phy, DAC_REG_CNTRL_1, (dds_st[phy->id_no].enable || force_on) ? DAC_ENABLE : 0);
	}
	else
	{
		dac_write(phy, DAC_REG_CNTRL_1, DAC_SYNC);
	}
}

/***************************************************************************//**
 * @brief dac_init
*******************************************************************************/
void dac_init(struct ad9361_rf_phy *phy, uint8_t data_sel)
{
#ifdef DMA_UIO
	uint32_t tx_count;
	uint32_t index, index_i1, index_q1, index_i2, index_q2;
	uint32_t data_i1, data_q1, data_i2, data_q2;
	uint32_t length;
	int dev_mem_fd;
	uint32_t mapping_length, page_mask, page_size;
	void *mapping_addr, *tx_buff_virt_addr;

	tx_dma_uio_fd = open(TX_DMA_UIO_DEV, O_RDWR);
	if(tx_dma_uio_fd < 1)
	{
		printf("%s: Can't open tx_dma_uio device\n\r", __func__);
		return;
	}
	
	tx_dma_uio_addr = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, tx_dma_uio_fd, 0);
#endif
	dac_write(phy, DAC_REG_RSTN, 0x0);
	dac_write(phy, DAC_REG_RSTN, DAC_RSTN);

	dac_write(phy, DAC_REG_RATECNTRL, DAC_RATE(3));

	dds_st[phy->id_no].dac_clk = &phy->clks[TX_SAMPL_CLK]->rate;
	dds_st[phy->id_no].rx2tx2 = phy->pdata->rx2tx2;
	if(dds_st[phy->id_no].rx2tx2)
	{
		dds_st[phy->id_no].num_dds_channels = 8;
	}
	else
	{
		dds_st[phy->id_no].num_dds_channels = 4;
	}

	dac_read(phy, DAC_REG_VERSION, &dds_st[phy->id_no].pcore_version);

	dac_write(phy, DAC_REG_CNTRL_1, 0);
	switch (data_sel) {
	case DATA_SEL_DDS:
		dds_default_setup(phy, DDS_CHAN_TX1_I_F1, 90000, 1000000, 250000);
		dds_default_setup(phy, DDS_CHAN_TX1_I_F2, 90000, 1000000, 250000);
		dds_default_setup(phy, DDS_CHAN_TX1_Q_F1, 0, 1000000, 250000);
		dds_default_setup(phy, DDS_CHAN_TX1_Q_F2, 0, 1000000, 250000);
		if(dds_st[phy->id_no].rx2tx2)
		{
			dds_default_setup(phy, DDS_CHAN_TX2_I_F1, 90000, 1000000, 250000);
			dds_default_setup(phy, DDS_CHAN_TX2_I_F2, 90000, 1000000, 250000);
			dds_default_setup(phy, DDS_CHAN_TX2_Q_F1, 0, 1000000, 250000);
			dds_default_setup(phy, DDS_CHAN_TX2_Q_F2, 0, 1000000, 250000);
		}
		dac_write(phy, DAC_REG_CNTRL_2, 0);
		dac_datasel(phy, -1, DATA_SEL_DDS);
		break;
	case DATA_SEL_DMA:
#ifdef DMA_UIO
		get_file_info(TX_BUFF_MEM_SIZE, &tx_buff_mem_size);
		get_file_info(TX_BUFF_MEM_ADDR, &tx_buff_mem_addr);

		dev_mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
		if(dev_mem_fd == -1)
		{
			printf("%s: Can't open /dev/mem device\n\r", __func__);
			return;
		}

		page_size = sysconf(_SC_PAGESIZE);
		mapping_length = (((tx_buff_mem_size / page_size) + 1) * page_size);
		page_mask = (page_size - 1);
		mapping_addr = mmap(NULL,
				   mapping_length,
				   PROT_READ | PROT_WRITE,
				   MAP_SHARED,
				   dev_mem_fd,
				   (tx_buff_mem_addr & ~page_mask));
		if(mapping_addr == MAP_FAILED)
		{
			printf("%s: mmap error\n\r", __func__);
			return;
		}

		tx_buff_virt_addr = (mapping_addr + (tx_buff_mem_addr & page_mask));

		tx_count = sizeof(sine_lut) / sizeof(uint16_t);
		if(dds_st[phy->id_no].rx2tx2)
		{
			for(index = 0; index < (tx_count * 2); index += 2)
			{
				index_i1 = index;
				index_q1 = index + (tx_count / 2);
				if(index_q1 >= (tx_count * 2))
					index_q1 -= (tx_count * 2);
				data_i1 = (sine_lut[index_i1 / 2] << 20);
				data_q1 = (sine_lut[index_q1 / 2] << 4);
				*((unsigned *) (tx_buff_virt_addr + (index * 4))) = data_i1 | data_q1;
				index_i2 = index_i1;
				index_q2 = index_q1;
				if(index_i2 >= (tx_count * 2))
					index_i2 -= (tx_count * 2);
				if(index_q2 >= (tx_count * 2))
					index_q2 -= (tx_count * 2);
				data_i2 = (sine_lut[index_i2 / 2] << 20);
				data_q2 = (sine_lut[index_q2 / 2] << 4);
				*((unsigned *) (tx_buff_virt_addr + ((index + 1) * 4))) = data_i2 | data_q2;
			}
		}
		else
		{
			for(index = 0; index < tx_count; index += 1)
			{
				index_i1 = index;
				index_q1 = index + (tx_count / 4);
				if(index_q1 >= tx_count)
					index_q1 -= tx_count;
				data_i1 = (sine_lut[index_i1 / 2] << 20);
				data_q1 = (sine_lut[index_q1 / 2] << 4);
				*((unsigned *) (tx_buff_virt_addr + (index * 4))) = data_i1 | data_q1;
			}
		}

		munmap(mapping_addr, mapping_length);
		close(dev_mem_fd);

		if(dds_st[phy->id_no].rx2tx2)
		{
			length = (tx_count * 8);
		}
		else
		{
			length = (tx_count * 4);
		}
		dac_dma_write(AXI_DMAC_REG_CTRL, 0);
		dac_dma_write(AXI_DMAC_REG_CTRL, AXI_DMAC_CTRL_ENABLE);
		dac_dma_write(AXI_DMAC_REG_SRC_ADDRESS, tx_buff_mem_addr);
		dac_dma_write(AXI_DMAC_REG_SRC_STRIDE, 0x0);
		dac_dma_write(AXI_DMAC_REG_X_LENGTH, length - 1);
		dac_dma_write(AXI_DMAC_REG_Y_LENGTH, 0x0);
		dac_dma_write(AXI_DMAC_REG_START_TRANSFER, 0x1);
#endif
		dac_write(phy, DAC_REG_CNTRL_2, 0);
		dac_datasel(phy, -1, DATA_SEL_DMA);
		break;
	default:
		break;
	}
	dds_st[phy->id_no].enable = true;
	dac_start_sync(phy, 0);
}

/***************************************************************************//**
 * @brief dds_set_frequency
*******************************************************************************/
void dds_set_frequency(struct ad9361_rf_phy *phy, uint32_t chan, uint32_t freq)
{
	uint64_t val64;
	uint32_t reg;

	dds_st[phy->id_no].cached_freq[chan] = freq;
	dac_stop(phy);
	dac_read(phy, DAC_REG_CHAN_CNTRL_2_IIOCHAN(chan), &reg);
	reg &= ~DAC_DDS_INCR(~0);
	val64 = (uint64_t) freq * 0xFFFFULL;
	do_div(&val64, *dds_st[phy->id_no].dac_clk);
	reg |= DAC_DDS_INCR(val64) | 1;
	dac_write(phy, DAC_REG_CHAN_CNTRL_2_IIOCHAN(chan), reg);
	dac_start_sync(phy, 0);
}

/***************************************************************************//**
 * @brief dds_set_phase
*******************************************************************************/
void dds_set_phase(struct ad9361_rf_phy *phy, uint32_t chan, uint32_t phase)
{
	uint64_t val64;
	uint32_t reg;

	dds_st[phy->id_no].cached_phase[chan] = phase;
	dac_stop(phy);
	dac_read(phy, DAC_REG_CHAN_CNTRL_2_IIOCHAN(chan), &reg);
	reg &= ~DAC_DDS_INIT(~0);
	val64 = (uint64_t) phase * 0x10000ULL + (360000 / 2);
	do_div(&val64, 360000);
	reg |= DAC_DDS_INIT(val64);
	dac_write(phy, DAC_REG_CHAN_CNTRL_2_IIOCHAN(chan), reg);
	dac_start_sync(phy, 0);
}

/***************************************************************************//**
 * @brief dds_set_phase
*******************************************************************************/
void dds_set_scale(struct ad9361_rf_phy *phy, uint32_t chan, int32_t scale_micro_units)
{
	uint32_t scale_reg;
	uint32_t sign_part;
	uint32_t int_part;
	uint32_t fract_part;

	if (PCORE_VERSION_MAJOR(dds_st[phy->id_no].pcore_version) > 6)
	{
		if(scale_micro_units >= 1000000)
		{
			sign_part = 0;
			int_part = 1;
			fract_part = 0;
			dds_st[phy->id_no].cached_scale[chan] = 1000000;
			goto set_scale_reg;
		}
		if(scale_micro_units <= -1000000)
		{
			sign_part = 1;
			int_part = 1;
			fract_part = 0;
			dds_st[phy->id_no].cached_scale[chan] = -1000000;
			goto set_scale_reg;
		}
		dds_st[phy->id_no].cached_scale[chan] = scale_micro_units;
		if(scale_micro_units < 0)
		{
			sign_part = 1;
			int_part = 0;
			scale_micro_units *= -1;
		}
		else
		{
			sign_part = 0;
			int_part = 0;
		}
		fract_part = (uint32_t)(((uint64_t)scale_micro_units * 0x4000) / 1000000);
	set_scale_reg:
		scale_reg = (sign_part << 15) | (int_part << 14) | fract_part;
	}
	else
	{
		if(scale_micro_units >= 1000000)
		{
			scale_reg = 0;
			scale_micro_units = 1000000;
		}
		if(scale_micro_units <= 0)
		{
			scale_reg = 0;
			scale_micro_units = 0;
		}
		dds_st[phy->id_no].cached_scale[chan] = scale_micro_units;
		fract_part = (uint32_t)(scale_micro_units);
		scale_reg = 500000 / fract_part;
	}
	dac_stop(phy);
	dac_write(phy, DAC_REG_CHAN_CNTRL_1_IIOCHAN(chan), DAC_DDS_SCALE(scale_reg));
	dac_start_sync(phy, 0);
}

/***************************************************************************//**
 * @brief dds_update
*******************************************************************************/
void dds_update(struct ad9361_rf_phy *phy)
{
	uint32_t chan;

	for(chan = DDS_CHAN_TX1_I_F1; chan <= DDS_CHAN_TX2_Q_F2; chan++)
	{
		dds_set_frequency(phy, chan, dds_st[phy->id_no].cached_freq[chan]);
		dds_set_phase(phy, chan, dds_st[phy->id_no].cached_phase[chan]);
		dds_set_scale(phy, chan, dds_st[phy->id_no].cached_scale[chan]);
	}
}

/***************************************************************************//**
 * @brief dac_datasel
*******************************************************************************/
int dac_datasel(struct ad9361_rf_phy *phy, int32_t chan, enum dds_data_select sel)
{
	if (PCORE_VERSION_MAJOR(dds_st[phy->id_no].pcore_version) > 7) {
		if (chan < 0) { /* ALL */
			uint32_t i;
			for (i = 0; i < dds_st[phy->id_no].num_dds_channels; i++) {
				dac_write(phy, DAC_REG_CHAN_CNTRL_7(i), sel);
			}
		} else {
			dac_write(phy, DAC_REG_CHAN_CNTRL_7(chan), sel);
		}
	} else {
		uint32_t reg;

		switch(sel) {
		case DATA_SEL_DDS:
		case DATA_SEL_SED:
		case DATA_SEL_DMA:
			dac_read(phy, DAC_REG_CNTRL_2, &reg);
			reg &= ~DAC_DATA_SEL(~0);
			reg |= DAC_DATA_SEL(sel);
			dac_write(phy, DAC_REG_CNTRL_2, reg);
			break;
		default:
			return -EINVAL;
		}
	}

	return 0;
}
