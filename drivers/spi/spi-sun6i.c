// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2012 - 2014 Allwinner Tech
 * Pan Nan <pannan@allwinnertech.com>
 *
 * Copyright (C) 2014 Maxime Ripard
 * Maxime Ripard <maxime.ripard@free-electrons.com>
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/dmaengine.h>
#include <linux/dmapool.h>
#include <linux/spi/spi.h>
#include <linux/highmem.h>

#define SUN6I_AUTOSUSPEND_TIMEOUT 2000

#define SUN6I_FIFO_DEPTH 128
#define SUN8I_FIFO_DEPTH 64

#define SUN6I_GBL_CTL_REG 0x04
#define SUN6I_GBL_CTL_BUS_ENABLE BIT(0)
#define SUN6I_GBL_CTL_MASTER BIT(1)
#define SUN6I_GBL_CTL_TP BIT(7)
#define SUN6I_GBL_CTL_RST BIT(31)

#define SUN6I_TFR_CTL_REG 0x08
#define SUN6I_TFR_CTL_CPHA BIT(0)
#define SUN6I_TFR_CTL_CPOL BIT(1)
#define SUN6I_TFR_CTL_SPOL BIT(2)
#define SUN6I_TFR_CTL_CS_MASK 0x30
#define SUN6I_TFR_CTL_CS(cs) (((cs) << 4) & SUN6I_TFR_CTL_CS_MASK)
#define SUN6I_TFR_CTL_CS_MANUAL BIT(6)
#define SUN6I_TFR_CTL_CS_LEVEL BIT(7)
#define SUN6I_TFR_CTL_DHB BIT(8)
#define SUN6I_TFR_CTL_FBS BIT(12)
#define SUN6I_TFR_CTL_XCH BIT(31)

#define SUN6I_INT_CTL_REG 0x10
#define SUN6I_INT_CTL_RF_RDY BIT(0)
#define SUN6I_INT_CTL_TF_ERQ BIT(4)
#define SUN6I_INT_CTL_RF_OVF BIT(8)
#define SUN6I_INT_CTL_TC BIT(12)

#define SUN6I_INT_STA_REG 0x14

#define SUN6I_FIFO_CTL_REG 0x18
#define SUN6I_FIFO_CTL_RF_RDY_TRIG_LEVEL_MASK 0xff
#define SUN6I_FIFO_CTL_RF_DRQ_EN BIT(8)
#define SUN6I_FIFO_CTL_RF_RDY_TRIG_LEVEL_BITS 0
#define SUN6I_FIFO_CTL_RF_RST BIT(15)
#define SUN6I_FIFO_CTL_TF_ERQ_TRIG_LEVEL_MASK 0xff
#define SUN6I_FIFO_CTL_TF_ERQ_TRIG_LEVEL_BITS 16
#define SUN6I_FIFO_CTL_TF_DRQ_EN BIT(24)
#define SUN6I_FIFO_CTL_TF_RST BIT(31)

#define SUN6I_FIFO_STA_REG 0x1c
#define SUN6I_FIFO_STA_RF_CNT_MASK GENMASK(7, 0)
#define SUN6I_FIFO_STA_TF_CNT_MASK GENMASK(23, 16)

#define SUN6I_CLK_CTL_REG 0x24
#define SUN6I_CLK_CTL_CDR2_MASK 0xff
#define SUN6I_CLK_CTL_CDR2(div) (((div)&SUN6I_CLK_CTL_CDR2_MASK) << 0)
#define SUN6I_CLK_CTL_CDR1_MASK 0xf
#define SUN6I_CLK_CTL_CDR1(div) (((div)&SUN6I_CLK_CTL_CDR1_MASK) << 8)
#define SUN6I_CLK_CTL_DRS BIT(12)

#define SUN6I_MAX_XFER_SIZE 0xffffff

#define SUN6I_BURST_CNT_REG 0x30

#define SUN6I_XMIT_CNT_REG 0x34

#define SUN6I_BURST_CTL_CNT_REG 0x38

#define SUN6I_TXDATA_REG 0x200
#define SUN6I_RXDATA_REG 0x300

#define PB_CFG0_REG 0x02000400 + 1 * 0x24 //PB配置寄存器 A:0 B:1 C:2 ....
#define PB_DATA_REG 0x02000400 + 1 * 0x34 //PB数据寄存器 A:0 B:1 C:2 ....
#define PIN_N 7
#define N (PIN_N % 8 * 4)
volatile unsigned int *PB_DAT;
volatile unsigned int *PB_CFG;

#define TEST0_0() *PB_DAT &= ~(1 << PIN_N)
#define TEST0_1() *PB_DAT |= (1 << PIN_N)

struct sun6i_spi_cfg {
	unsigned long fifo_depth;
	bool has_clk_ctl;
};

struct sun6i_spi {
	struct spi_master *master;
	void __iomem *base_addr;
	dma_addr_t dma_addr_rx;
	dma_addr_t dma_addr_tx;
	struct clk *hclk;
	struct clk *mclk;
	struct reset_control *rstc;

	struct completion done;

	const u8 *tx_buf;
	u8 *rx_buf;
	int len;
	const struct sun6i_spi_cfg *cfg;
};
static inline u32 sun6i_spi_read(struct sun6i_spi *sspi, u32 reg)

{
	return readl(sspi->base_addr + reg);
}

static inline void sun6i_spi_write(struct sun6i_spi *sspi, u32 reg, u32 value)
{
	writel(value, sspi->base_addr + reg);
}

static inline u32 sun6i_spi_get_rx_fifo_count(struct sun6i_spi *sspi)
{
	u32 reg = sun6i_spi_read(sspi, SUN6I_FIFO_STA_REG);

	return FIELD_GET(SUN6I_FIFO_STA_RF_CNT_MASK, reg);
}

static inline u32 sun6i_spi_get_tx_fifo_count(struct sun6i_spi *sspi)
{
	u32 reg = sun6i_spi_read(sspi, SUN6I_FIFO_STA_REG);

	return FIELD_GET(SUN6I_FIFO_STA_TF_CNT_MASK, reg);
}

static inline void sun6i_spi_disable_interrupt(struct sun6i_spi *sspi, u32 mask)
{
	u32 reg = sun6i_spi_read(sspi, SUN6I_INT_CTL_REG);

	reg &= ~mask;
	sun6i_spi_write(sspi, SUN6I_INT_CTL_REG, reg);
}

static inline void sun6i_spi_drain_fifo(struct sun6i_spi *sspi)
{
	u32 len;
	u8 byte;

	/* See how much data is available */
	len = sun6i_spi_get_rx_fifo_count(sspi);

	while (len--) {
		byte = readb(sspi->base_addr + SUN6I_RXDATA_REG);
		if (sspi->rx_buf)
			*sspi->rx_buf++ = byte;
	}
}

static inline void sun6i_spi_fill_fifo(struct sun6i_spi *sspi)
{
	u32 cnt;
	int len;
	u8 byte;

	/* See how much data we can fit */
	cnt = sspi->cfg->fifo_depth - sun6i_spi_get_tx_fifo_count(sspi);

	len = min((int)cnt, sspi->len);

	while (len--) {
		byte = sspi->tx_buf ? *sspi->tx_buf++ : 0;
		writeb(byte, sspi->base_addr + SUN6I_TXDATA_REG);
		sspi->len--;
	}
}

static void sun6i_spi_set_cs(struct spi_device *spi, bool enable)
{
	struct sun6i_spi *sspi = spi_master_get_devdata(spi->master);
	u32 reg;

	reg = sun6i_spi_read(sspi, SUN6I_TFR_CTL_REG);
	reg &= ~SUN6I_TFR_CTL_CS_MASK;
	reg |= SUN6I_TFR_CTL_CS(spi->chip_select);

	if (enable)
		reg |= SUN6I_TFR_CTL_CS_LEVEL;
	else
		reg &= ~SUN6I_TFR_CTL_CS_LEVEL;

	sun6i_spi_write(sspi, SUN6I_TFR_CTL_REG, reg);
}

static size_t sun6i_spi_max_transfer_size(struct spi_device *spi)
{
	return SUN6I_MAX_XFER_SIZE - 1;
}

static int sun6i_spi_prepare_dma(struct sun6i_spi *sspi,
				 struct spi_transfer *tfr)
{
	struct dma_async_tx_descriptor *rxdesc, *txdesc;
	struct spi_master *master = sspi->master;

	rxdesc = NULL;
	if (tfr->rx_buf) {
		struct dma_slave_config rxconf = {
			.direction = DMA_DEV_TO_MEM,
			.src_addr = sspi->dma_addr_rx,
			.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES,
			.src_maxburst = 8,
		};

		dmaengine_slave_config(master->dma_rx, &rxconf);

		rxdesc = dmaengine_prep_slave_sg(master->dma_rx, tfr->rx_sg.sgl,
						 tfr->rx_sg.nents,
						 DMA_DEV_TO_MEM,
						 DMA_PREP_INTERRUPT);
		if (!rxdesc)
			return -EINVAL;
	}

	txdesc = NULL;
	if (tfr->tx_buf) {
		struct dma_slave_config txconf = {
			.direction = DMA_MEM_TO_DEV,
			.dst_addr = sspi->dma_addr_tx,
			.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES,
			.dst_maxburst = 8,
		};

		dmaengine_slave_config(master->dma_tx, &txconf);

		txdesc = dmaengine_prep_slave_sg(master->dma_tx, tfr->tx_sg.sgl,
						 tfr->tx_sg.nents,
						 DMA_MEM_TO_DEV,
						 DMA_PREP_INTERRUPT);
		if (!txdesc) {
			if (rxdesc)
				dmaengine_terminate_sync(master->dma_rx);
			return -EINVAL;
		}
	}

	if (tfr->rx_buf) {
		dmaengine_submit(rxdesc);
		dma_async_issue_pending(master->dma_rx);
	}

	if (tfr->tx_buf) {
		dmaengine_submit(txdesc);
		dma_async_issue_pending(master->dma_tx);
	}

	return 0;
}

volatile unsigned int *SPI_BASE_MAP;
volatile unsigned int *DMA_BASE_MAP;

#define SPI_BASE 0x04026000
#define SPI_SIZE ((0x300 / 4) + 1)

#define DMA_BASE    0x03002000
#define DMA_SIZE (((0x0130 + 0x40 * 7) / 4) + 1)


#define SREG(reg) *(SPI_BASE_MAP + (reg) / 4)
#define DMAREG(reg) *(DMA_BASE_MAP + (reg) / 4)

#define DEBUG_REG0  (0x30)
#define DEBUG_REG1  (0x0100 + 0x40 * 0)
#define DEBUG_REG2  (0x0108 + 0x40 * 0)
#define DEBUG_REG3  (0x010C + 0x40 * 0)
#define DMA_DESC(i) (0x0108 + 0x40 * i)
#define DMA_PEND    (0x0010 + 0x40 * 0)


static void printf_dma_spi_status(void)
{
uint32_t dma_show[16]={
    0x00,
    0x10,
    0x20,
    0x28,
    0x30,
    0x100,
    0x104,
    0x108,
    0x10C,
    0x110,
    0x114,
    0x118,
    0x11C,
    0x128,
    0x12C,
    0x0130
};

uint32_t spi_show[16]={
    0x04,
    0x08,
    0x10,
    0x14,
    0x18,
    0x1C,
    0x20,
    0x28,
    0x30,
    0x34,
    0x38,
    0x40,
    0x44,
    0x48,
    0x4C,
    0x88,
};
    int i=0;
    printk(KERN_CONT "DMA0=>");
    for(i=0;i<16;i++)
        printk(KERN_CONT"%08X ",*(DMA_BASE_MAP+ dma_show[i] / 4));
    printk(KERN_CONT "DMA1=>");
    for(i=0;i<16;i++)
        printk(KERN_CONT"%08X ",*(DMA_BASE_MAP+ (dma_show[i] + 0x40) / 4));
    printk(KERN_CONT "SPI=>");
    for(i=0;i<16;i++)
        printk(KERN_CONT"%08X ",*(SPI_BASE_MAP+ spi_show[i] / 4));
    printk(KERN_CONT "\n");
}

static void printf_status(void)
{
    int i=0;
    // for(i=0;i<16;i++)
    //     printk(KERN_CONT"%08X ",*(SPI_DMA_MAP+ dma_show[i] / 4));

    printk(KERN_CONT"sta:%08X ",*(DMA_BASE_MAP + 0x30 / 4));
    
    printk(KERN_CONT "en:");
    for(i=0;i<8;i++)
        printk(KERN_CONT"%d ",*(DMA_BASE_MAP + (0x0100 + 0x40 * i) / 4));

    printk(KERN_CONT "pause:");
    for(i=0;i<8;i++)
        printk(KERN_CONT"%d ",*(DMA_BASE_MAP + (0x0104 + 0x40 * i) / 4));

    printk(KERN_CONT"desc0:%08X ",*(DMA_BASE_MAP + (0x0108 + 0x40 * 0) / 4));
    printk(KERN_CONT"desc1:%08X ",*(DMA_BASE_MAP + (0x0108 + 0x40 * 1) / 4));

    printk(KERN_CONT"config0:%08X ",*(DMA_BASE_MAP + (0x010C + 0x40 * 0) / 4));
    printk(KERN_CONT"config1:%08X ",*(DMA_BASE_MAP + (0x010C + 0x40 * 1) / 4));

    printk(KERN_CONT "\n");
}

struct sun6i_dma_lli {
	u32			cfg;
	u32			src;
	u32			dst;
	u32			len;
	u32			para;
	u32			p_lli_next;
};

uint32_t *rxbuf;
uint32_t *txbuf;

struct sun6i_dma_lli *desc_tx;
struct sun6i_dma_lli *desc_rx;
dma_addr_t desc_txp,desc_rxp,rxp,txp;
struct dma_pool *pool;
static int is_first = 3;

static void init_spi_fo(struct spi_transfer *tfr,bool is_dma)
{
    //SREG(SUN6I_INT_CTL_REG) = SUN6I_INT_CTL_TC; //isr
    SREG(SUN6I_INT_CTL_REG) = 0;
    SREG(SUN6I_INT_STA_REG) = 0xFFFFFFFF;

    if(is_dma)
    {
        desc_tx->cfg = 0x05970481;
        desc_tx->src = tfr->tx_sg.sgl->dma_address;// tfr->tx_buf;
        desc_tx->dst = 0x04026200;//(SPI_BASE_MAP + (reg) / 4);//(SPI_BASE_MAP + SUN6I_TXDATA_REG / 4);
        desc_tx->len = 16;
        desc_tx->para = 0x00000008;
        desc_tx->p_lli_next = desc_txp;//0xFFFFF800;

        desc_rx->cfg = 0x04810597;
        desc_rx->src = 0x04026300;//(SPI_BASE_MAP + SUN6I_RXDATA_REG / 4);
        desc_rx->dst = tfr->rx_sg.sgl->dma_address;// tfr->rx_buf;
        desc_rx->len = 16;
        desc_rx->para = 0x00000008;
        desc_rx->p_lli_next = desc_rxp;//0xFFFFF800;
    }


    SREG(SUN6I_FIFO_CTL_REG) = (SUN6I_FIFO_CTL_RF_RST | SUN6I_FIFO_CTL_TF_RST);

    SREG(SUN6I_FIFO_CTL_REG) = 0x01200120;

    SREG(SUN6I_GBL_CTL_REG) = (SUN6I_GBL_CTL_BUS_ENABLE | SUN6I_GBL_CTL_MASTER);

    //SREG(SUN6I_GBL_CTL_REG) = (SUN6I_GBL_CTL_BUS_ENABLE | SUN6I_GBL_CTL_MASTER | SUN6I_GBL_CTL_TP);
    SREG(SUN6I_TFR_CTL_REG) = (SUN6I_TFR_CTL_SPOL | SUN6I_TFR_CTL_CS_MANUAL | SUN6I_TFR_CTL_CS_LEVEL);

    if(is_dma)
    {
        DMAREG(DMA_DESC(0)) = (uint32_t)desc_rxp;
        DMAREG(0x0100 + 0x40 * 0) = 0x01;

        DMAREG(DMA_DESC(1)) = (uint32_t)desc_txp;
        DMAREG(0x0100 + 0x40 * 1) = 0x01;
    }
}

static int sun6i_spi_transfer_one_dma(struct spi_master *master,
				  struct spi_device *spi,
				  struct spi_transfer *tfr)
{
	static bool first = true;
	
	int ret = 0;

	u32 tttimeout = 100000;

    if(first)
    {
        init_spi_fo(tfr,true);
        first = false;
    }

	TEST0_0();

	SREG(SUN6I_BURST_CNT_REG) = 16;//16*1024;
	SREG(SUN6I_XMIT_CNT_REG) = 16;//16*1024;
	SREG(SUN6I_BURST_CTL_CNT_REG) = 16;//16*1024;

    DMAREG(0x0100 + 0x40 * 0) = 0x01;

	SREG(SUN6I_TFR_CTL_REG) |= SUN6I_TFR_CTL_XCH;
	while (tttimeout--) {
		if (SREG(SUN6I_INT_STA_REG) & SUN6I_INT_CTL_TC) {
			SREG(SUN6I_INT_STA_REG) = SUN6I_INT_CTL_TC;
			break;
		}
	}

    SREG(SUN6I_INT_CTL_REG) = 0;

    TEST0_1();

	return ret;
}



static int sun6i_spi_transfer_one(struct spi_master *master,
				   struct spi_device *spi,
				   struct spi_transfer *tfr)
{
	u32 tttimeout = 100000;
	u32 reg;

	static bool first = true;
	if (first) {
		init_spi_fo(tfr,false);
		first = false;
		return 0;
	}

	TEST0_1();

	SREG(SUN6I_BURST_CNT_REG) = 16;
	SREG(SUN6I_XMIT_CNT_REG) = 16;
	SREG(SUN6I_BURST_CTL_CNT_REG) = 16;

	SREG(SUN6I_TXDATA_REG) = *(((uint32_t *)(tfr->tx_buf))+0);
	SREG(SUN6I_TXDATA_REG) = *(((uint32_t *)(tfr->tx_buf))+1);
	SREG(SUN6I_TXDATA_REG) = *(((uint32_t *)(tfr->tx_buf))+2);
	SREG(SUN6I_TXDATA_REG) = *(((uint32_t *)(tfr->tx_buf))+3);

	SREG(SUN6I_TFR_CTL_REG) |=  SUN6I_TFR_CTL_XCH;

	while (tttimeout--) {
		/* Transfer complete */
		if (SREG(SUN6I_INT_STA_REG) & SUN6I_INT_CTL_TC) {
			SREG(SUN6I_INT_STA_REG) = SUN6I_INT_CTL_TC;
			*(((uint32_t *)(tfr->rx_buf))+0) = SREG(SUN6I_RXDATA_REG);
			*(((uint32_t *)(tfr->rx_buf))+1) = SREG(SUN6I_RXDATA_REG);
			*(((uint32_t *)(tfr->rx_buf))+2) = SREG(SUN6I_RXDATA_REG);
			*(((uint32_t *)(tfr->rx_buf))+3) = SREG(SUN6I_RXDATA_REG);

			break;
		}
	}
	
	TEST0_0();
	return 0;
}

static irqreturn_t sun6i_spi_handler(int irq, void *dev_id)
{

    SREG(SUN6I_INT_STA_REG) = 0xFFFFFFFF;

	return IRQ_NONE;
}

static int sun6i_spi_runtime_resume(struct device *dev)
{
	struct spi_master *master = dev_get_drvdata(dev);
	struct sun6i_spi *sspi = spi_master_get_devdata(master);
	int ret;

	ret = clk_prepare_enable(sspi->hclk);
	if (ret) {
		dev_err(dev, "Couldn't enable AHB clock\n");
		goto out;
	}

	ret = clk_prepare_enable(sspi->mclk);
	if (ret) {
		dev_err(dev, "Couldn't enable module clock\n");
		goto err;
	}

	ret = reset_control_deassert(sspi->rstc);
	if (ret) {
		dev_err(dev, "Couldn't deassert the device from reset\n");
		goto err2;
	}

	sun6i_spi_write(sspi, SUN6I_GBL_CTL_REG,
			SUN6I_GBL_CTL_MASTER | SUN6I_GBL_CTL_TP);

	return 0;

err2:
	clk_disable_unprepare(sspi->mclk);
err:
	clk_disable_unprepare(sspi->hclk);
out:
	return ret;
}

static int sun6i_spi_runtime_suspend(struct device *dev)
{
	struct spi_master *master = dev_get_drvdata(dev);
	struct sun6i_spi *sspi = spi_master_get_devdata(master);

	reset_control_assert(sspi->rstc);
	clk_disable_unprepare(sspi->mclk);
	clk_disable_unprepare(sspi->hclk);

	return 0;
}

static bool sun6i_spi_can_dma(struct spi_master *master, struct spi_device *spi,
			      struct spi_transfer *xfer)
{
	struct sun6i_spi *sspi = spi_master_get_devdata(master);

	/*
	 * If the number of spi words to transfer is less or equal than
	 * the fifo length we can just fill the fifo and wait for a single
	 * irq, so don't bother setting up dma
	 */
	return true; //xfer->len > sspi->cfg->fifo_depth;
}

static int sun6i_spi_probe(struct platform_device *pdev)
{
	struct spi_master *master;
	struct sun6i_spi *sspi;
	struct resource *mem;
	int ret = 0, irq;

	unsigned long val = 0;

	pool = dmam_pool_create(dev_name(&pdev->dev), &pdev->dev,
				     128, 4, 0);

    desc_tx = dma_pool_alloc(pool, GFP_NOWAIT, &desc_txp);
	if (!desc_tx) {
		printk("Failed to alloc lli0 memory\n");
	}

    desc_rx = dma_pool_alloc(pool, GFP_NOWAIT, &desc_rxp);
	if (!desc_rx) {
		printk("Failed to alloc lli1 memory\n");
	}

    // rxbuf = dma_pool_alloc(pool, GFP_NOWAIT, &desc_rxp);
	// if (!rxbuf) {
	// 	printk("Failed to alloc lli1 memory\n");
	// }

    // txbuf = dma_pool_alloc(pool, GFP_NOWAIT, &desc_rxp);
	// if (!txbuf) {
	// 	printk("Failed to alloc lli1 memory\n");
	// }


	PB_DAT = (volatile unsigned int *)ioremap(PB_DATA_REG, 1);
	PB_CFG = (volatile unsigned int *)ioremap(PB_CFG0_REG, 1);

	*PB_CFG &= ~(7 << N); //7=111 取反000 20：22设置000 默认是0x7=111 失能
	*PB_CFG |= (1 << N); //设置输出 20：22设置001

	TEST0_0();

SPI_BASE_MAP = (volatile unsigned int *)ioremap(SPI_BASE, SPI_SIZE);
DMA_BASE_MAP = (volatile unsigned int *)ioremap(DMA_BASE, DMA_SIZE);

	master = spi_alloc_master(&pdev->dev, sizeof(struct sun6i_spi));
	if (!master) {
		dev_err(&pdev->dev, "Unable to allocate SPI Master\n");
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, master);
	sspi = spi_master_get_devdata(master);

	sspi->base_addr = devm_platform_get_and_ioremap_resource(pdev, 0, &mem);
	if (IS_ERR(sspi->base_addr)) {
		ret = PTR_ERR(sspi->base_addr);
		goto err_free_master;
	}

	// irq = platform_get_irq(pdev, 0);
	// if (irq < 0) {
	// 	ret = -ENXIO;
	// 	goto err_free_master;
	// }

	// ret = devm_request_irq(&pdev->dev, irq, sun6i_spi_handler,
	// 		       0, "sun6i-spi", sspi);
	// if (ret) {
	// 	dev_err(&pdev->dev, "Cannot request IRQ\n");
	// 	goto err_free_master;
	// }

	sspi->master = master;
	sspi->cfg = of_device_get_match_data(&pdev->dev);

	master->max_speed_hz = 100 * 1000 * 1000;
	master->min_speed_hz = 3 * 1000;
	master->use_gpio_descriptors = true;
	master->set_cs = sun6i_spi_set_cs;
	master->transfer_one = sun6i_spi_transfer_one;
	master->num_chipselect = 4;
	master->mode_bits = SPI_CPOL | SPI_CPHA | SPI_CS_HIGH | SPI_LSB_FIRST;
	master->bits_per_word_mask = SPI_BPW_MASK(8);
	master->dev.of_node = pdev->dev.of_node;
	master->auto_runtime_pm = true;
	master->max_transfer_size = sun6i_spi_max_transfer_size;

	sspi->hclk = devm_clk_get(&pdev->dev, "ahb");
	if (IS_ERR(sspi->hclk)) {
		dev_err(&pdev->dev, "Unable to acquire AHB clock\n");
		ret = PTR_ERR(sspi->hclk);
		goto err_free_master;
	}

	sspi->mclk = devm_clk_get(&pdev->dev, "mod");
	if (IS_ERR(sspi->mclk)) {
		dev_err(&pdev->dev, "Unable to acquire module clock\n");
		ret = PTR_ERR(sspi->mclk);
		goto err_free_master;
	}
    clk_set_rate(sspi->mclk, 30*1000*1000);

	init_completion(&sspi->done);

	sspi->rstc = devm_reset_control_get_exclusive(&pdev->dev, NULL);
	if (IS_ERR(sspi->rstc)) {
		dev_err(&pdev->dev, "Couldn't get reset controller\n");
		ret = PTR_ERR(sspi->rstc);
		goto err_free_master;
	}

	master->dma_tx = dma_request_chan(&pdev->dev, "tx");
	if (IS_ERR(master->dma_tx)) {
		/* Check tx to see if we need defer probing driver */
		if (PTR_ERR(master->dma_tx) == -EPROBE_DEFER) {
			ret = -EPROBE_DEFER;
			goto err_free_master;
		}
		dev_warn(&pdev->dev, "Failed to request TX DMA channel\n");
		master->dma_tx = NULL;
	}

	master->dma_rx = dma_request_chan(&pdev->dev, "rx");
	if (IS_ERR(master->dma_rx)) {
		if (PTR_ERR(master->dma_rx) == -EPROBE_DEFER) {
			ret = -EPROBE_DEFER;
			goto err_free_dma_tx;
		}
		dev_warn(&pdev->dev, "Failed to request RX DMA channel\n");
		master->dma_rx = NULL;
	}

	if (master->dma_tx && master->dma_rx) {
		sspi->dma_addr_tx = mem->start + SUN6I_TXDATA_REG;
		sspi->dma_addr_rx = mem->start + SUN6I_RXDATA_REG;
		master->can_dma = sun6i_spi_can_dma;
	}

	/*
	 * This wake-up/shutdown pattern is to be able to have the
	 * device woken up, even if runtime_pm is disabled
	 */
	ret = sun6i_spi_runtime_resume(&pdev->dev);
	if (ret) {
		dev_err(&pdev->dev, "Couldn't resume the device\n");
		goto err_free_dma_rx;
	}

	pm_runtime_set_autosuspend_delay(&pdev->dev, SUN6I_AUTOSUSPEND_TIMEOUT);
	pm_runtime_use_autosuspend(&pdev->dev);
	pm_runtime_set_active(&pdev->dev);
	pm_runtime_enable(&pdev->dev);

	ret = devm_spi_register_master(&pdev->dev, master);
	if (ret) {
		dev_err(&pdev->dev, "cannot register SPI master\n");
		goto err_pm_disable;
	}

    //init_spi_fo(NULL,false);

	return 0;

err_pm_disable:
	pm_runtime_disable(&pdev->dev);
	sun6i_spi_runtime_suspend(&pdev->dev);
err_free_dma_rx:
	if (master->dma_rx)
		dma_release_channel(master->dma_rx);
err_free_dma_tx:
	if (master->dma_tx)
		dma_release_channel(master->dma_tx);
err_free_master:
	spi_master_put(master);
	return ret;
}

static int sun6i_spi_remove(struct platform_device *pdev)
{
	struct spi_master *master = platform_get_drvdata(pdev);

	pm_runtime_force_suspend(&pdev->dev);

	if (master->dma_tx)
		dma_release_channel(master->dma_tx);
	if (master->dma_rx)
		dma_release_channel(master->dma_rx);
	return 0;
}

static const struct sun6i_spi_cfg sun6i_a31_spi_cfg = {
	.fifo_depth = SUN6I_FIFO_DEPTH,
	.has_clk_ctl = true,
};

static const struct sun6i_spi_cfg sun8i_h3_spi_cfg = {
	.fifo_depth = SUN8I_FIFO_DEPTH,
	.has_clk_ctl = true,
};

static const struct sun6i_spi_cfg sun50i_r329_spi_cfg = {
	.fifo_depth = SUN8I_FIFO_DEPTH,
};

static const struct of_device_id sun6i_spi_match[] = {
	{ .compatible = "allwinner,sun6i-a31-spi", .data = &sun6i_a31_spi_cfg },
	{ .compatible = "allwinner,sun8i-h3-spi", .data = &sun8i_h3_spi_cfg },
	{ .compatible = "allwinner,sun50i-r329-spi",
	  .data = &sun50i_r329_spi_cfg },
	{ .compatible = "allwinner,sun50i-r329-spi-dbi",
	  .data = &sun50i_r329_spi_cfg },
	{}
};
MODULE_DEVICE_TABLE(of, sun6i_spi_match);

static const struct dev_pm_ops sun6i_spi_pm_ops = {
	.runtime_resume = NULL,//sun6i_spi_runtime_resume,
	.runtime_suspend = NULL,//sun6i_spi_runtime_suspend,
};

static struct platform_driver sun6i_spi_driver = {
	.probe	= sun6i_spi_probe,
	.remove	= sun6i_spi_remove,
	.driver	= {
		.name		= "sun6i-spi",
		.of_match_table	= sun6i_spi_match,
		.pm		= &sun6i_spi_pm_ops,
	},
};
module_platform_driver(sun6i_spi_driver);

MODULE_AUTHOR("Pan Nan <pannan@allwinnertech.com>");
MODULE_AUTHOR("Maxime Ripard <maxime.ripard@free-electrons.com>");
MODULE_DESCRIPTION("Allwinner A31 SPI controller driver");
MODULE_LICENSE("GPL");
