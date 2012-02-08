//
// Copyright 2012 Fairwaves
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#ifndef LMS_DBOARD_IFACE_HPP
#define LMS_DBOARD_IFACE_HPP

#include <boost/assign/list_of.hpp>
#include <uhd/utils/log.hpp>                                                                                                    
#include <uhd/utils/msg.hpp>
#include <uhd/usrp/dboard_iface.hpp>
#include "usrp2_iface.hpp"

using namespace uhd;
using namespace uhd::usrp;
using namespace boost::assign;

dboard_iface::sptr make_lms_dboard_iface(usrp2_iface::sptr);

static const uhd::dict<dboard_iface::unit_t, int> unit_to_spi_dev = map_list_of 
    (dboard_iface::UNIT_TX, SPI_SS_TX_DB) 
    (dboard_iface::UNIT_RX, SPI_SS_RX_DB);


class lms_dboard_iface : public dboard_iface {

    usrp2_iface::sptr _iface;

public:
    lms_dboard_iface(usrp2_iface::sptr iface) { _iface = iface; };
    ~lms_dboard_iface(void) {}; //NOP

// LMS-specific functions
    void reg_dump(bool rise = true);
    void write_addr_data(uint8_t, uint8_t, uint8_t, bool rise = true);
    uint32_t read_addr(uint8_t lms, uint8_t addr, bool rise = true);
    uint32_t write_n_check(uint8_t, uint8_t, uint8_t, bool rise = true);

// functions from parent class
    void write_spi(unit_t unit, const spi_config_t &config, boost::uint32_t data, size_t num_bits) {
	_iface->write_spi(unit_to_spi_dev[unit], config, data, num_bits);
    }
    
    boost::uint32_t read_write_spi(unit_t unit, const spi_config_t &config, boost::uint32_t data, size_t num_bits) {
        return _iface->read_spi(unit_to_spi_dev[unit], config, data, num_bits);
    }

    special_props_t get_special_props(void) {
        special_props_t props;
        props.soft_clock_divider = false;
        props.mangle_i2c_addrs = false;
        return props;
    }

// dummy functions to make compiler happy
    void write_aux_dac(unit_t, aux_dac_t, double) { UHD_LOGV(always) << "FIXME: Not Implemented."; };
    double read_aux_adc(unit_t, aux_adc_t) { UHD_LOGV(always) << "FIXME: Not Implemented."; return 0; };

    void _set_pin_ctrl(unit_t, boost::uint16_t) { UHD_LOGV(always) << "FIXME: Not Implemented."; };
    void _set_atr_reg(unit_t, atr_reg_t, boost::uint16_t) { UHD_LOGV(always) << "FIXME: Not Implemented."; };
    void _set_gpio_ddr(unit_t, boost::uint16_t) { UHD_LOGV(always) << "FIXME: Not Implemented."; };
    void _set_gpio_out(unit_t, boost::uint16_t) { UHD_LOGV(always) << "FIXME: Not Implemented."; };

    void set_gpio_debug(unit_t, int) { UHD_LOGV(always) << "FIXME: Not Implemented."; };
    boost::uint16_t read_gpio(unit_t) { UHD_LOGV(always) << "FIXME: Not Implemented."; return 0; };
    void write_i2c(boost::uint8_t, const byte_vector_t &) { UHD_LOGV(always) << "FIXME: Not Implemented."; };
    byte_vector_t read_i2c(boost::uint8_t, size_t) { byte_vector_t FIXME; UHD_LOGV(always) << "FIXME: Not Implemented."; return FIXME; };
    void set_clock_rate(unit_t, double) { UHD_LOGV(always) << "FIXME: Not Implemented."; };
    double get_clock_rate(unit_t) { UHD_LOGV(always) << "FIXME: Not Implemented."; return 0; };
    std::vector<double> get_clock_rates(unit_t) { std::vector<double> FIXME; UHD_LOGV(always) << "FIXME: Not Implemented."; return FIXME; };
    void set_clock_enabled(unit_t, bool) { UHD_LOGV(always) << "FIXME: Not Implemented."; };
    double get_codec_rate(unit_t) { UHD_LOGV(always) << "FIXME: Not Implemented."; return 0; };
};

#endif 