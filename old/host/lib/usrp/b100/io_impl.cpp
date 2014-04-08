//
// Copyright 2011 Ettus Research LLC
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

#include "recv_packet_demuxer.hpp"
#include "validate_subdev_spec.hpp"
#include "../../transport/super_recv_packet_handler.hpp"
#include "../../transport/super_send_packet_handler.hpp"
#include "usrp_commands.h"
#include "b100_impl.hpp"
#include "b100_regs.hpp"
#include <uhd/utils/thread_priority.hpp>
#include <uhd/transport/bounded_buffer.hpp>
#include <boost/bind.hpp>
#include <boost/format.hpp>
#include <boost/bind.hpp>
#include <boost/thread.hpp>
#include <uhd/utils/msg.hpp>
#include <uhd/utils/log.hpp>
#include <boost/make_shared.hpp>

using namespace uhd;
using namespace uhd::usrp;
using namespace uhd::transport;

/***********************************************************************
 * IO Implementation Details
 **********************************************************************/
struct b100_impl::io_impl{
    io_impl(void):
        async_msg_fifo(100/*messages deep*/)
    { /* NOP */ }

    zero_copy_if::sptr data_transport;
    bounded_buffer<async_metadata_t> async_msg_fifo;
    recv_packet_demuxer::sptr demuxer;
};

/***********************************************************************
 * Initialize internals within this file
 **********************************************************************/
void b100_impl::io_init(void){

    //clear state machines
    _fpga_ctrl->poke32(B100_REG_CLEAR_RX, 0);
    _fpga_ctrl->poke32(B100_REG_CLEAR_TX, 0);

    //set the expected packet size in USB frames
    _fpga_ctrl->poke32(B100_REG_MISC_RX_LEN, 4);

    //allocate streamer weak ptrs containers
    _rx_streamers.resize(_rx_dsps.size());
    _tx_streamers.resize(1/*known to be 1 dsp*/);

    //create new io impl
    _io_impl = UHD_PIMPL_MAKE(io_impl, ());
    _io_impl->demuxer = recv_packet_demuxer::make(_data_transport, _rx_dsps.size(), B100_RX_SID_BASE);

    //now its safe to register the async callback
    _fpga_ctrl->set_async_cb(boost::bind(&b100_impl::handle_async_message, this, _1));
}

void b100_impl::handle_async_message(managed_recv_buffer::sptr rbuf){
    vrt::if_packet_info_t if_packet_info;
    if_packet_info.num_packet_words32 = rbuf->size()/sizeof(boost::uint32_t);
    const boost::uint32_t *vrt_hdr = rbuf->cast<const boost::uint32_t *>();
    try{
        vrt::if_hdr_unpack_le(vrt_hdr, if_packet_info);
    }
    catch(const std::exception &e){
        UHD_MSG(error) << "Error (handle_async_message): " << e.what() << std::endl;
    }

    if (if_packet_info.sid == B100_TX_ASYNC_SID and if_packet_info.packet_type != vrt::if_packet_info_t::PACKET_TYPE_DATA){
        //fill in the async metadata
        async_metadata_t metadata;
        metadata.channel = 0;
        metadata.has_time_spec = if_packet_info.has_tsi and if_packet_info.has_tsf;
        metadata.time_spec = time_spec_t(
            time_t(if_packet_info.tsi), size_t(if_packet_info.tsf), _clock_ctrl->get_fpga_clock_rate()
        );
        metadata.event_code = async_metadata_t::event_code_t(sph::get_context_code(vrt_hdr, if_packet_info));
        _io_impl->async_msg_fifo.push_with_pop_on_full(metadata);
        if (metadata.event_code &
            ( async_metadata_t::EVENT_CODE_UNDERFLOW
            | async_metadata_t::EVENT_CODE_UNDERFLOW_IN_PACKET)
        ) UHD_MSG(fastpath) << "U";
        else if (metadata.event_code &
            ( async_metadata_t::EVENT_CODE_SEQ_ERROR
            | async_metadata_t::EVENT_CODE_SEQ_ERROR_IN_BURST)
        ) UHD_MSG(fastpath) << "S";
        else if (metadata.event_code &
            async_metadata_t::EVENT_CODE_TIME_ERROR
        ) UHD_MSG(fastpath) << "L";
    }
    else UHD_MSG(error) << "Unknown async packet" << std::endl;
}

void b100_impl::update_rates(void){
    const fs_path mb_path = "/mboards/0";
    _tree->access<double>(mb_path / "tick_rate").update();

    //and now that the tick rate is set, init the host rates to something
    BOOST_FOREACH(const std::string &name, _tree->list(mb_path / "rx_dsps")){
        _tree->access<double>(mb_path / "rx_dsps" / name / "rate" / "value").update();
    }
    BOOST_FOREACH(const std::string &name, _tree->list(mb_path / "tx_dsps")){
        _tree->access<double>(mb_path / "tx_dsps" / name / "rate" / "value").update();
    }
}

void b100_impl::update_tick_rate(const double rate){
    //update the tick rate on all existing streamers -> thread safe
    for (size_t i = 0; i < _rx_streamers.size(); i++){
        boost::shared_ptr<sph::recv_packet_streamer> my_streamer =
            boost::dynamic_pointer_cast<sph::recv_packet_streamer>(_rx_streamers[i].lock());
        if (my_streamer.get() == NULL) continue;
        my_streamer->set_tick_rate(rate);
    }
    for (size_t i = 0; i < _tx_streamers.size(); i++){
        boost::shared_ptr<sph::send_packet_streamer> my_streamer =
            boost::dynamic_pointer_cast<sph::send_packet_streamer>(_tx_streamers[i].lock());
        if (my_streamer.get() == NULL) continue;
        my_streamer->set_tick_rate(rate);
    }
}

void b100_impl::update_rx_samp_rate(const size_t dspno, const double rate){
    boost::shared_ptr<sph::recv_packet_streamer> my_streamer =
        boost::dynamic_pointer_cast<sph::recv_packet_streamer>(_rx_streamers[dspno].lock());
    if (my_streamer.get() == NULL) return;

    my_streamer->set_samp_rate(rate);
    const double adj = _rx_dsps[dspno]->get_scaling_adjustment();
    my_streamer->set_scale_factor(adj);
}

void b100_impl::update_tx_samp_rate(const size_t dspno, const double rate){
    boost::shared_ptr<sph::send_packet_streamer> my_streamer =
        boost::dynamic_pointer_cast<sph::send_packet_streamer>(_tx_streamers[dspno].lock());
    if (my_streamer.get() == NULL) return;

    my_streamer->set_samp_rate(rate);
}

void b100_impl::update_rx_subdev_spec(const uhd::usrp::subdev_spec_t &spec){
    fs_path root = "/mboards/0/dboards";

    //sanity checking
    validate_subdev_spec(_tree, spec, "rx");

    //setup mux for this spec
    bool fe_swapped = false;
    for (size_t i = 0; i < spec.size(); i++){
        const std::string conn = _tree->access<std::string>(root / spec[i].db_name / "rx_frontends" / spec[i].sd_name / "connection").get();
        if (i == 0 and (conn == "QI" or conn == "Q")) fe_swapped = true;
        _rx_dsps[i]->set_mux(conn, fe_swapped);
    }
    _rx_fe->set_mux(fe_swapped);
}

void b100_impl::update_tx_subdev_spec(const uhd::usrp::subdev_spec_t &spec){
    fs_path root = "/mboards/0/dboards";

    //sanity checking
    validate_subdev_spec(_tree, spec, "tx");

    //set the mux for this spec
    const std::string conn = _tree->access<std::string>(root / spec[0].db_name / "tx_frontends" / spec[0].sd_name / "connection").get();
    _tx_fe->set_mux(conn);
}

/***********************************************************************
 * Async Data
 **********************************************************************/
bool b100_impl::recv_async_msg(
    async_metadata_t &async_metadata, double timeout
){
    boost::this_thread::disable_interruption di; //disable because the wait can throw
    return _io_impl->async_msg_fifo.pop_with_timed_wait(async_metadata, timeout);
}

/***********************************************************************
 * Receive streamer
 **********************************************************************/
rx_streamer::sptr b100_impl::get_rx_stream(const uhd::stream_args_t &args_){
    stream_args_t args = args_;

    //setup defaults for unspecified values
    args.otw_format = args.otw_format.empty()? "sc16" : args.otw_format;
    args.channels = args.channels.empty()? std::vector<size_t>(1, 0) : args.channels;
    const unsigned sc8_scalar = unsigned(args.args.cast<double>("scalar", 0x400));

    //calculate packet size
    static const size_t hdr_size = 0
        + vrt::max_if_hdr_words32*sizeof(boost::uint32_t)
        + sizeof(vrt::if_packet_info_t().tlr) //forced to have trailer
        - sizeof(vrt::if_packet_info_t().cid) //no class id ever used
    ;
    const size_t bpp = 2048 - hdr_size; //limited by FPGA pkt buffer size
    const size_t spp = bpp/convert::get_bytes_per_item(args.otw_format);

    //make the new streamer given the samples per packet
    boost::shared_ptr<sph::recv_packet_streamer> my_streamer = boost::make_shared<sph::recv_packet_streamer>(spp);

    //init some streamer stuff
    my_streamer->resize(args.channels.size());
    my_streamer->set_vrt_unpacker(&vrt::if_hdr_unpack_le);

    //set the converter
    uhd::convert::id_type id;
    id.input_format = args.otw_format + "_item32_le";
    id.num_inputs = 1;
    id.output_format = args.cpu_format;
    id.num_outputs = 1;
    my_streamer->set_converter(id);

    //bind callbacks for the handler
    for (size_t chan_i = 0; chan_i < args.channels.size(); chan_i++){
        const size_t dsp = args.channels[chan_i];
        _rx_dsps[dsp]->set_nsamps_per_packet(spp); //seems to be a good place to set this
        if (not args.args.has_key("noclear")) _rx_dsps[dsp]->clear();
        _rx_dsps[dsp]->set_format(args.otw_format, sc8_scalar);
        my_streamer->set_xport_chan_get_buff(chan_i, boost::bind(
            &recv_packet_demuxer::get_recv_buff, _io_impl->demuxer, dsp, _1
        ), true /*flush*/);
        my_streamer->set_overflow_handler(chan_i, boost::bind(
            &rx_dsp_core_200::handle_overflow, _rx_dsps[dsp]
        ));
        _rx_streamers[dsp] = my_streamer; //store weak pointer
    }

    //sets all tick and samp rates on this streamer
    this->update_rates();

    return my_streamer;
}

/***********************************************************************
 * Transmit streamer
 **********************************************************************/
tx_streamer::sptr b100_impl::get_tx_stream(const uhd::stream_args_t &args_){
    stream_args_t args = args_;

    //setup defaults for unspecified values
    args.otw_format = args.otw_format.empty()? "sc16" : args.otw_format;
    args.channels = args.channels.empty()? std::vector<size_t>(1, 0) : args.channels;

    if (args.otw_format != "sc16"){
        throw uhd::value_error("USRP TX cannot handle requested wire format: " + args.otw_format);
    }

    //calculate packet size
    static const size_t hdr_size = 0
        + vrt::max_if_hdr_words32*sizeof(boost::uint32_t)
        - sizeof(vrt::if_packet_info_t().cid) //no class id ever used
    ;
    static const size_t bpp = 2048 - hdr_size;
    const size_t spp = bpp/convert::get_bytes_per_item(args.otw_format);

    //make the new streamer given the samples per packet
    boost::shared_ptr<sph::send_packet_streamer> my_streamer = boost::make_shared<sph::send_packet_streamer>(spp);

    //init some streamer stuff
    my_streamer->resize(args.channels.size());
    my_streamer->set_vrt_packer(&vrt::if_hdr_pack_le);

    //set the converter
    uhd::convert::id_type id;
    id.input_format = args.cpu_format;
    id.num_inputs = 1;
    id.output_format = args.otw_format + "_item32_le";
    id.num_outputs = 1;
    my_streamer->set_converter(id);

    //bind callbacks for the handler
    for (size_t chan_i = 0; chan_i < args.channels.size(); chan_i++){
        const size_t dsp = args.channels[chan_i];
        UHD_ASSERT_THROW(dsp == 0); //always 0
        if (not args.args.has_key("noclear")) _tx_dsp->clear();
        if (args.args.has_key("underflow_policy")) _tx_dsp->set_underflow_policy(args.args["underflow_policy"]);
        my_streamer->set_xport_chan_get_buff(chan_i, boost::bind(
            &zero_copy_if::get_send_buff, _data_transport, _1
        ));
        _tx_streamers[dsp] = my_streamer; //store weak pointer
    }

    //sets all tick and samp rates on this streamer
    this->update_rates();

    return my_streamer;
}