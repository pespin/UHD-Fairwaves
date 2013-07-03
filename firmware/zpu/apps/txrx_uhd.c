/*
 * Copyright 2010-2011 Ettus Research LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

//peripheral headers
#include "u2_init.h"
#include "umtrx_init.h"
#include "spi.h"
#include "i2c.h"
#include "hal_io.h"
#include "pic.h"
#ifdef UMTRX
#  include "gpsdo.h"
#endif

//printf headers
#include "nonstdio.h"

//network headers
#include "arp_cache.h"
#include "ethernet.h"
#include "net_common.h"
#include "usrp2/fw_common.h"
#include "udp_fw_update.h"
#include "pkt_ctrl.h"
#include "udp_uart.h"

//standard headers
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#ifdef BOOTLOADER
#include <bootloader_utils.h>
#endif

//virtual registers in the firmware to store persistent values
static uint32_t fw_regs[8];

static void handle_udp_data_packet(
    struct socket_address src, struct socket_address dst,
    unsigned char *payload, int payload_len
){
    //handle ICMP destination unreachable
    if (payload == NULL) switch(src.port){
    case USRP2_UDP_RX_DSP0_PORT:
        //the end continuous streaming command
        sr_rx_ctrl0->cmd = 1 << 31 | 1 << 28; //no samples now
        sr_rx_ctrl0->time_secs = 0;
        sr_rx_ctrl0->time_ticks = 0; //latch the command
        break;

    case USRP2_UDP_RX_DSP1_PORT:
        //the end continuous streaming command
        sr_rx_ctrl1->cmd = 1 << 31 | 1 << 28; //no samples now
        sr_rx_ctrl1->time_secs = 0;
        sr_rx_ctrl1->time_ticks = 0; //latch the command
        break;

    case USRP2_UDP_RX_DSP0_2_PORT:
        //the end continuous streaming command
        sr_rx_ctrl0_2->cmd = 1 << 31 | 1 << 28; //no samples now
        sr_rx_ctrl0_2->time_secs = 0;
        sr_rx_ctrl0_2->time_ticks = 0; //latch the command
        break;

    case USRP2_UDP_RX_DSP1_2_PORT:
        //the end continuous streaming command
        sr_rx_ctrl1_2->cmd = 1 << 31 | 1 << 28; //no samples now
        sr_rx_ctrl1_2->time_secs = 0;
        sr_rx_ctrl1_2->time_ticks = 0; //latch the command
        break;

    default: return;
    }

    //handle an incoming UDP packet
    size_t which = 0;
    if (payload != 0) switch(dst.port){
    case USRP2_UDP_RX_DSP0_PORT:
        which = 0;
        break;

    case USRP2_UDP_RX_DSP1_PORT:
        which = 2;
        break;

    case USRP2_UDP_RX_DSP0_2_PORT:
        which = 1;
        break;

    case USRP2_UDP_RX_DSP1_2_PORT:
        which = 3;
        break;

    default: return;
    }

    eth_mac_addr_t eth_mac_host; arp_cache_lookup_mac(&src.addr, &eth_mac_host);
    setup_framer(eth_mac_host, *ethernet_mac_addr(), src, dst, which);
}

#define OTW_GPIO_BANK_TO_NUM(bank) \
    (((bank) == USRP2_DIR_RX)? (GPIO_RX_BANK) : (GPIO_TX_BANK))

static void handle_udp_ctrl_packet(
    struct socket_address src, struct socket_address dst,
    unsigned char *payload, int payload_len
){
    //printf("Got ctrl packet #words: %d\n", (int)payload_len);
    const usrp2_ctrl_data_t *ctrl_data_in = (usrp2_ctrl_data_t *)payload;
    uint32_t ctrl_data_in_id = ctrl_data_in->id;

    //ensure that the protocol versions match
    if (payload_len >= sizeof(uint32_t) && ctrl_data_in->proto_ver != USRP2_FW_COMPAT_NUM){
        if (ctrl_data_in->proto_ver) printf("!Error in control packet handler: Expected compatibility number %d, but got %d\n",
            USRP2_FW_COMPAT_NUM, ctrl_data_in->proto_ver
        );
#ifdef UMTRX
        ctrl_data_in_id = UMTRX_CTRL_ID_REQUEST;
#else
        ctrl_data_in_id = USRP2_CTRL_ID_WAZZUP_BRO;
#endif
    }

    //ensure that this is not a short packet
    if (payload_len < sizeof(usrp2_ctrl_data_t)){
        printf("!Error in control packet handler: Expected payload length %d, but got %d\n",
            (int)sizeof(usrp2_ctrl_data_t), payload_len
        );
        ctrl_data_in_id = USRP2_CTRL_ID_HUH_WHAT;
    }

    //setup the output data
    usrp2_ctrl_data_t ctrl_data_out;
    ctrl_data_out.proto_ver = USRP2_FW_COMPAT_NUM;
    ctrl_data_out.id=USRP2_CTRL_ID_HUH_WHAT;
    ctrl_data_out.seq=ctrl_data_in->seq;

    //handle the data based on the id
    switch(ctrl_data_in_id){

    /*******************************************************************
     * Addressing
     ******************************************************************/
#ifdef UMTRX
    case UMTRX_CTRL_ID_REQUEST:
        ctrl_data_out.id = UMTRX_CTRL_ID_RESPONSE;
        memcpy(&ctrl_data_out.data.ip_addr, get_ip_addr(), sizeof(struct ip_addr));
        break;
#else
    case USRP2_CTRL_ID_WAZZUP_BRO:
        ctrl_data_out.id = USRP2_CTRL_ID_WAZZUP_DUDE;
        memcpy(&ctrl_data_out.data.ip_addr, get_ip_addr(), sizeof(struct ip_addr));
        break;
#endif

    /*******************************************************************
     * ZPU actions
     ******************************************************************/
#ifdef UMTRX
    case UMTRX_CTRL_ID_ZPU_REQUEST:{
            ctrl_data_out.id = UMTRX_CTRL_ID_ZPU_RESPONSE;
            ctrl_data_out.data.zpu_action.action = ctrl_data_in->data.zpu_action.action;
            switch (ctrl_data_in->data.zpu_action.action) {
            case UMTRX_ZPU_REQUEST_GET_VCTCXO_DAC:
                ctrl_data_out.data.zpu_action.data = (uint32_t)get_vctcxo_dac();
                break;
            case UMTRX_ZPU_REQUEST_SET_VCTCXO_DAC:
                set_vctcxo_dac((uint16_t)ctrl_data_in->data.zpu_action.data);
                break;
            }
        }
        break;
#endif

#ifndef NO_SPI_I2C
    /*******************************************************************
     * SPI
     ******************************************************************/
    case USRP2_CTRL_ID_TRANSACT_ME_SOME_SPI_BRO:{
            //transact
            uint32_t result = spi_transact(
                (ctrl_data_in->data.spi_args.readback == 0)? SPI_TXONLY : SPI_TXRX,
                ctrl_data_in->data.spi_args.dev,      //which device
                ctrl_data_in->data.spi_args.data,     //32 bit data
                ctrl_data_in->data.spi_args.num_bits, //length in bits
                (ctrl_data_in->data.spi_args.mosi_edge == USRP2_CLK_EDGE_RISE)? SPIF_PUSH_FALL : SPIF_PUSH_RISE |
                (ctrl_data_in->data.spi_args.miso_edge == USRP2_CLK_EDGE_RISE)? SPIF_LATCH_RISE : SPIF_LATCH_FALL
            );

            //load output
            ctrl_data_out.data.spi_args.data = result;
            ctrl_data_out.id = USRP2_CTRL_ID_OMG_TRANSACTED_SPI_DUDE;
        }
        break;

    /*******************************************************************
     * I2C
     ******************************************************************/
    case USRP2_CTRL_ID_DO_AN_I2C_READ_FOR_ME_BRO:{
            uint8_t num_bytes = ctrl_data_in->data.i2c_args.bytes;
            i2c_read(
                ctrl_data_in->data.i2c_args.addr,
                ctrl_data_out.data.i2c_args.data,
                num_bytes
            );
            ctrl_data_out.id = USRP2_CTRL_ID_HERES_THE_I2C_DATA_DUDE;
            ctrl_data_out.data.i2c_args.bytes = num_bytes;
        }
        break;

    case USRP2_CTRL_ID_WRITE_THESE_I2C_VALUES_BRO:{
            uint8_t num_bytes = ctrl_data_in->data.i2c_args.bytes;
            i2c_write(
                ctrl_data_in->data.i2c_args.addr,
                ctrl_data_in->data.i2c_args.data,
                num_bytes
            );
            ctrl_data_out.id = USRP2_CTRL_ID_COOL_IM_DONE_I2C_WRITE_DUDE;
            ctrl_data_out.data.i2c_args.bytes = num_bytes;
        }
        break;
#endif

    /*******************************************************************
     * Peek and Poke Register
     ******************************************************************/
    case USRP2_CTRL_ID_GET_THIS_REGISTER_FOR_ME_BRO:
        switch(ctrl_data_in->data.reg_args.action){
            case USRP2_REG_ACTION_FPGA_PEEK32:
                ctrl_data_out.data.reg_args.data = *((uint32_t *) ctrl_data_in->data.reg_args.addr);
                break;

            case USRP2_REG_ACTION_FPGA_PEEK16:
                ctrl_data_out.data.reg_args.data = *((uint16_t *) ctrl_data_in->data.reg_args.addr);
                break;

            case USRP2_REG_ACTION_FPGA_POKE32:
                *((uint32_t *) ctrl_data_in->data.reg_args.addr) = (uint32_t)ctrl_data_in->data.reg_args.data;
                break;

            case USRP2_REG_ACTION_FPGA_POKE16:
                *((uint16_t *) ctrl_data_in->data.reg_args.addr) = (uint16_t)ctrl_data_in->data.reg_args.data;
                break;

            case USRP2_REG_ACTION_FW_PEEK32:
                ctrl_data_out.data.reg_args.data = fw_regs[(ctrl_data_in->data.reg_args.addr)];
                break;

            case USRP2_REG_ACTION_FW_POKE32:
                fw_regs[(ctrl_data_in->data.reg_args.addr)] = ctrl_data_in->data.reg_args.data;
                break;

        }
        ctrl_data_out.id = USRP2_CTRL_ID_OMG_GOT_REGISTER_SO_BAD_DUDE;
        break;

    /*******************************************************************
     * Echo test
     ******************************************************************/
    case USRP2_CTRL_ID_HOLLER_AT_ME_BRO:
        ctrl_data_out.data.echo_args.len = payload_len;
        ctrl_data_out.id = USRP2_CTRL_ID_HOLLER_BACK_DUDE;
        send_udp_pkt(USRP2_UDP_CTRL_PORT, src, &ctrl_data_out, ctrl_data_in->data.echo_args.len);
        return;

    default:
        ctrl_data_out.id = USRP2_CTRL_ID_HUH_WHAT;
    }
    send_udp_pkt(USRP2_UDP_CTRL_PORT, src, &ctrl_data_out, sizeof(ctrl_data_out));
}

#include <net/padded_eth_hdr.h>
static void handle_inp_packet(uint32_t *buff, size_t num_lines){

  //test if its an ip recovery packet
  typedef struct{
      padded_eth_hdr_t eth_hdr;
      char code[4];
      union {
        struct ip_addr ip_addr;
      } data;
  }recovery_packet_t;
  recovery_packet_t *recovery_packet = (recovery_packet_t *)buff;
  if (recovery_packet->eth_hdr.ethertype == 0xbeee && strncmp(recovery_packet->code, "addr", 4) == 0){
      printf("Got ip recovery packet: "); print_ip_addr(&recovery_packet->data.ip_addr); newline();
      set_ip_addr(&recovery_packet->data.ip_addr);
      return;
  }

  //pass it to the slow-path handler
  handle_eth_packet(buff, num_lines);
}

//------------------------------------------------------------------

/*
 * Called when eth phy state changes (w/ interrupts disabled)
 */
void link_changed_callback(int speed){
    printf("\neth link changed: speed = %d\n", speed);
    if (speed != 0){
        char led = speed==1000?LED_RJ45_ORANGE:LED_RJ45_GREEN;
        hal_set_leds(led, led);
        pkt_ctrl_set_routing_mode(PKT_CTRL_ROUTING_MODE_MASTER);
        send_gratuitous_arp();
    }
    else{
        hal_set_leds(0x0, LED_RJ45_ORANGE);
        hal_set_leds(0x0, LED_RJ45_GREEN);
        pkt_ctrl_set_routing_mode(PKT_CTRL_ROUTING_MODE_SLAVE);
    }
}

int
main(void)
{
  u2_init();
#ifdef BOOTLOADER
  putstr("\nUSRP N210 UDP bootloader\n");
#else
  putstr("\nTxRx-UHD-ZPU\n");
#endif
  printf("FPGA compatibility number: %d\n", USRP2_FPGA_COMPAT_NUM);
  printf("Firmware compatibility number: %d\n", USRP2_FW_COMPAT_NUM);

  //init readback for firmware minor version number
  fw_regs[U2_FW_REG_VER_MINOR] = USRP2_FW_VER_MINOR;

#ifdef BOOTLOADER
  //load the production FPGA image or firmware if appropriate
  do_the_bootload_thing();
  //if we get here we've fallen through to safe firmware
  set_default_mac_addr();
  set_default_ip_addr();
#endif

#ifdef UMTRX
  umtrx_init();
#endif

  print_mac_addr(ethernet_mac_addr()); newline();
  print_ip_addr(get_ip_addr()); newline();

  //1) register the addresses into the network stack
  register_addrs(ethernet_mac_addr(), get_ip_addr());
  pkt_ctrl_program_inspector(get_ip_addr());

  //2) register callbacks for udp ports we service
  init_udp_listeners();
  register_udp_listener(USRP2_UDP_CTRL_PORT, handle_udp_ctrl_packet);
  register_udp_listener(USRP2_UDP_RX_DSP0_PORT, handle_udp_data_packet);
  register_udp_listener(USRP2_UDP_RX_DSP1_PORT, handle_udp_data_packet);
  register_udp_listener(USRP2_UDP_RX_DSP0_2_PORT, handle_udp_data_packet);
  register_udp_listener(USRP2_UDP_RX_DSP1_2_PORT, handle_udp_data_packet);
  
#ifdef USRP2P
#ifndef NO_FLASH
  register_udp_listener(USRP2_UDP_UPDATE_PORT, handle_udp_fw_update_packet);
#endif
#endif

  udp_uart_init(USRP2_UDP_UART_BASE_PORT); //setup uart messaging

  //3) set the routing mode to slave to set defaults
  pkt_ctrl_set_routing_mode(PKT_CTRL_ROUTING_MODE_SLAVE);

  //4) setup ethernet hardware to bring the link up
  ethernet_register_link_changed_callback(link_changed_callback);
  ethernet_init();

  while(true){

    size_t num_lines;
    void *buff = pkt_ctrl_claim_incoming_buffer(&num_lines);
    if (buff != NULL){
        handle_inp_packet((uint32_t *)buff, num_lines);
        pkt_ctrl_release_incoming_buffer();
    }

    udp_uart_poll(); //uart message handling

    pic_interrupt_handler();
    /*
    int pending = pic_regs->pending;		// poll for under or overrun

    if (pending & PIC_UNDERRUN_INT){
      pic_regs->pending = PIC_UNDERRUN_INT;	// clear interrupt
      putchar('U');
    }

    if (pending & PIC_OVERRUN_INT){
      pic_regs->pending = PIC_OVERRUN_INT;	// clear interrupt
      putchar('O');
    }
    */
  }
}
