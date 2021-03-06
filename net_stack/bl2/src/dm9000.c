/*
 *   dm9000.c: Version 1.2 12/15/2003
 *   A Davicom DM9000 ISA NIC fast Ethernet driver for Linux.
 *   Copyright (C) 1997  Sten Wang
 *   This program is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU General Public License
 *   as published by the Free Software Foundation; either version 2
 *   of the License, or (at your option) any later version.
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *   (C)Copyright 1997-1998 DAVICOM Semiconductor,Inc. All Rights Reserved.
 *   V0.11	06/20/2001	REG_0A bit3=1, default enable BP with DA match
 *   06/22/2001	Support DM9801 progrmming
 *   E3: R25 = ((R24 + NF) & 0x00ff) | 0xf000
 *   E4: R25 = ((R24 + NF) & 0x00ff) | 0xc200
 *   R17 = (R17 & 0xfff0) | NF + 3
 *   E5: R25 = ((R24 + NF - 3) & 0x00ff) | 0xc200
 *   R17 = (R17 & 0xfff0) | NF
 *   v1.00			modify by simon 2001.9.5
 *   change for kernel 2.4.x
 *   v1.1   11/09/2001	fix force mode bug
 *   v1.2   03/18/2003       Weilun Huang <weilun_huang@davicom.com.tw>:
 *   Fixed phy reset.
 *   Added tx/rx 32 bit mode.
 *   Cleaned up for kernel merge.
 *   --------------------------------------
 *   12/15/2003       Initial port to u-boot by   Sascha Hauer <saschahauer@web.de>
 *   06/03/2008	Remy Bohmer <linux@bohmer.net>
 *   - Fixed the driver to work with DM9000A.
 *   (check on ISR receive status bit before reading the
 *   FIFO as described in DM9000 programming guide and
 *   application notes)
 *  - Added autodetect of databus width.
 *  - Made debug code compile again.
 *  - Adapt eth_send such that it matches the DM9000*
 *  - application notes. Needed to make it work properly
 *    for DM9000A.
 *  - Adapted reset procedure to match DM9000 application
 *    notes (i.e. double reset)
 *  - some minor code cleanups
 *  These changes are tested with DM9000{A,EP,E} together
 *  with a 200MHz Atmel AT91SAM9261 core
 *  TODO: external MII is not functional, only internal at the moment.
 * 
 *
 * - adapted for bl2 code running from SRAM in S5PV210 based Tiny210 v1-1204 board from friendly arm
 *   by moaz korena <moazkorena@gmail.com>
 */

#include <net.h>
#include "eth.h"
#include <stdint.h>
#include <io.h>
#include <dm9000.h>
#include <terminal.h>
#include <timer.h>




/*static function declaration ------------------------------------- */
static int dm9000_probe(void);
static uint16_t dm9000_phy_read(int);
static void dm9000_phy_write(int, uint16_t);
static uint8_t DM9000_ior(int);
static void DM9000_iow(int reg, uint8_t value);


/* Board/System/Debug information/definition ---------------- */

/* #define CONFIG_DM9000_DEBUG */

#ifdef CONFIG_DM9000_DEBUG
#define DM9000_DBG(fmt,...) print_format(fmt,##__VA_ARGS__)
#define DM9000_DMP_PACKET(func,packet,length)  \
	do { \
		int i; 							\
		print_format(": length: %d\n",length);		\
		for (i = 0; i < length; i++) {				\
			if (i % 8 == 0){					\
				uart_print(func);				\
				print_format(": %x: ", i);}	\
			print_format("%x ", ((unsigned char *) packet)[i]);	\
		} print_format("\n");						\
	} while(0)
#else
#define DM9000_DBG(fmt,args...)
#define DM9000_DMP_PACKET(func,packet,length)
#endif

/* Structure/enum declaration ------------------------------- */
typedef struct board_info {
	uint32_t runt_length_counter;	/* counter: RX length < 64byte */
	uint32_t long_length_counter;	/* counter: RX length > 1514byte */
	uint32_t reset_counter;	/* counter: RESET */
	uint32_t reset_tx_timeout;	/* RESET caused by TX Timeout */
	uint32_t reset_rx_status;	/* RESET caused by RX Statsus wrong */
	uint16_t tx_pkt_cnt;
	uint16_t queue_start_addr;
	uint16_t dbug_cnt;
	uint8_t phy_addr;
	uint8_t device_wait_reset;	/* device state */
	unsigned char srom[128];
	void (*outblk)(volatile void *data_ptr, int count);
	void (*inblk)(void *data_ptr, int count);
	void (*rx_status)(uint16_t *RxStatus, uint16_t *RxLen);
	struct eth_device netdev;
} board_info_t;
static board_info_t dm9000_info;



/* DM9000 network board routine ---------------------------- */
//#ifndef CONFIG_DM9000_BYTE_SWAPPED
//#define DM9000_outb(d,r) writeb(d, (volatile uint8_t *)(r))
//#define DM9000_outw(d,r) writew(d, (volatile uint16_t *)(r))
//#define DM9000_outl(d,r) writel(d, (volatile uint32_t *)(r))
//#define DM9000_inb(r) readb((volatile uint8_t *)(r))
//#define DM9000_inw(r) readw((volatile uint16_t *)(r))
//#define DM9000_inl(r) readl((volatile uint32_t *)(r))
//#else
#define DM9000_outb(d, r) _raw_writeb(d, r)
#define DM9000_outw(d, r) _raw_writehw(d, r)
#define DM9000_outl(d, r) _raw_writel(d, r)
#define DM9000_inb(r) _raw_readb(r)
//#define _raw_readw(a)		(*(volatile uint16_t *)(a))
#define DM9000_inw(r) _raw_readhw(r)
#define DM9000_inl(r) _raw_readl(r)
//#endif

#ifdef CONFIG_DM9000_DEBUG
	void
dm9000_dump_regs(void)
{
	DM9000_DBG("dumping registers .. \n\r");
	DM9000_DBG("NCR   (0x00): %x\n\r", DM9000_ior(0));
	DM9000_DBG("NSR   (0x01): %x\n\r", DM9000_ior(1));
	DM9000_DBG("TCR   (0x02): %x\n\r", DM9000_ior(2));
	DM9000_DBG("TSRI  (0x03): %x\n\r", DM9000_ior(3));
	DM9000_DBG("TSRII (0x04): %x\n\r", DM9000_ior(4));
	DM9000_DBG("RCR   (0x05): %x\n\r", DM9000_ior(5));
	DM9000_DBG("RSR   (0x06): %x\n\r", DM9000_ior(6));
	DM9000_DBG("BPTR  (0x08): %x\n\r", DM9000_ior(8));
      	DM9000_DBG("ISR   (0xFE): %x\n\r", DM9000_ior(DM9000_ISR));
	DM9000_DBG("IMR   (0xFF): %x\n\r", DM9000_ior(DM9000_IMR));
	DM9000_DBG("FCR   (0x0A): %x\n\r", DM9000_ior(DM9000_FCR));
	DM9000_DBG("done dumping registers \n\r");
}

void dm9000_dump_eth_frame(uint8_t *packet,int len){
	print_format("ethernet header print ------\n\r");

	for(int a=0;a<len;a++)
		print_format("%d byte is: 0x%x\n\r",a,packet[a]);

	print_format("ethernet header print end ------\n\r");
}

#endif

static void dm9000_outblk_8bit(volatile void *data_ptr, int count)
{
	int i;
	for (i = 0; i < count; i++)
		DM9000_outb((((uint8_t *) data_ptr)[i] & 0xff), DM9000_DATA);
}

static void dm9000_outblk_16bit(volatile void *data_ptr, int count)
{
	int i;
	uint32_t tmplen = (count + 1) / 2;

	for (i = 0; i < tmplen; i++)
		DM9000_outw(((uint16_t *) data_ptr)[i], DM9000_DATA);
}
static void dm9000_outblk_32bit(volatile void *data_ptr, int count)
{
	int i;
	uint32_t tmplen = (count + 3) / 4;

	for (i = 0; i < tmplen; i++)
		DM9000_outl(((uint32_t *) data_ptr)[i], DM9000_DATA);
}

static void dm9000_inblk_8bit(void *data_ptr, int count)
{
	int i;
	for (i = 0; i < count; i++)
		((uint8_t *) data_ptr)[i] = DM9000_inb(DM9000_DATA);
}



static void dm9000_inblk_16bit(void *data_ptr, int count)
{
	int i;
	uint32_t tmplen = (count + 1) / 2;

	for (i = 0; i< tmplen; i++){
		((uint16_t *) data_ptr)[i] = DM9000_inw(DM9000_DATA);
	}
}
static void dm9000_inblk_32bit(void *data_ptr, int count)
{
	int i;
	uint32_t tmplen = (count + 3) / 4;

	for (i = 0; i < tmplen; i++){
		((uint32_t *) data_ptr)[i] = DM9000_inl(DM9000_DATA);
	}
}



static void dm9000_rx_status_32bit(uint16_t *RxStatus, uint16_t *RxLen)
{
	uint32_t tmpdata;

	DM9000_outb(DM9000_MRCMD, DM9000_IO);

	tmpdata = DM9000_inl(DM9000_DATA);
	/* We'll assume no endianness business is required, seems safe enough ...*/
	*RxStatus = (uint16_t) (tmpdata & 0xff); // discarding higher bits, cause I'm paranoid like that 
	*RxLen = (uint16_t) (tmpdata >> 16);
}


static void dm9000_rx_status_16bit(uint16_t *RxStatus, uint16_t *RxLen)
{
	DM9000_outb(DM9000_MRCMD, DM9000_IO);
//	udelay(8000);
	*RxStatus = (uint16_t) (DM9000_inw(DM9000_DATA)); // reads first half word, including 0x1 as first byte
	*RxLen = (uint16_t) DM9000_inw(DM9000_DATA);
}

static void dm9000_rx_status_8bit(uint16_t *RxStatus, uint16_t *RxLen)
{
	uint32_t tmpdata;

	DM9000_outb(DM9000_MRCMD, DM9000_IO);

	tmpdata = DM9000_inb(DM9000_DATA);
	/* We'll assume no endianness business is required, seems safe enough ...*/
	*RxStatus = (uint16_t) (tmpdata & 0xff); // discarding higher bits, cause I'm paranoid like that 
	*RxLen = (uint16_t) (tmpdata >> 16);
}


/*
 *   Search DM9000 board, allocate space and register it
 *
 **/
int dm9000_probe(void)
{
	uint32_t id_val;
	id_val = DM9000_ior(DM9000_VIDL);
	id_val |= DM9000_ior(DM9000_VIDH) << 8;
	id_val |= DM9000_ior(DM9000_PIDL) << 16;
	id_val |= DM9000_ior(DM9000_PIDH) << 24;
	if (id_val == DM9000_ID) {
		print_format("dm9000 i/o: 0x%x, id: 0x%x \n\r", CONFIG_DM9000_BASE,
				id_val);
		return 0;
	} else {
		print_format("dm9000 not found at 0x%x id: 0x%x\n\r",
				CONFIG_DM9000_BASE, id_val);
		return -1;
	}
}


static void dm9000_start(void){
	/* Activate DM9000 */
	/* RX enable */
	DM9000_iow(DM9000_RCR, RCR_DIS_LONG | RCR_DIS_CRC | RCR_RXEN);
	/* Enable TX/RX interrupt mask */
	DM9000_iow(DM9000_IMR, IMR_PAR);
	/*Enable flow control*/
	//	DM9000_iow(DM9000_FCR, 0x1);
}


/* General Purpose dm9000 reset routine */
static void dm9000_reset(void)
{
	print_format("resetting DM9000\n\r");
//	(*(uint32_t*)SROMC_BW) |= ((1 << 0) | (1 << 2) | (1 << 3)) << 4; // switching to 16 bit bus on SROM bank 1 
       _raw_writel((0x1 << S5P_SROM_BCX__PMC__SHIFT) |
   		    (0x9 << S5P_SROM_BCX__TACP__SHIFT) |
                    (0xc << S5P_SROM_BCX__TCAH__SHIFT) |
                    (0x1 << S5P_SROM_BCX__TCOH__SHIFT) |
                    (0x6 << S5P_SROM_BCX__TACC__SHIFT) |
                    (0x1 << S5P_SROM_BCX__TCOS__SHIFT) |
                    (0x1 << S5P_SROM_BCX__TACS__SHIFT), 0xE8000008);
       //udelay(8000);
	/* Reset DM9000,
	 * 	   see DM9000 Application Notes V1.22 Jun 11, 2004 page 29 */

	/* DEBUG: Make all GPIO0 outputs, all others inputs */
	DM9000_iow(DM9000_GPCR, GPCR_GPIO0_OUT); //this is in 8-bit mode, why is it here ?? brain damaged documentation ('-_-)

	/* Step 1: Power internal PHY by writing 0 to GPIO0 pin */
	DM9000_iow(DM9000_GPR, 0);
	/* Step 2: Software reset */
	DM9000_iow(DM9000_NCR, (NCR_LBK_INT_MAC | NCR_RST));

	do {
#ifdef CONFIG_DM9000_DEBUG
		DM9000_DBG("resetting the DM9000, 1st reset\n\r");
#endif
		udelay(25); /* Wait at least 20 us */
	} while (DM9000_ior(DM9000_NCR) & 1);

	DM9000_iow(DM9000_NCR, 0);
	DM9000_iow(DM9000_NCR, (NCR_LBK_INT_MAC | NCR_RST)); /* Issue a second reset */

	do {
#ifdef CONFIG_DM9000_DEBUG
		DM9000_DBG("resetting the DM9000, 2nd reset\n\r");
#endif
		udelay(25); /* Wait at least 20 us */
	} while (DM9000_ior(DM9000_NCR) & 1);

	/* Check whether the ethernet controller is present */
	if ((DM9000_ior(DM9000_PIDL) != 0x0) ||
			(DM9000_ior(DM9000_PIDH) != 0x90)){
		print_format("ERROR: resetting DM9000 -> not responding \n\r(PIDH = 0x%x,PIDL = 0x%x)\n\r");
#ifdef CONFIG_DM9000_DEBUG
		dm9000_dump_regs();
#endif
	}
}

/* Initialize dm9000 board
 * */
//static int dm9000_init(struct eth_device *dev, bd_t *bd)
static int dm9000_init(struct eth_device *dev)
{
	int i, oft, lnk;
	uint8_t io_mode;
	struct board_info *db = &dm9000_info;

	//uart_print(__func__);
	print_format("Reseting ethernet device ...\n\r");
	/* RESET device */
	dm9000_reset();
	print_format("Done resetting device, probing ... \n\r");

	if (dm9000_probe() < 0){
		print_format("Failed to probe device!\n\r");
		return -1;
	}

	print_format("Device successfully probed ...\n\rdetecting bit mode ...\n\r");

	/* Auto-detect 8/16/32 bit mode, ISR Bit 6+7 indicate bus width */

	io_mode = DM9000_ior(DM9000_ISR) >> 6;

#ifdef CONFIG_DM9000_DEBUG
	dm9000_dump_regs();
#endif
	switch (io_mode) {
		case 0x0:  /* 16-bit mode */
			print_format("DM9000: running in 16 bit mode\n\r");
			db->outblk    = dm9000_outblk_16bit;
			db->inblk     = dm9000_inblk_16bit;
			db->rx_status = dm9000_rx_status_16bit;
			break;
		case 0x01:  /* 32-bit mode */
			print_format("DM9000: running in 32 bit mode\n\r");
			db->outblk    = dm9000_outblk_32bit;
			db->inblk     = dm9000_inblk_32bit;
			db->rx_status = dm9000_rx_status_32bit;
			break;
		case 0x02: /* 8 bit mode */
			print_format("DM9000: running in 8 bit mode\n\r");
			db->outblk    = dm9000_outblk_8bit;
			db->inblk     = dm9000_inblk_8bit;
			db->rx_status = dm9000_rx_status_8bit;
			break;
		default:
			/* Assume 8 bit mode, will probably not work anyway */
			print_format("DM9000: Undefined IO-mode:0x%x\n\r",io_mode);
			db->outblk    = dm9000_outblk_8bit;
			db->inblk     = dm9000_inblk_8bit;
			db->rx_status = dm9000_rx_status_8bit;
			break;
	}

	/* Program operating register, only internal phy supported */
	DM9000_iow(DM9000_NCR, 0x0);
	/* TX Polling clear */
	DM9000_iow(DM9000_TCR, 0);
	/* Less 3Kb, 200us */
	DM9000_iow(DM9000_BPTR, BPTR_BPHW(3) | BPTR_JPT_600US);
	/* Flow Control : High/Low Water */
	DM9000_iow(DM9000_FCTR, FCTR_HWOT(3) | FCTR_LWOT(8));
	/* SH FIXME: This looks strange! Flow Control */
	DM9000_iow(DM9000_FCR, 0x1);
	/* Special Mode */
	DM9000_iow(DM9000_SMCR, 0);
	/* clear TX status */
	DM9000_iow(DM9000_NSR, NSR_WAKEST | NSR_TX2END | NSR_TX1END);
	/* Clear interrupt status */
	DM9000_iow(DM9000_ISR, ISR_ROOS | ISR_ROS | ISR_PTS | ISR_PRS);
	print_format("MAC : [ ");	
	for(i = 0;i<6;i++)
		print_format(":%x:",(dev->enetaddr)[i]);
	print_format("]\n\r");	
	/* fill device MAC address registers */
	for (i = 0, oft = DM9000_PAR; i < 6; i++, oft++)
		DM9000_iow(oft, dev->enetaddr[i]);
	/*Multicast*/
	for (i = 0, oft = 0x16; i < 8; i++, oft++)
		DM9000_iow(oft, 0xff);

	/* read back mac, just to be sure */
	print_format("reading MAC from hardware ...\n\r");
	for (i = 0, oft = 0x10; i < 6; i++, oft++)
		DM9000_DBG("%x\n\r:", DM9000_ior(oft));

	/* read back multicast address, just to be sure*/
	print_format("reading multicast from hardware ...\n\r");
	for(i = 0,oft=0x16; i<8;i++,oft++){
#ifdef CONFIG_DM9000_DEBUG
		DM9000_DBG("%x\n\r:", DM9000_ior(oft));
#endif
	}

	/* Activate DM9000 */
	/* RX enable */
	print_format("Activating DM9000\n\r");
	DM9000_iow(DM9000_RCR, RCR_DIS_LONG | RCR_DIS_CRC | RCR_RXEN);
	/* Enable TX/RX interrupt mask */
	print_format("Enabling Interrupt ...\n\r");
	DM9000_iow(DM9000_IMR, IMR_PAR);

	i = 0;
	while (!(dm9000_phy_read(1) & 0x20)) {	/* autonegation complete bit */
		udelay(1000);
		i++;
		if (i == 10000) {
			print_format("could not establish link\n\r");
			return 0;
		}
	}

	/* see what we've got */
	lnk = dm9000_phy_read(17) >> 12;
	print_format("operating at ");
	switch (lnk) {
		case 1:
			print_format("10M half duplex ");
			break;
		case 2:
			print_format("10M full duplex ");
			break;
		case 4:
			print_format("100M half duplex ");
			break;
		case 8:
			print_format("100M full duplex ");
			break;
		default:
			print_format("unknown: %d ", lnk);
			break;
	}
	print_format("mode\n\r");
	return 0;
}

/*
 *   Hardware start transmission.
 *     Send a packet to media from the upper layer.
 *     */
static int dm9000_send(struct eth_device *netdev, volatile void *packet,
		int length)
{
	int tmo;
	struct board_info *db = &dm9000_info;

	//DM9000_DMP_PACKET(__func__ , packet, length);

	DM9000_iow(DM9000_ISR, IMR_PTM); /* Clear Tx bit in ISR */

	/* Move data to DM9000 TX RAM */
	DM9000_outb(DM9000_MWCMD, DM9000_IO); /* Prepare for TX-data */
#ifdef CONFIG_DM9000_DEBUG
	dm9000_dump_eth_frame(packet,length);
#endif

	/* push the data to the TX-fifo */
	(db->outblk)(packet, length);

	/* Set TX length to DM9000 */
	DM9000_iow(DM9000_TXPLL, length & 0xff);
	DM9000_iow(DM9000_TXPLH, (length >> 8) & 0xff);

	/* Issue TX polling command */
	DM9000_iow(DM9000_TCR, TCR_TXREQ); /* Cleared after TX complete */

	/* wait for end of transmission */
	tmo = get_timer(0) + 5 * CONFIG_SYS_HZ;
	while ( !(DM9000_ior(DM9000_NSR) & (NSR_TX1END | NSR_TX2END)) ||
			!(DM9000_ior(DM9000_ISR) & IMR_PTM) ) {
		// transmission will never timeout (get_timer stubbed) ...
		if (get_timer(0) >= tmo) {
			print_format("transmission timeout\n\r");
			break;
		}
	}
	DM9000_iow(DM9000_ISR, IMR_PTM); /* Clear Tx bit in ISR */
#ifdef CONFIG_DM9000_DEBUG
	DM9000_DBG("transmit done\n\r\n\r");
#endif
	return 0;
}

/*
 *   Stop the interface.
 *     The interface is stopped when it is brought.
 *     */
static void dm9000_halt(struct eth_device *netdev)
{
	//	uart_print(__func__);

	/* RESET devie */
	dm9000_phy_write(0, 0x8000);	/* PHY RESET */
	DM9000_iow(DM9000_GPR, 0x01);	/* Power-Down PHY */
	DM9000_iow(DM9000_IMR, 0x80);	/* Disable all interrupt */
	DM9000_iow(DM9000_RCR, 0x00);	/* Disable RX */
}

/*
 *   Received a packet and pass to upper layer
 *   */
static int dm9000_rx(struct eth_device *netdev)
{
	uint8_t rxbyte, *rdptr = (uint8_t *) net_rx_packets[0];
	uint16_t RxStatus=0, RxLen = 0;
	struct board_info *db = &dm9000_info;

	/* Check packet ready or not, we must check
	 * 	   the ISR status first for DM9000A */

	if (!(DM9000_ior(DM9000_ISR) & 0x01)){ /* Rx-ISR bit must be set. */
		return 0;
	}

	DM9000_iow(DM9000_ISR, 0x01); /* clear PR status latched in bit 0 */

	/* There is _at least_ 1 package in the fifo, read them all */
	for (;;) {
		DM9000_ior(DM9000_MRCMDX);	/* Dummy read */
		/* Get most updated data,
		 * 		   only look at bits 0:1, See application notes DM9000 */
		rxbyte = DM9000_inb(DM9000_DATA) & 0x03;
		/* Status check: this byte must be 0 or 1 */
		if (rxbyte > DM9000_PKT_RDY) {
			print_format("DM9000 error: status check fail: 0x%x\n\r",
					rxbyte);
			DM9000_iow(DM9000_RCR, 0x00);	/* Stop Device */
			DM9000_iow(DM9000_ISR, 0x80);	/* Stop INT request */
			// reset device ...
			dm9000_reset();
			dm9000_start();
			return 0;
		}

		if (rxbyte != DM9000_PKT_RDY){
#ifdef CONFIG_DM9000_DEBUG
			print_format("no packets received\n\r");
			dm9000_dump_regs();
#endif
			return 0; /* No packet received, ignore */
		}


#ifdef CONFIG_DM9000_DEBUG
		DM9000_DBG("receiving packet\n\r");
#endif
		//udelay(20);
		/* A packet ready now  & Get status/length */
		(db->rx_status)(&RxStatus, &RxLen);
		
#ifdef CONFIG_DM9000_DEBUG
		DM9000_DBG("rx status: 0x%x rx len: %d\n\r", RxStatus, RxLen);
#endif
		/* Move data from DM9000 */
		/* Read received packet from RX SRAM */
		(db->inblk)(rdptr, RxLen);
#ifdef CONFIG_DM9000_DEBUG
		DM9000_DBG("net_rx_packets filled ...\n\r");
#endif
		// a good RxStatus would look like: 0x4001
		// higher byte 0x40 is being anded with 0xBF, which should
		// produce 0, unless something is wrong ...
		if ((RxStatus & 0xbf00) || (RxLen < 0x40)
				|| (RxLen > DM9000_PKT_MAX)) {
			if (RxStatus & 0x100) {
				print_format("rx fifo error\n\r");
#ifdef CONFIG_DM9000_DEBUG
				dm9000_dump_regs();
				dm9000_dump_eth_frame(rdptr,RxLen);
#endif
			}
			if (RxStatus & 0x200) {
				print_format("rx crc error\n\r");
			}
			if (RxStatus & 0x8000) {
				print_format("rx length error\n\r");
			}
			if (RxLen > DM9000_PKT_MAX) {
				print_format("rx length too big\n\r");
				dm9000_reset();
				dm9000_start();
#ifdef CONFIG_DM9000_DEBUG
				dm9000_dump_regs();
#endif
			}
		} else {
			//	DM9000_DMP_PACKET(__func__ , rdptr, RxLen);
	//		DM9000_DBG("passing packet to upper layer\n\r");
//			dm9000_dump_eth_frame(rdptr,RxLen);
			net_process_received_packet(net_rx_packets[0], RxLen);
		//	return RxLen;
		}
	}
	return 0;
}

/*
 *   Read a word data from SROM
 *   */
#if !defined(CONFIG_DM9000_NO_SROM)
void dm9000_read_srom_word(int offset, uint8_t *to)
{
	DM9000_iow(DM9000_EPAR, offset);
	DM9000_iow(DM9000_EPCR, 0x4);
	udelay(8000);
	DM9000_iow(DM9000_EPCR, 0x0);
	to[0] = DM9000_ior(DM9000_EPDRL);
	to[1] = DM9000_ior(DM9000_EPDRH);
}

void dm9000_write_srom_word(int offset, uint16_t val)
{
	DM9000_iow(DM9000_EPAR, offset);
	DM9000_iow(DM9000_EPDRH, ((val >> 8) & 0xff));
	DM9000_iow(DM9000_EPDRL, (val & 0xff));
	DM9000_iow(DM9000_EPCR, 0x12);
	udelay(8000);
	DM9000_iow(DM9000_EPCR, 0);
}
#endif

static void dm9000_get_enetaddr(struct eth_device *dev)
{
#if !defined(CONFIG_DM9000_NO_SROM)
	int i;
	for (i = 0; i < 3; i++)
		dm9000_read_srom_word(i, dev->enetaddr + (2 * i));
	print_format("Our MAC address is:\t");
	char printableMAC[7];
	for(i = 0;i<6;i++)
		printableMAC[i] = (dev->enetaddr)[i];
	printableMAC[7] = '\0';
	print_format(printableMAC);
	print_format("\n\rMoving on ...\n\r");
#endif
}

/*
 *    Read a byte from I/O port
 *    */
static uint8_t DM9000_ior(int reg)
{
	DM9000_outb(reg, DM9000_IO);
	return (uint8_t) DM9000_inb(DM9000_DATA);

}

/*
 *    Write a byte to I/O port
 *    */
static void DM9000_iow(int reg, uint8_t value)
{
	DM9000_outb(reg, DM9000_IO);
	DM9000_outb(value, DM9000_DATA);
}

/*
 *    Read a word from phyxcer
 *    */
static uint16_t dm9000_phy_read(int reg)
{
	uint16_t val;

	/* Fill the phyxcer register into REG_0C */
	DM9000_iow(DM9000_EPAR, DM9000_PHY | reg);
	DM9000_iow(DM9000_EPCR, 0xc);	/* Issue phyxcer read command */
	udelay(100);			/* Wait read complete */
	DM9000_iow(DM9000_EPCR, 0x0);	/* Clear phyxcer read command */
	val = (DM9000_ior(DM9000_EPDRH) << 8) | DM9000_ior(DM9000_EPDRL);

	/* The read data keeps on REG_0D & REG_0E */
	print_format("dm9000_phy_read(0x%x): 0x%x\n\r", reg, val);
	return val;
}

/*
 *    Write a word to phyxcer
 *    */
static void dm9000_phy_write(int reg, uint16_t value)
{

	/* Fill the phyxcer register into REG_0C */
	DM9000_iow(DM9000_EPAR, DM9000_PHY | reg);

	/* Fill the written data into REG_0D & REG_0E */
	DM9000_iow(DM9000_EPDRL, (value & 0xff));
	DM9000_iow(DM9000_EPDRH, ((value >> 8) & 0xff));
	DM9000_iow(DM9000_EPCR, 0xa);	/* Issue phyxcer write command */
	udelay(500);			/* Wait write complete */
	DM9000_iow(DM9000_EPCR, 0x0);	/* Clear phyxcer write command */
	DM9000_DBG("dm9000_phy_write(reg:0x%x, value:0x%x)\n\r", reg, value);
}

//int dm9000_initialize(bd_t *bis)
int dm9000_initialize(void)
{
	int i;
	int regStat;
	struct eth_device *dev = &(dm9000_info.netdev);
	/*Fill MAC address ...*/	
//	print_format("SROM_BANK1 data bus: 0x%x\n\r",*(uint32_t*)SROMC_BW);
//	print_format("Setting bus width of SROM_BANK1 to 16 ...\n\r");
//	print_format("SROM_BANK1 data bus: 0x%x\n\r",*(uint32_t*)SROMC_BW);

	for(i = 0;i<6;i++)
		dev->enetaddr[i] = net_ethaddr[i];

	if (dm9000_init(dev) == 0){
		dev->init = dm9000_init;
		dev->halt = dm9000_halt;
		dev->send = dm9000_send;
		dev->recv = dm9000_rx;
		sprintf((char*)dev->name, "dm9000");
		dev->index = 1;
		regStat = eth_register(dev);

		switch(regStat){
			case 0:{
				       print_format("eth_current successfully registered\n\r");
				       return 0;
			       }
			case 1:{ 
				       print_format("dev was null, eth_current not registered\n\r");
				       return 1;
			       }
			case 2:{ 
				       print_format("dev name too long, eth_current not registered\n\r");
				       return 2;
			       }
			case 3:{ 
				       print_format("eth_current is null, eth_current not registered\n\r");// should not happen
				       return 3;
			       }
			default:
			       return 4;

		}
	}else{
		print_format("Failed to initialize dm9000\n\r");
		return -1;
	}

	//	dev->ops->start = dm9000_init;
	//	dev->ops->send  = dm9000_send;
	//	dev->ops->stop   = dm9000_halt;
	//	dev->ops->recv = dm9000_rx;

}
