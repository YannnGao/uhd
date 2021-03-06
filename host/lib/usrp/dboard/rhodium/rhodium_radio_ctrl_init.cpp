//
// Copyright 2018 Ettus Research, a National Instruments Company
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "rhodium_radio_ctrl_impl.hpp"
#include "rhodium_constants.hpp"
#include <uhdlib/usrp/cores/spi_core_3000.hpp>
#include <uhd/utils/log.hpp>
#include <uhd/utils/algorithm.hpp>
#include <uhd/types/eeprom.hpp>
#include <uhd/types/sensors.hpp>
#include <uhd/transport/chdr.hpp>
#include <vector>
#include <string>

using namespace uhd;
using namespace uhd::rfnoc;

namespace {
    enum slave_select_t {
        SEN_CPLD = 8,
        SEN_TX_LO = 1,
        SEN_RX_LO = 2,
        SEN_LO_DIST = 4 /* Unused */
    };

    constexpr uint32_t TX_FE_BASE = 224;
    constexpr uint32_t RX_FE_BASE = 232;

    constexpr double RHODIUM_DEFAULT_FREQ         = 2.5e9; // Hz
    // An invalid default index ensures that set gain will apply settings
    // the first time it is called
    constexpr double RHODIUM_DEFAULT_INVALID_GAIN = -1; // gain index
    constexpr double RHODIUM_DEFAULT_GAIN         = 0;  // gain index
    constexpr double RHODIUM_DEFAULT_LO_GAIN      = 30; // gain index
    constexpr char   RHODIUM_DEFAULT_RX_ANTENNA[] = "RX2";
    constexpr char   RHODIUM_DEFAULT_TX_ANTENNA[] = "TX/RX";
    constexpr double RHODIUM_DEFAULT_BANDWIDTH    = 250e6; // Hz

    //! Rhodium gain profile options
    const std::vector<std::string> RHODIUM_GP_OPTIONS = {
        "default"
    };

    //! Returns the SPI config used by the CPLD
    spi_config_t _get_cpld_spi_config() {
        spi_config_t spi_config;
        spi_config.use_custom_divider = true;
        spi_config.divider = 10;
        spi_config.mosi_edge = spi_config_t::EDGE_RISE;
        spi_config.miso_edge = spi_config_t::EDGE_FALL;

        return spi_config;
    }

    //! Returns the SPI config used by the TX LO
    spi_config_t _get_tx_lo_spi_config() {
        spi_config_t spi_config;
        spi_config.use_custom_divider = true;
        spi_config.divider = 10;
        spi_config.mosi_edge = spi_config_t::EDGE_RISE;
        spi_config.miso_edge = spi_config_t::EDGE_FALL;

        return spi_config;
    }

    //! Returns the SPI config used by the RX LO
    spi_config_t _get_rx_lo_spi_config() {
        spi_config_t spi_config;
        spi_config.use_custom_divider = true;
        spi_config.divider = 10;
        spi_config.mosi_edge = spi_config_t::EDGE_RISE;
        spi_config.miso_edge = spi_config_t::EDGE_FALL;

        return spi_config;
    }

    std::function<void(uint32_t)> _generate_write_spi(
        uhd::spi_iface::sptr spi,
        slave_select_t slave,
        spi_config_t config
    ) {
        return [spi, slave, config](const uint32_t transaction) {
            spi->write_spi(slave, config, transaction, 24);
        };
    }

    std::function<uint32_t(uint32_t)> _generate_read_spi(
        uhd::spi_iface::sptr spi,
        slave_select_t slave,
        spi_config_t config
    ) {
        return [spi, slave, config](const uint32_t transaction) {
            return spi->read_spi(slave, config, transaction, 24);
        };
    }
}

void rhodium_radio_ctrl_impl::_init_defaults()
{
    UHD_LOG_TRACE(unique_id(), "Initializing defaults...");
    const size_t num_rx_chans = get_output_ports().size();
    const size_t num_tx_chans = get_input_ports().size();

    UHD_LOG_TRACE(unique_id(),
            "Num TX chans: " << num_tx_chans
            << " Num RX chans: " << num_rx_chans);

    for (size_t chan = 0; chan < num_rx_chans; chan++) {
        radio_ctrl_impl::set_rx_frequency(RHODIUM_DEFAULT_FREQ, chan);
        radio_ctrl_impl::set_rx_gain(RHODIUM_DEFAULT_INVALID_GAIN, chan);
        radio_ctrl_impl::set_rx_antenna(RHODIUM_DEFAULT_RX_ANTENNA, chan);
        radio_ctrl_impl::set_rx_bandwidth(RHODIUM_DEFAULT_BANDWIDTH, chan);
    }

    for (size_t chan = 0; chan < num_tx_chans; chan++) {
        radio_ctrl_impl::set_tx_frequency(RHODIUM_DEFAULT_FREQ, chan);
        radio_ctrl_impl::set_tx_gain(RHODIUM_DEFAULT_INVALID_GAIN, chan);
        radio_ctrl_impl::set_tx_antenna(RHODIUM_DEFAULT_TX_ANTENNA, chan);
        radio_ctrl_impl::set_rx_bandwidth(RHODIUM_DEFAULT_BANDWIDTH, chan);
    }

    /** Update default SPP (overwrites the default value from the XML file) **/
    const size_t max_bytes_header =
        uhd::transport::vrt::chdr::max_if_hdr_words64 * sizeof(uint64_t);
    const size_t default_spp =
        (_tree->access<size_t>("mtu/recv").get() - max_bytes_header)
        / (2 * sizeof(int16_t));
    UHD_LOG_DEBUG(unique_id(),
        "Setting default spp to " << default_spp);
    _tree->access<int>(get_arg_path("spp") / "value").set(default_spp);
}

void rhodium_radio_ctrl_impl::_init_peripherals()
{
    UHD_LOG_TRACE(unique_id(), "Initializing peripherals...");

    UHD_LOG_TRACE(unique_id(), "Initializing SPI core...");
    _spi = spi_core_3000::make(_get_ctrl(0),
        regs::sr_addr(regs::SPI),
        regs::rb_addr(regs::RB_SPI)
    );

    UHD_LOG_TRACE(unique_id(), "Initializing CPLD...");
    _cpld = std::make_shared<rhodium_cpld_ctrl>(
        _generate_write_spi(this->_spi, SEN_CPLD, _get_cpld_spi_config()),
        _generate_read_spi(this->_spi, SEN_CPLD, _get_cpld_spi_config()));

    UHD_LOG_TRACE(unique_id(), "Writing initial gain values...");
    set_tx_gain(RHODIUM_DEFAULT_GAIN, 0);
    set_tx_lo_gain(RHODIUM_DEFAULT_LO_GAIN, RHODIUM_LO1, 0);
    set_rx_gain(RHODIUM_DEFAULT_GAIN, 0);
    set_rx_lo_gain(RHODIUM_DEFAULT_LO_GAIN, RHODIUM_LO1, 0);

    UHD_LOG_TRACE(unique_id(), "Initializing TX LO...");
    _tx_lo = lmx2592_iface::make(
        _generate_write_spi(this->_spi, SEN_TX_LO, _get_tx_lo_spi_config()),
        _generate_read_spi(this->_spi, SEN_TX_LO, _get_tx_lo_spi_config()));

    UHD_LOG_TRACE(unique_id(), "Writing initial TX LO state...");
    _tx_lo->set_reference_frequency(RHODIUM_LO1_REF_FREQ);
    _tx_lo->set_mash_order(lmx2592_iface::mash_order_t::THIRD);

    UHD_LOG_TRACE(unique_id(), "Initializing RX LO...");
    _rx_lo = lmx2592_iface::make(
        _generate_write_spi(this->_spi, SEN_RX_LO, _get_rx_lo_spi_config()),
        _generate_read_spi(this->_spi, SEN_RX_LO, _get_rx_lo_spi_config()));

    UHD_LOG_TRACE(unique_id(), "Writing initial RX LO state...");
    _rx_lo->set_reference_frequency(RHODIUM_LO1_REF_FREQ);
    _rx_lo->set_mash_order(lmx2592_iface::mash_order_t::THIRD);

    UHD_LOG_TRACE(unique_id(), "Initializing GPIOs...");
    _gpio =
        usrp::gpio_atr::gpio_atr_3000::make(
            _get_ctrl(0),
            regs::sr_addr(regs::GPIO),
            regs::rb_addr(regs::RB_DB_GPIO)
        );
    _gpio->set_atr_mode(
        usrp::gpio_atr::MODE_GPIO, // Disable ATR mode
        usrp::gpio_atr::gpio_atr_3000::MASK_SET_ALL
    );
    _gpio->set_gpio_ddr(
        usrp::gpio_atr::DDR_OUTPUT, // Make all GPIOs outputs
        usrp::gpio_atr::gpio_atr_3000::MASK_SET_ALL
    );

    // TODO: put this in the right spot
    UHD_LOG_TRACE(unique_id(), "Setting Switch 10 to 0x1");
    _gpio->set_gpio_out(0x1, 0x3);

    _rx_fe_core = rx_frontend_core_3000::make(_get_ctrl(0), regs::sr_addr(RX_FE_BASE));
    _rx_fe_core->set_adc_rate(_master_clock_rate);
    _rx_fe_core->set_dc_offset(rx_frontend_core_3000::DEFAULT_DC_OFFSET_VALUE);
    _rx_fe_core->set_dc_offset_auto(rx_frontend_core_3000::DEFAULT_DC_OFFSET_ENABLE);
    _rx_fe_core->populate_subtree(_tree->subtree(_root_path / "rx_fe_corrections" / 0));

    _tx_fe_core = tx_frontend_core_200::make(_get_ctrl(0), regs::sr_addr(TX_FE_BASE));
    _tx_fe_core->set_dc_offset(tx_frontend_core_200::DEFAULT_DC_OFFSET_VALUE);
    _tx_fe_core->set_iq_balance(tx_frontend_core_200::DEFAULT_IQ_BALANCE_VALUE);
    _tx_fe_core->populate_subtree(_tree->subtree(_root_path / "tx_fe_corrections" / 0));
}

void rhodium_radio_ctrl_impl::_init_frontend_subtree(
    uhd::property_tree::sptr subtree,
    const size_t chan_idx
) {
    const fs_path tx_fe_path = fs_path("tx_frontends") / chan_idx;
    const fs_path rx_fe_path = fs_path("rx_frontends") / chan_idx;
    UHD_LOG_TRACE(unique_id(),
        "Adding non-RFNoC block properties for channel " << chan_idx <<
        " to prop tree path " << tx_fe_path << " and " << rx_fe_path);
    // TX Standard attributes
    subtree->create<std::string>(tx_fe_path / "name")
        .set(str(boost::format("Rhodium")))
    ;
    subtree->create<std::string>(tx_fe_path / "connection")
        .add_coerced_subscriber([this](const std::string& conn){
            this->_set_tx_fe_connection(conn);
        })
        .set_publisher([this](){
            return this->_get_tx_fe_connection();
        })
    ;
    subtree->create<device_addr_t>(tx_fe_path / "tune_args")
        .set(device_addr_t())
    ;
    // RX Standard attributes
    subtree->create<std::string>(rx_fe_path / "name")
        .set(str(boost::format("Rhodium")))
    ;
    subtree->create<std::string>(rx_fe_path / "connection")
        .add_coerced_subscriber([this](const std::string& conn){
            this->_set_rx_fe_connection(conn);
        })
        .set_publisher([this](){
            return this->_get_rx_fe_connection();
        })
    ;
    subtree->create<device_addr_t>(rx_fe_path / "tune_args")
        .set(device_addr_t())
    ;
    // TX Antenna
    subtree->create<std::string>(tx_fe_path / "antenna" / "value")
        .add_coerced_subscriber([this, chan_idx](const std::string &ant){
            this->set_tx_antenna(ant, chan_idx);
        })
        .set_publisher([this, chan_idx](){
            return this->get_tx_antenna(chan_idx);
        })
    ;
    subtree->create<std::vector<std::string>>(tx_fe_path / "antenna" / "options")
        .set({RHODIUM_DEFAULT_TX_ANTENNA})
        .add_coerced_subscriber([](const std::vector<std::string> &){
            throw uhd::runtime_error(
                    "Attempting to update antenna options!");
        })
    ;
    // RX Antenna
    subtree->create<std::string>(rx_fe_path / "antenna" / "value")
        .add_coerced_subscriber([this, chan_idx](const std::string &ant){
            this->set_rx_antenna(ant, chan_idx);
        })
        .set_publisher([this, chan_idx](){
            return this->get_rx_antenna(chan_idx);
        })
    ;
    subtree->create<std::vector<std::string>>(rx_fe_path / "antenna" / "options")
        .set(RHODIUM_RX_ANTENNAS)
        .add_coerced_subscriber([](const std::vector<std::string> &){
            throw uhd::runtime_error(
                "Attempting to update antenna options!");
        })
    ;
    // TX frequency
    subtree->create<double>(tx_fe_path / "freq" / "value")
        .set_coercer([this, chan_idx](const double freq){
            return this->set_tx_frequency(freq, chan_idx);
        })
        .set_publisher([this, chan_idx](){
            return this->get_tx_frequency(chan_idx);
        })
    ;
    subtree->create<meta_range_t>(tx_fe_path / "freq" / "range")
        .set(meta_range_t(RHODIUM_MIN_FREQ, RHODIUM_MAX_FREQ, 1.0))
        .add_coerced_subscriber([](const meta_range_t &){
            throw uhd::runtime_error(
                "Attempting to update freq range!");
        })
    ;
    // RX frequency
    subtree->create<double>(rx_fe_path / "freq" / "value")
        .set_coercer([this, chan_idx](const double freq){
            return this->set_rx_frequency(freq, chan_idx);
        })
        .set_publisher([this, chan_idx](){
            return this->get_rx_frequency(chan_idx);
        })
    ;
    subtree->create<meta_range_t>(rx_fe_path / "freq" / "range")
        .set(meta_range_t(RHODIUM_MIN_FREQ, RHODIUM_MAX_FREQ, 1.0))
        .add_coerced_subscriber([](const meta_range_t &){
            throw uhd::runtime_error(
                "Attempting to update freq range!");
        })
    ;
    // TX bandwidth
    subtree->create<double>(tx_fe_path / "bandwidth" / "value")
        .set_coercer([this, chan_idx](const double bw){
            return this->set_tx_bandwidth(bw, chan_idx);
        })
        .set_publisher([this, chan_idx](){
            return this->get_tx_bandwidth(chan_idx);
        })
    ;
    subtree->create<meta_range_t>(tx_fe_path / "bandwidth" / "range")
        .set(meta_range_t(0.0, 0.0, 0.0)) // FIXME
        .add_coerced_subscriber([](const meta_range_t &){
            throw uhd::runtime_error(
                "Attempting to update bandwidth range!");
        })
    ;
    // RX bandwidth
    subtree->create<double>(rx_fe_path / "bandwidth" / "value")
        .set_coercer([this, chan_idx](const double bw){
            return this->set_rx_bandwidth(bw, chan_idx);
        })
        .set_publisher([this, chan_idx](){
            return this->get_rx_bandwidth(chan_idx);
        })
    ;
    subtree->create<meta_range_t>(rx_fe_path / "bandwidth" / "range")
        .set(meta_range_t(0.0, 0.0, 0.0)) // FIXME
        .add_coerced_subscriber([](const meta_range_t &){
            throw uhd::runtime_error(
                "Attempting to update bandwidth range!");
        })
    ;
    // TX gains
    subtree->create<double>(tx_fe_path / "gains" / "all" / "value")
        .set_coercer([this, chan_idx](const double gain){
            return this->set_tx_gain(gain, chan_idx);
        })
        .set_publisher([this, chan_idx](){
            return radio_ctrl_impl::get_tx_gain(chan_idx);
        })
    ;
    subtree->create<meta_range_t>(tx_fe_path / "gains" / "all" / "range")
        .add_coerced_subscriber([](const meta_range_t &){
            throw uhd::runtime_error(
                "Attempting to update gain range!");
        })
        .set_publisher([](){
            return rhodium_radio_ctrl_impl::_get_gain_range(TX_DIRECTION);
        })
    ;

    subtree->create<std::vector<std::string>>(tx_fe_path / "gains/all/profile/options")
            .set(RHODIUM_GP_OPTIONS);

    subtree->create<std::string>(tx_fe_path / "gains/all/profile/value")
        .set_coercer([this](const std::string& profile){
            std::string return_profile = profile;
            if (!uhd::has(RHODIUM_GP_OPTIONS, profile))
            {
                return_profile = "default";
            }
            _gain_profile[TX_DIRECTION] = return_profile;
            return return_profile;
        })
        .set_publisher([this](){
            return _gain_profile[TX_DIRECTION];
        })
    ;

    // RX gains
    subtree->create<double>(rx_fe_path / "gains" / "all" / "value")
        .set_coercer([this, chan_idx](const double gain){
            return this->set_rx_gain(gain, chan_idx);
        })
        .set_publisher([this, chan_idx](){
            return radio_ctrl_impl::get_rx_gain(chan_idx);
        })
    ;

    subtree->create<meta_range_t>(rx_fe_path / "gains" / "all" / "range")
        .add_coerced_subscriber([](const meta_range_t &){
            throw uhd::runtime_error(
                "Attempting to update gain range!");
        })
        .set_publisher([](){
            return rhodium_radio_ctrl_impl::_get_gain_range(RX_DIRECTION);
        })
    ;

    subtree->create<std::vector<std::string> >(rx_fe_path / "gains/all/profile/options")
            .set(RHODIUM_GP_OPTIONS);

    subtree->create<std::string>(rx_fe_path / "gains/all/profile/value")
        .set_coercer([this](const std::string& profile){
            std::string return_profile = profile;
            if (!uhd::has(RHODIUM_GP_OPTIONS, profile))
            {
                return_profile = "default";
            }
            _gain_profile[RX_DIRECTION] = return_profile;
            return return_profile;
        })
        .set_publisher([this](){
            return _gain_profile[RX_DIRECTION];
        })
    ;

    // TX LO lock sensor
    subtree->create<sensor_value_t>(tx_fe_path / "sensors" / "lo_locked")
        .set(sensor_value_t("all_los", false,  "locked", "unlocked"))
        .add_coerced_subscriber([](const sensor_value_t &){
            throw uhd::runtime_error(
                "Attempting to write to sensor!");
        })
        .set_publisher([this](){
            return sensor_value_t(
                "all_los",
                this->get_lo_lock_status(TX_DIRECTION),
                "locked", "unlocked"
            );
        })
    ;
    // RX LO lock sensor
    subtree->create<sensor_value_t>(rx_fe_path / "sensors" / "lo_locked")
        .set(sensor_value_t("all_los", false,  "locked", "unlocked"))
        .add_coerced_subscriber([](const sensor_value_t &){
            throw uhd::runtime_error(
                "Attempting to write to sensor!");
        })
        .set_publisher([this](){
            return sensor_value_t(
                "all_los",
                this->get_lo_lock_status(RX_DIRECTION),
                "locked", "unlocked"
            );
        })
    ;
    //LO Specific
    //RX LO
    //RX LO1 Frequency
    subtree->create<double>(rx_fe_path / "los"/RHODIUM_LO1/"freq/value")
        .set_publisher([this,chan_idx](){
            return this->get_rx_lo_freq(RHODIUM_LO1, chan_idx);
        })
        .set_coercer([this,chan_idx](const double freq){
            return this->set_rx_lo_freq(freq, RHODIUM_LO1, chan_idx);
        })
    ;
    subtree->create<meta_range_t>(rx_fe_path / "los"/RHODIUM_LO1/"freq/range")
        .set_publisher([this,chan_idx](){
            return this->get_rx_lo_freq_range(RHODIUM_LO1, chan_idx);
        })
    ;
    //RX LO1 Source
    subtree->create<std::vector<std::string>>(rx_fe_path / "los"/RHODIUM_LO1/"source/options")
        .set_publisher([this,chan_idx](){
            return this->get_rx_lo_sources(RHODIUM_LO1, chan_idx);
        })
    ;
    subtree->create<std::string>(rx_fe_path / "los"/RHODIUM_LO1/"source/value")
        .add_coerced_subscriber([this,chan_idx](std::string src){
            this->set_rx_lo_source(src, RHODIUM_LO1,chan_idx);
        })
        .set_publisher([this,chan_idx](){
            return this->get_rx_lo_source(RHODIUM_LO1, chan_idx);
        })
    ;
    //RX LO1 Export
    subtree->create<bool>(rx_fe_path / "los"/RHODIUM_LO1/"export")
        .add_coerced_subscriber([this,chan_idx](bool enabled){
            this->set_rx_lo_export_enabled(enabled, RHODIUM_LO1, chan_idx);
        })
    ;
    //RX LO1 Gain
    subtree->create<double>(rx_fe_path / "los" /RHODIUM_LO1/ "gains" / RHODIUM_LO_GAIN / "value")
        .set_publisher([this,chan_idx](){
            return this->get_rx_lo_gain(RHODIUM_LO1, chan_idx);
        })
        .set_coercer([this,chan_idx](const double gain){
            return this->set_rx_lo_gain(gain, RHODIUM_LO1, chan_idx);
        })
    ;
    subtree->create<meta_range_t>(rx_fe_path / "los" /RHODIUM_LO1/ "gains" / RHODIUM_LO_GAIN / "range")
        .set_publisher([](){
            return rhodium_radio_ctrl_impl::_get_lo_gain_range();
        })
        .add_coerced_subscriber([](const meta_range_t &){
            throw uhd::runtime_error("Attempting to update LO gain range!");
        })
    ;
    //RX LO1 Output Power
    subtree->create<double>(rx_fe_path / "los" /RHODIUM_LO1/ "gains" / RHODIUM_LO_POWER / "value")
        .set_publisher([this,chan_idx](){
            return this->get_rx_lo_power(RHODIUM_LO1, chan_idx);
        })
        .set_coercer([this,chan_idx](const double gain){
            return this->set_rx_lo_power(gain, RHODIUM_LO1, chan_idx);
        })
    ;
    subtree->create<meta_range_t>(rx_fe_path / "los" /RHODIUM_LO1/ "gains" / RHODIUM_LO_POWER / "range")
        .set_publisher([](){
            return rhodium_radio_ctrl_impl::_get_lo_power_range();
        })
        .add_coerced_subscriber([](const meta_range_t &){
            throw uhd::runtime_error("Attempting to update LO output power range!");
        })
    ;
    //RX LO2 Frequency
    subtree->create<double>(rx_fe_path / "los"/RHODIUM_LO2/"freq/value")
        .set_publisher([this,chan_idx](){
            return this->get_rx_lo_freq(RHODIUM_LO2, chan_idx);
        })
        .set_coercer([this,chan_idx](double freq){
            return this->set_rx_lo_freq(freq, RHODIUM_LO2, chan_idx);
        })
    ;
    subtree->create<meta_range_t>(rx_fe_path / "los"/RHODIUM_LO2/"freq/range")
        .set_publisher([this,chan_idx](){
            return this->get_rx_lo_freq_range(RHODIUM_LO2, chan_idx);
        })
    ;
    //RX LO2 Source
    subtree->create<std::vector<std::string>>(rx_fe_path / "los"/RHODIUM_LO2/"source/options")
        .set_publisher([this,chan_idx](){
            return this->get_rx_lo_sources(RHODIUM_LO2, chan_idx);
        })
    ;
    subtree->create<std::string>(rx_fe_path / "los"/RHODIUM_LO2/"source/value")
        .add_coerced_subscriber([this,chan_idx](std::string src){
            this->set_rx_lo_source(src, RHODIUM_LO2, chan_idx);
        })
        .set_publisher([this,chan_idx](){
            return this->get_rx_lo_source(RHODIUM_LO2, chan_idx);
        })
    ;
    //RX LO2 Export
    subtree->create<bool>(rx_fe_path / "los"/RHODIUM_LO2/"export")
            .add_coerced_subscriber([this,chan_idx](bool enabled){
              this->set_rx_lo_export_enabled(enabled, RHODIUM_LO2, chan_idx);
        });
    //TX LO
    //TX LO1 Frequency
    subtree->create<double>(tx_fe_path / "los"/RHODIUM_LO1/"freq/value ")
        .set_publisher([this,chan_idx](){
            return this->get_tx_lo_freq(RHODIUM_LO1, chan_idx);
        })
        .set_coercer([this,chan_idx](double freq){
            return this->set_tx_lo_freq(freq, RHODIUM_LO1, chan_idx);
        })
    ;
     subtree->create<meta_range_t>(tx_fe_path / "los"/RHODIUM_LO1/"freq/range")
        .set_publisher([this,chan_idx](){
            return this->get_rx_lo_freq_range(RHODIUM_LO1, chan_idx);
        })
    ;
    //TX LO1 Source
    subtree->create<std::vector<std::string>>(tx_fe_path / "los"/RHODIUM_LO1/"source/options")
        .set_publisher([this,chan_idx](){
            return this->get_tx_lo_sources(RHODIUM_LO1, chan_idx);
        })
    ;
    subtree->create<std::string>(tx_fe_path / "los"/RHODIUM_LO1/"source/value")
        .add_coerced_subscriber([this,chan_idx](std::string src){
            this->set_tx_lo_source(src, RHODIUM_LO1, chan_idx);
        })
        .set_publisher([this,chan_idx](){
            return this->get_tx_lo_source(RHODIUM_LO1, chan_idx);
        })
    ;
    //TX LO1 Export
    subtree->create<bool>(tx_fe_path / "los"/RHODIUM_LO1/"export")
            .add_coerced_subscriber([this,chan_idx](bool enabled){
              this->set_tx_lo_export_enabled(enabled, RHODIUM_LO1, chan_idx);
            })
    ;
    //TX LO1 Gain
    subtree->create<double>(tx_fe_path / "los" /RHODIUM_LO1/ "gains" / RHODIUM_LO_GAIN / "value")
        .set_publisher([this,chan_idx](){
            return this->get_tx_lo_gain(RHODIUM_LO1, chan_idx);
        })
        .set_coercer([this,chan_idx](const double gain){
            return this->set_tx_lo_gain(gain, RHODIUM_LO1, chan_idx);
        })
    ;
    subtree->create<meta_range_t>(tx_fe_path / "los" /RHODIUM_LO1/ "gains" / RHODIUM_LO_GAIN / "range")
        .set_publisher([](){
            return rhodium_radio_ctrl_impl::_get_lo_gain_range();
        })
        .add_coerced_subscriber([](const meta_range_t &){
            throw uhd::runtime_error("Attempting to update LO gain range!");
        })
    ;
    //TX LO1 Output Power
    subtree->create<double>(tx_fe_path / "los" /RHODIUM_LO1/ "gains" / RHODIUM_LO_POWER / "value")
        .set_publisher([this,chan_idx](){
            return this->get_tx_lo_power(RHODIUM_LO1, chan_idx);
        })
        .set_coercer([this,chan_idx](const double gain){
            return this->set_tx_lo_power(gain, RHODIUM_LO1, chan_idx);
        })
    ;
    subtree->create<meta_range_t>(tx_fe_path / "los" /RHODIUM_LO1/ "gains" / RHODIUM_LO_POWER / "range")
        .set_publisher([](){
            return rhodium_radio_ctrl_impl::_get_lo_power_range();
        })
        .add_coerced_subscriber([](const meta_range_t &){
            throw uhd::runtime_error("Attempting to update LO output power range!");
        })
    ;
    //TX LO2 Frequency
    subtree->create<double>(tx_fe_path / "los"/RHODIUM_LO2/"freq/value")
        .set_publisher([this,chan_idx](){
            return this->get_tx_lo_freq(RHODIUM_LO2, chan_idx);
        })
        .set_coercer([this,chan_idx](double freq){
            return this->set_tx_lo_freq(freq, RHODIUM_LO2, chan_idx);
        })
    ;
    subtree->create<meta_range_t>(tx_fe_path / "los"/RHODIUM_LO2/"freq/range")
        .set_publisher([this,chan_idx](){
            return this->get_tx_lo_freq_range(RHODIUM_LO2,chan_idx);
        })
    ;
    //TX LO2 Source
    subtree->create<std::vector<std::string>>(tx_fe_path / "los"/RHODIUM_LO2/"source/options")
        .set_publisher([this,chan_idx](){
            return this->get_tx_lo_sources(RHODIUM_LO2, chan_idx);
        })
    ;
    subtree->create<std::string>(tx_fe_path / "los"/RHODIUM_LO2/"source/value")
        .add_coerced_subscriber([this,chan_idx](std::string src){
            this->set_tx_lo_source(src, RHODIUM_LO2, chan_idx);
        })
        .set_publisher([this,chan_idx](){
            return this->get_tx_lo_source(RHODIUM_LO2, chan_idx);
        })
    ;
    //TX LO2 Export
    subtree->create<bool>(tx_fe_path / "los"/RHODIUM_LO2/"export")
        .add_coerced_subscriber([this,chan_idx](bool enabled){
            this->set_tx_lo_export_enabled(enabled, RHODIUM_LO2, chan_idx);
        })
    ;
}

void rhodium_radio_ctrl_impl::_init_prop_tree()
{
    const fs_path fe_base = fs_path("dboards") / _radio_slot;
    this->_init_frontend_subtree(_tree->subtree(fe_base), 0);

    // EEPROM paths subject to change FIXME
    _tree->create<eeprom_map_t>(_root_path / "eeprom")
        .set(eeprom_map_t());

    _tree->create<int>("rx_codecs" / _radio_slot / "gains");
    _tree->create<int>("tx_codecs" / _radio_slot / "gains");
    _tree->create<std::string>("rx_codecs" / _radio_slot / "name").set("ad9695-625");
    _tree->create<std::string>("tx_codecs" / _radio_slot / "name").set("dac37j82");

    // TODO remove this dirty hack
    if (not _tree->exists("tick_rate"))
    {
        _tree->create<double>("tick_rate")
            .set_publisher([this](){ return this->get_rate(); })
        ;
    }
}

void rhodium_radio_ctrl_impl::_init_mpm_sensors(
        const direction_t dir,
        const size_t chan_idx
) {
    const std::string trx = (dir == RX_DIRECTION) ? "RX" : "TX";
    const fs_path fe_path =
        fs_path("dboards") / _radio_slot /
        (dir == RX_DIRECTION ? "rx_frontends" : "tx_frontends") / chan_idx;
    auto sensor_list =
        _rpcc->request_with_token<std::vector<std::string>>(
                this->_rpc_prefix + "get_sensors", trx);
    UHD_LOG_TRACE(unique_id(),
        "Chan " << chan_idx << ": Found "
        << sensor_list.size() << " " << trx << " sensors.");
    for (const auto &sensor_name : sensor_list) {
        UHD_LOG_TRACE(unique_id(),
            "Adding " << trx << " sensor " << sensor_name);
        _tree->create<sensor_value_t>(fe_path / "sensors" / sensor_name)
            .add_coerced_subscriber([](const sensor_value_t &){
                throw uhd::runtime_error(
                    "Attempting to write to sensor!");
            })
            .set_publisher([this, trx, sensor_name, chan_idx](){
                return sensor_value_t(
                    this->_rpcc->request_with_token<sensor_value_t::sensor_map_t>(
                        this->_rpc_prefix + "get_sensor",
                            trx, sensor_name, chan_idx)
                );
            })
        ;
    }
}

