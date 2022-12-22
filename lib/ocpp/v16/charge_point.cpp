// SPDX-License-Identifier: Apache-2.0
// Copyright 2020 - 2023 Pionix GmbH and Contributors to EVerest
#include <thread>

#include <everest/logging.hpp>
#include <ocpp/v16/charge_point.hpp>
#include <ocpp/v16/charge_point_configuration.hpp>

#include <openssl/rsa.h>

namespace ocpp {
namespace v16 {

ChargePoint::ChargePoint(const json& config, const std::string& share_path, const std::string& user_config_path,
                         const std::string& database_path, const std::string& sql_init_path,
                         const std::string& message_log_path) :
    ocpp::ChargePoint(),
    initialized(false),
    registration_status(RegistrationStatus::Pending),
    diagnostics_status(DiagnosticsStatus::Idle),
    firmware_status(FirmwareStatus::Idle),
    log_status(UploadLogStatusEnumType::Idle),
    switch_security_profile_callback(nullptr),
    message_log_path(message_log_path) {
    this->pki_handler = std::make_shared<ocpp::PkiHandler>(share_path);
    this->configuration =
        std::make_shared<ocpp::v16::ChargePointConfiguration>(config, share_path, user_config_path, this->pki_handler);
    this->heartbeat_timer = std::make_unique<Everest::SteadyTimer>(&this->io_service, [this]() { this->heartbeat(); });
    this->heartbeat_interval = this->configuration->getHeartbeatInterval();
    this->database_handler = std::make_shared<ocpp::DatabaseHandler>(this->configuration->getChargePointId(),
                                                                     boost::filesystem::path(database_path),
                                                                     boost::filesystem::path(sql_init_path));
    this->database_handler->open_db_connection(this->configuration->getNumberOfConnectors());
    this->transaction_handler = std::make_unique<TransactionHandler>(this->configuration->getNumberOfConnectors());
    this->external_notify = {v16::MessageType::StartTransactionResponse};
    this->message_queue = std::make_unique<ocpp::MessageQueue<v16::MessageType>>(
        [this](json message) -> bool { return this->websocket->send(message.dump()); },
        this->configuration->getTransactionMessageAttempts(), this->configuration->getTransactionMessageRetryInterval(),
        this->external_notify);

    auto log_formats = this->configuration->getLogMessagesFormat();
    bool log_to_console = std::find(log_formats.begin(), log_formats.end(), "console") != log_formats.end();
    bool detailed_log_to_console =
        std::find(log_formats.begin(), log_formats.end(), "console_detailed") != log_formats.end();
    bool log_to_file = std::find(log_formats.begin(), log_formats.end(), "log") != log_formats.end();
    bool log_to_html = std::find(log_formats.begin(), log_formats.end(), "html") != log_formats.end();

    this->logging =
        std::make_shared<ocpp::MessageLogging>(this->configuration->getLogMessages(), message_log_path, log_to_console,
                                               detailed_log_to_console, log_to_file, log_to_html);

    this->boot_notification_timer =
        std::make_unique<Everest::SteadyTimer>(&this->io_service, [this]() { this->boot_notification(); });

    for (int32_t connector = 0; connector < this->configuration->getNumberOfConnectors() + 1; connector++) {
        this->status_notification_timers.push_back(std::make_unique<Everest::SteadyTimer>(&this->io_service));
    }

    this->clock_aligned_meter_values_timer = std::make_unique<Everest::SystemTimer>(
        &this->io_service, [this]() { this->clock_aligned_meter_values_sample(); });

    this->status = std::make_unique<ChargePointStates>(
        this->configuration->getNumberOfConnectors(),
        [this](int32_t connector, ChargePointErrorCode errorCode, ChargePointStatus status) {
            this->status_notification_timers.at(connector)->stop();
            this->status_notification_timers.at(connector)->timeout(
                [this, connector, errorCode, status]() { this->status_notification(connector, errorCode, status); },
                std::chrono::seconds(this->configuration->getMinimumStatusDuration().get_value_or(0)));
        });

    for (int id = 0; id <= this->configuration->getNumberOfConnectors(); id++) {
        this->connectors.insert(std::make_pair(id, std::make_shared<Connector>(id)));
    }

    this->smart_charging_handler = std::make_unique<SmartChargingHandler>(this->connectors, this->database_handler);
}

void ChargePoint::init_websocket(int32_t security_profile) {
    WebsocketConnectionOptions connection_options{OcppProtocolVersion::v16,
                                                  this->configuration->getCentralSystemURI(),
                                                  security_profile,
                                                  this->configuration->getChargePointId(),
                                                  this->configuration->getAuthorizationKey(),
                                                  this->configuration->getWebsocketReconnectInterval(),
                                                  this->configuration->getSupportedCiphers12(),
                                                  this->configuration->getSupportedCiphers13()};

    this->websocket = std::make_unique<Websocket>(connection_options, this->pki_handler, this->logging);
    this->websocket->register_connected_callback([this](const int security_profile) {
        if (this->connection_state_changed_callback != nullptr) {
            this->connection_state_changed_callback(true);
        }
        this->configuration->setSecurityProfile(security_profile);
        this->message_queue->resume(); //
        this->connected_callback();    //
    });
    this->websocket->register_disconnected_callback([this]() {
        if (this->connection_state_changed_callback != nullptr) {
            this->connection_state_changed_callback(false);
        }
        this->message_queue->pause(); //
        if (this->switch_security_profile_callback != nullptr) {
            this->switch_security_profile_callback();
        }
    });

    this->websocket->register_message_callback([this](const std::string& message) { this->message_callback(message); });

    if (security_profile == 3) {
        EVLOG_debug << "Registerung certificate timer";
        this->websocket->register_sign_certificate_callback([this]() { this->sign_certificate(); });
    }
}

void ChargePoint::connect_websocket() {
    if (!this->websocket->is_connected()) {
        this->init_websocket(this->configuration->getSecurityProfile());
        this->websocket->connect(this->configuration->getSecurityProfile());
    }
}

void ChargePoint::disconnect_websocket() {
    if (this->websocket->is_connected()) {
        this->websocket->disconnect(websocketpp::close::status::going_away);
    }
}

void ChargePoint::call_set_connection_timeout() {
    if (this->set_connection_timeout_callback != nullptr) {
        this->set_connection_timeout_callback(this->configuration->getConnectionTimeOut());
    }
}

void ChargePoint::heartbeat() {
    EVLOG_debug << "Sending heartbeat";
    HeartbeatRequest req;

    ocpp::Call<HeartbeatRequest> call(req, this->message_queue->createMessageId());
    this->send<HeartbeatRequest>(call);
}

void ChargePoint::boot_notification() {
    EVLOG_debug << "Sending BootNotification";
    BootNotificationRequest req;
    req.chargeBoxSerialNumber.emplace(this->configuration->getChargeBoxSerialNumber());
    req.chargePointModel = this->configuration->getChargePointModel();
    req.chargePointSerialNumber = this->configuration->getChargePointSerialNumber();
    req.chargePointVendor = this->configuration->getChargePointVendor();
    req.firmwareVersion.emplace(this->configuration->getFirmwareVersion());
    req.iccid = this->configuration->getICCID();
    req.imsi = this->configuration->getIMSI();
    req.meterSerialNumber = this->configuration->getMeterSerialNumber();
    req.meterType = this->configuration->getMeterType();

    ocpp::Call<BootNotificationRequest> call(req, this->message_queue->createMessageId());
    this->send<BootNotificationRequest>(call);
}

void ChargePoint::clock_aligned_meter_values_sample() {
    if (this->initialized) {
        EVLOG_debug << "Sending clock aligned meter values";
        for (int32_t connector = 1; connector < this->configuration->getNumberOfConnectors() + 1; connector++) {
            auto meter_value = this->get_latest_meter_value(
                connector, this->configuration->getMeterValuesAlignedDataVector(), ReadingContext::Sample_Clock);
            if (this->transaction_handler->transaction_active(connector)) {
                this->transaction_handler->get_transaction(connector)->add_meter_value(meter_value);
            }
            this->send_meter_value(connector, meter_value);
        }
        this->update_clock_aligned_meter_values_interval();
    }
}

void ChargePoint::update_heartbeat_interval() {
    this->heartbeat_timer->interval(std::chrono::seconds(this->configuration->getHeartbeatInterval()));
}

void ChargePoint::update_meter_values_sample_interval() {
    // TODO(kai): should we update the meter values for continuous monitoring here too?
    int32_t interval = this->configuration->getMeterValueSampleInterval();
    this->transaction_handler->change_meter_values_sample_intervals(interval);
}

void ChargePoint::update_clock_aligned_meter_values_interval() {
    int32_t clock_aligned_data_interval = this->configuration->getClockAlignedDataInterval();
    if (clock_aligned_data_interval == 0) {
        return;
    }
    auto seconds_in_a_day = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::hours(24)).count();
    auto now = date::utc_clock::now();
    auto midnight = date::floor<date::days>(now) + std::chrono::seconds(date::get_tzdb().leap_seconds.size());
    auto diff = now - midnight;
    auto start = std::chrono::duration_cast<std::chrono::seconds>(diff / clock_aligned_data_interval) *
                     clock_aligned_data_interval +
                 std::chrono::seconds(clock_aligned_data_interval);
    this->clock_aligned_meter_values_time_point = midnight + start;
    using date::operator<<;
    std::ostringstream oss;
    EVLOG_debug << "Sending clock aligned meter values every " << clock_aligned_data_interval
                << " seconds, starting at " << ocpp::DateTime(this->clock_aligned_meter_values_time_point)
                << ". This amounts to " << seconds_in_a_day / clock_aligned_data_interval << " samples per day.";
    EVLOG_debug << oss.str();

    this->clock_aligned_meter_values_timer->at(this->clock_aligned_meter_values_time_point);
}

void ChargePoint::stop_pending_transactions() {
    const auto transactions = this->database_handler->get_transactions(true);
    for (const auto& transaction_entry : transactions) {
        StopTransactionRequest req;
        req.meterStop = transaction_entry.meter_start; // FIXME(piet): Get latest meter value here
        req.timestamp = ocpp::DateTime();
        req.reason = Reason::PowerLoss;
        req.transactionId = transaction_entry.transaction_id;

        auto message_id = this->message_queue->createMessageId();
        ocpp::Call<StopTransactionRequest> call(req, message_id);

        {
            std::lock_guard<std::mutex> lock(this->stop_transaction_mutex);
            this->send<StopTransactionRequest>(call);
        }
        this->database_handler->update_transaction(transaction_entry.session_id, req.meterStop, req.timestamp,
                                                   boost::none, req.reason);
    }
}

void ChargePoint::load_charging_profiles() {
    auto profiles = this->database_handler->get_charging_profiles();
    EVLOG_info << "Found " << profiles.size() << " charging profile(s) in the database";
    for (auto& profile : profiles) {
        const auto connector_id = this->database_handler->get_connector_id(profile.chargingProfileId);
        if (this->smart_charging_handler->validate_profile(
                profile, connector_id, false, this->configuration->getChargeProfileMaxStackLevel(),
                this->configuration->getMaxChargingProfilesInstalled(),
                this->configuration->getChargingScheduleMaxPeriods(),
                this->configuration->getChargingScheduleAllowedChargingRateUnitVector())) {

            if (profile.chargingProfilePurpose == ChargingProfilePurposeType::ChargePointMaxProfile) {
                this->smart_charging_handler->add_charge_point_max_profile(profile);
            } else if (profile.chargingProfilePurpose == ChargingProfilePurposeType::TxDefaultProfile) {
                this->smart_charging_handler->add_tx_default_profile(profile, connector_id);
            } else if (profile.chargingProfilePurpose == ChargingProfilePurposeType::TxProfile) {
                this->smart_charging_handler->add_tx_profile(profile, connector_id);
            }
        } else {
            // delete if not valid anymore
            this->database_handler->delete_charging_profile(profile.chargingProfileId);
        }
    }
}

MeterValue ChargePoint::get_latest_meter_value(int32_t connector, std::vector<MeasurandWithPhase> values_of_interest,
                                               ReadingContext context) {
    std::lock_guard<std::mutex> lock(power_meters_mutex);
    MeterValue filtered_meter_value;
    // TODO(kai): also support readings from the charge point powermeter at "connector 0"
    if (this->connectors.find(connector) != this->connectors.end()) {
        const auto power_meter = this->connectors.at(connector)->powermeter;
        const auto timestamp = power_meter.timestamp;
        filtered_meter_value.timestamp = ocpp::DateTime(timestamp);
        EVLOG_debug << "PowerMeter value for connector: " << connector << ": " << power_meter;

        for (auto configured_measurand : values_of_interest) {
            EVLOG_debug << "Value of interest: " << conversions::measurand_to_string(configured_measurand.measurand);
            // constructing sampled value
            SampledValue sample;

            sample.context.emplace(context);
            sample.format.emplace(ValueFormat::Raw); // TODO(kai): support signed data as well

            sample.measurand.emplace(configured_measurand.measurand);
            if (configured_measurand.phase) {
                EVLOG_debug << "  there is a phase configured: "
                            << conversions::phase_to_string(configured_measurand.phase.value());
            }
            switch (configured_measurand.measurand) {
            case Measurand::Energy_Active_Import_Register: {
                const auto energy_Wh_import = power_meter.energy_Wh_import;

                // Imported energy in Wh (from grid)
                sample.unit.emplace(UnitOfMeasure::Wh);
                sample.location.emplace(Location::Outlet);

                if (configured_measurand.phase) {
                    // phase available and it makes sense here
                    auto phase = configured_measurand.phase.value();
                    sample.phase.emplace(phase);
                    if (phase == Phase::L1) {
                        if (energy_Wh_import.L1) {
                            sample.value = ocpp::conversions::double_to_string((double)energy_Wh_import.L1.value());
                        } else {
                            EVLOG_debug
                                << "Power meter does not contain energy_Wh_import configured measurand for phase L1";
                        }
                    } else if (phase == Phase::L2) {
                        if (energy_Wh_import.L2) {
                            sample.value = ocpp::conversions::double_to_string((double)energy_Wh_import.L2.value());
                        } else {
                            EVLOG_debug
                                << "Power meter does not contain energy_Wh_import configured measurand for phase L2";
                        }
                    } else if (phase == Phase::L3) {
                        if (energy_Wh_import.L3) {
                            sample.value = ocpp::conversions::double_to_string((double)energy_Wh_import.L3.value());
                        } else {
                            EVLOG_debug
                                << "Power meter does not contain energy_Wh_import configured measurand for phase L3";
                        }
                    }
                } else {
                    // store total value
                    sample.value = ocpp::conversions::double_to_string((double)energy_Wh_import.total);
                }
                break;
            }
            case Measurand::Energy_Active_Export_Register: {
                const auto energy_Wh_export = power_meter.energy_Wh_export;
                // Exported energy in Wh (to grid)
                sample.unit.emplace(UnitOfMeasure::Wh);
                // TODO: which location is appropriate here? Inlet?
                // sample.location.emplace(Location::Outlet);
                if (energy_Wh_export) {
                    if (configured_measurand.phase) {
                        // phase available and it makes sense here
                        auto phase = configured_measurand.phase.value();
                        sample.phase.emplace(phase);
                        if (phase == Phase::L1) {
                            if (energy_Wh_export.value().L1) {
                                sample.value =
                                    ocpp::conversions::double_to_string((double)energy_Wh_export.value().L1.value());
                            } else {
                                EVLOG_debug << "Power meter does not contain energy_Wh_export configured measurand "
                                               "for phase L1";
                            }
                        } else if (phase == Phase::L2) {
                            if (energy_Wh_export.value().L2) {
                                sample.value =
                                    ocpp::conversions::double_to_string((double)energy_Wh_export.value().L2.value());
                            } else {
                                EVLOG_debug << "Power meter does not contain energy_Wh_export configured measurand "
                                               "for phase L2";
                            }
                        } else if (phase == Phase::L3) {
                            if (energy_Wh_export.value().L3) {
                                sample.value =
                                    ocpp::conversions::double_to_string((double)energy_Wh_export.value().L3.value());
                            } else {
                                EVLOG_debug << "Power meter does not contain energy_Wh_export configured measurand "
                                               "for phase L3";
                            }
                        }
                    } else {
                        // store total value
                        sample.value = ocpp::conversions::double_to_string((double)energy_Wh_export.value().total);
                    }
                } else {
                    EVLOG_debug << "Power meter does not contain energy_Wh_export configured measurand";
                }
                break;
            }
            case Measurand::Power_Active_Import: {
                const auto power_W = power_meter.power_W;
                // power flow to EV, Instantaneous power in Watt
                sample.unit.emplace(UnitOfMeasure::W);
                sample.location.emplace(Location::Outlet);
                if (power_W) {
                    if (configured_measurand.phase) {
                        // phase available and it makes sense here
                        auto phase = configured_measurand.phase.value();
                        sample.phase.emplace(phase);
                        if (phase == Phase::L1) {
                            if (power_W.value().L1) {
                                sample.value = ocpp::conversions::double_to_string((double)power_W.value().L1.value());
                            } else {
                                EVLOG_debug << "Power meter does not contain power_W configured measurand for phase L1";
                            }
                        } else if (phase == Phase::L2) {
                            if (power_W.value().L2) {
                                sample.value = ocpp::conversions::double_to_string((double)power_W.value().L2.value());
                            } else {
                                EVLOG_debug << "Power meter does not contain power_W configured measurand for phase L2";
                            }
                        } else if (phase == Phase::L3) {
                            if (power_W.value().L3) {
                                sample.value = ocpp::conversions::double_to_string((double)power_W.value().L3.value());
                            } else {
                                EVLOG_debug << "Power meter does not contain power_W configured measurand for phase L3";
                            }
                        }
                    } else {
                        // store total value
                        sample.value = ocpp::conversions::double_to_string((double)power_W.value().total);
                    }
                } else {
                    EVLOG_debug << "Power meter does not contain power_W configured measurand";
                }
                break;
            }
            case Measurand::Voltage: {
                const auto voltage_V = power_meter.voltage_V;
                // AC supply voltage, Voltage in Volts
                sample.unit.emplace(UnitOfMeasure::V);
                sample.location.emplace(Location::Outlet);
                if (voltage_V) {
                    if (configured_measurand.phase) {
                        // phase available and it makes sense here
                        auto phase = configured_measurand.phase.value();
                        sample.phase.emplace(phase);
                        if (phase == Phase::L1) {
                            if (voltage_V.value().L1) {
                                sample.value =
                                    ocpp::conversions::double_to_string((double)voltage_V.value().L1.value());
                            } else {
                                EVLOG_debug
                                    << "Power meter does not contain voltage_V configured measurand for phase L1";
                            }
                        } else if (phase == Phase::L2) {
                            if (voltage_V.value().L2) {
                                sample.value =
                                    ocpp::conversions::double_to_string((double)voltage_V.value().L2.value());
                            } else {
                                EVLOG_debug
                                    << "Power meter does not contain voltage_V configured measurand for phase L2";
                            }
                        } else if (phase == Phase::L3) {
                            if (voltage_V.value().L3) {
                                sample.value =
                                    ocpp::conversions::double_to_string((double)voltage_V.value().L3.value());
                            } else {
                                EVLOG_debug
                                    << "Power meter does not contain voltage_V configured measurand for phase L3";
                            }
                        }
                    }
                } else {
                    EVLOG_debug << "Power meter does not contain voltage_V configured measurand";
                }
                break;
            }
            case Measurand::Current_Import: {
                const auto current_A = power_meter.current_A;
                // current flow to EV in A
                sample.unit.emplace(UnitOfMeasure::A);
                sample.location.emplace(Location::Outlet);
                if (current_A) {
                    if (configured_measurand.phase) {
                        // phase available and it makes sense here
                        auto phase = configured_measurand.phase.value();
                        sample.phase.emplace(phase);
                        if (phase == Phase::L1) {
                            if (current_A.value().L1) {
                                sample.value =
                                    ocpp::conversions::double_to_string((double)current_A.value().L1.value());
                            } else {
                                EVLOG_debug
                                    << "Power meter does not contain current_A configured measurand for phase L1";
                            }
                        } else if (phase == Phase::L2) {
                            if (current_A.value().L2) {
                                sample.value =
                                    ocpp::conversions::double_to_string((double)current_A.value().L2.value());
                            } else {
                                EVLOG_debug
                                    << "Power meter does not contain current_A configured measurand for phase L2";
                            }
                        } else if (phase == Phase::L3) {
                            if (current_A.value().L3) {
                                sample.value =
                                    ocpp::conversions::double_to_string((double)current_A.value().L3.value());
                            } else {
                                EVLOG_debug
                                    << "Power meter does not contain current_A configured measurand for phase L3";
                            }
                        }
                    }
                } else {
                    EVLOG_debug << "Power meter does not contain current_A configured measurand";
                }

                break;
            }
            case Measurand::Frequency: {
                const auto frequency_Hz = power_meter.frequency_Hz;
                // Grid frequency in Hertz
                // TODO: which location is appropriate here? Inlet?
                // sample.location.emplace(Location::Outlet);
                if (frequency_Hz) {
                    if (configured_measurand.phase) {
                        // phase available and it makes sense here
                        auto phase = configured_measurand.phase.value();
                        sample.phase.emplace(phase);
                        if (phase == Phase::L1) {
                            sample.value = ocpp::conversions::double_to_string((double)frequency_Hz.value().L1);
                        } else if (phase == Phase::L2) {
                            if (frequency_Hz.value().L2) {
                                sample.value =
                                    ocpp::conversions::double_to_string((double)frequency_Hz.value().L2.value());
                            } else {
                                EVLOG_debug
                                    << "Power meter does not contain frequency_Hz configured measurand for phase L2";
                            }
                        } else if (phase == Phase::L3) {
                            if (frequency_Hz.value().L3) {
                                sample.value =
                                    ocpp::conversions::double_to_string((double)frequency_Hz.value().L3.value());
                            } else {
                                EVLOG_debug
                                    << "Power meter does not contain frequency_Hz configured measurand for phase L3";
                            }
                        }
                    }
                } else {
                    EVLOG_debug << "Power meter does not contain frequency_Hz configured measurand";
                }
                break;
            }
            case Measurand::Current_Offered: {
                // current offered to EV
                sample.unit.emplace(UnitOfMeasure::A);
                sample.location.emplace(Location::Outlet);

                sample.value = ocpp::conversions::double_to_string(this->connectors.at(connector)->max_current_offered);
                break;
            }
            default:
                break;
            }
            // only add if value is set
            if (!sample.value.empty()) {
                filtered_meter_value.sampledValue.push_back(sample);
            }
        }
    }
    return filtered_meter_value;
}

MeterValue ChargePoint::get_signed_meter_value(const std::string& signed_value, const ReadingContext& context,
                                               const ocpp::DateTime& timestamp) {
    MeterValue meter_value;
    meter_value.timestamp = timestamp;
    SampledValue sampled_value;
    sampled_value.context = context;
    sampled_value.value = signed_value;
    sampled_value.format = ValueFormat::SignedData;

    meter_value.sampledValue.push_back(sampled_value);
    return meter_value;
}

void ChargePoint::send_meter_value(int32_t connector, MeterValue meter_value) {

    if (meter_value.sampledValue.size() == 0) {
        return;
    }

    MeterValuesRequest req;
    // connector = 0 designates the main powermeter
    // connector > 0 designates a connector of the charge point
    req.connectorId = connector;
    std::ostringstream oss;
    oss << "Gathering measurands of connector: " << connector;
    if (connector > 0) {
        auto transaction = this->transaction_handler->get_transaction(connector);
        if (transaction != nullptr && transaction->get_transaction_id() != -1) {
            auto transaction_id = transaction->get_transaction_id();
            req.transactionId.emplace(transaction_id);
        }
    }

    EVLOG_debug << oss.str();

    req.meterValue.push_back(meter_value);

    ocpp::Call<MeterValuesRequest> call(req, this->message_queue->createMessageId());
    this->send<MeterValuesRequest>(call);
}

bool ChargePoint::start() {
    this->init_websocket(this->configuration->getSecurityProfile());
    this->websocket->connect(this->configuration->getSecurityProfile());
    this->boot_notification();
    this->stop_pending_transactions();
    this->load_charging_profiles();
    this->stopped = false;
    return true;
}

bool ChargePoint::restart() {
    if (this->stopped) {
        EVLOG_info << "Restarting OCPP Chargepoint";
        this->database_handler->open_db_connection(this->configuration->getNumberOfConnectors());
        // instantiating new message queue on restart
        this->message_queue = std::make_unique<ocpp::MessageQueue<v16::MessageType>>(
            [this](json message) -> bool { return this->websocket->send(message.dump()); },
            this->configuration->getTransactionMessageAttempts(),
            this->configuration->getTransactionMessageRetryInterval(), this->external_notify);
        this->initialized = true;
        return this->start();
    } else {
        EVLOG_warning << "Attempting to restart Chargepoint while it has not been stopped before";
        return false;
    }
}

void ChargePoint::stop_all_transactions() {
    this->stop_all_transactions(Reason::Other);
}

void ChargePoint::stop_all_transactions(Reason reason) {
    int32_t number_of_connectors = this->configuration->getNumberOfConnectors();
    for (int32_t connector = 1; connector <= number_of_connectors; connector++) {
        if (this->transaction_handler->transaction_active(connector)) {
            this->stop_transaction_callback(connector, reason);
        }
    }
}

bool ChargePoint::stop() {
    if (!this->stopped) {
        EVLOG_info << "Stopping OCPP Chargepoint";
        this->initialized = false;
        if (this->boot_notification_timer != nullptr) {
            this->boot_notification_timer->stop();
        }
        if (this->heartbeat_timer != nullptr) {
            this->heartbeat_timer->stop();
        }
        if (this->clock_aligned_meter_values_timer != nullptr) {
            this->clock_aligned_meter_values_timer->stop();
        }

        this->stop_all_transactions();

        this->database_handler->close_db_connection();
        this->websocket->disconnect(websocketpp::close::status::going_away);
        this->message_queue->stop();

        this->stopped = true;
        EVLOG_info << "Terminating...";
        return true;
    } else {
        EVLOG_warning << "Attempting to stop Chargepoint while it has been stopped before";
        return false;
    }

    EVLOG_info << "Terminating...";
}

void ChargePoint::connected_callback() {
    this->switch_security_profile_callback = nullptr;
    this->pki_handler->removeCentralSystemFallbackCa();
    switch (this->connection_state) {
    case ChargePointConnectionState::Disconnected: {
        this->connection_state = ChargePointConnectionState::Connected;
        break;
    }
    case ChargePointConnectionState::Booted: {
        // on_open in a Booted state can happen after a successful reconnect.
        // according to spec, a charge point should not send a BootNotification after a reconnect
        // still we send StatusNotification.req for all connectors after a reconnect
        for (int32_t connector = 0; connector <= this->configuration->getNumberOfConnectors(); connector++) {
            this->status_notification(connector, ChargePointErrorCode::NoError, this->status->get_state(connector));
        }
        break;
    }
    default:
        EVLOG_error << "Connected but not in state 'Disconnected' or 'Booted', something is wrong: "
                    << this->connection_state;
        break;
    }
}

void ChargePoint::message_callback(const std::string& message) {
    EVLOG_debug << "Received Message: " << message;

    // EVLOG_debug << "json message: " << json_message;
    auto enhanced_message = this->message_queue->receive(message);
    auto json_message = enhanced_message.message;
    this->logging->central_system(conversions::messagetype_to_string(enhanced_message.messageType), message);
    // reject unsupported messages
    if (this->configuration->getSupportedMessageTypesReceiving().count(enhanced_message.messageType) == 0) {
        EVLOG_warning << "Received an unsupported message: " << enhanced_message.messageType;
        // FIXME(kai): however, only send a CALLERROR when it is a CALL message we just received
        if (enhanced_message.messageTypeId == MessageTypeId::CALL) {
            auto call_error = CallError(enhanced_message.uniqueId, "NotSupported", "", json({}));
            this->send(call_error);
        }

        // in any case stop message handling here:
        return;
    }

    switch (this->connection_state) {
    case ChargePointConnectionState::Disconnected: {
        EVLOG_error << "Received a message in disconnected state, this cannot be correct";
        break;
    }
    case ChargePointConnectionState::Connected: {
        if (enhanced_message.messageType == MessageType::BootNotificationResponse) {
            this->handleBootNotificationResponse(json_message);
        }
        break;
    }
    case ChargePointConnectionState::Rejected: {
        if (this->registration_status == RegistrationStatus::Rejected) {
            if (enhanced_message.messageType == MessageType::BootNotificationResponse) {
                this->handleBootNotificationResponse(json_message);
            }
        }
        break;
    }
    case ChargePointConnectionState::Pending: {
        if (this->registration_status == RegistrationStatus::Pending) {
            if (enhanced_message.messageType == MessageType::BootNotificationResponse) {
                this->handleBootNotificationResponse(json_message);
            } else {
                this->handle_message(json_message, enhanced_message.messageType);
            }
        }
        break;
    }
    case ChargePointConnectionState::Booted: {
        this->handle_message(json_message, enhanced_message.messageType);
        break;
    }

    default:
        EVLOG_error << "Reached default statement in on_message, this should not be possible";
        break;
    }
}

void ChargePoint::handle_message(const json& json_message, MessageType message_type) {
    // lots of messages are allowed here
    switch (message_type) {

    case MessageType::AuthorizeResponse:
        // handled by authorize_id_tag future
        break;

    case MessageType::CertificateSigned:
        this->handleCertificateSignedRequest(json_message);
        break;

    case MessageType::ChangeAvailability:
        this->handleChangeAvailabilityRequest(json_message);
        break;

    case MessageType::ChangeConfiguration:
        this->handleChangeConfigurationRequest(json_message);
        break;

    case MessageType::ClearCache:
        this->handleClearCacheRequest(json_message);
        break;

    case MessageType::DataTransfer:
        this->handleDataTransferRequest(json_message);
        break;

    case MessageType::DataTransferResponse:
        // handled by data_transfer future
        break;

    case MessageType::GetConfiguration:
        this->handleGetConfigurationRequest(json_message);
        break;

    case MessageType::RemoteStartTransaction:
        this->handleRemoteStartTransactionRequest(json_message);
        break;

    case MessageType::RemoteStopTransaction:
        this->handleRemoteStopTransactionRequest(json_message);
        break;

    case MessageType::Reset:
        this->handleResetRequest(json_message);
        break;

    case MessageType::StartTransactionResponse:
        this->handleStartTransactionResponse(json_message);
        break;

    case MessageType::StopTransactionResponse:
        this->handleStopTransactionResponse(json_message);
        break;

    case MessageType::UnlockConnector:
        this->handleUnlockConnectorRequest(json_message);
        break;

    case MessageType::SetChargingProfile:
        this->handleSetChargingProfileRequest(json_message);
        break;

    case MessageType::GetCompositeSchedule:
        this->handleGetCompositeScheduleRequest(json_message);
        break;

    case MessageType::ClearChargingProfile:
        this->handleClearChargingProfileRequest(json_message);
        break;

    case MessageType::TriggerMessage:
        this->handleTriggerMessageRequest(json_message);
        break;

    case MessageType::GetDiagnostics:
        this->handleGetDiagnosticsRequest(json_message);
        break;

    case MessageType::UpdateFirmware:
        this->handleUpdateFirmwareRequest(json_message);
        break;

    case MessageType::GetInstalledCertificateIds:
        this->handleGetInstalledCertificateIdsRequest(json_message);
        break;

    case MessageType::DeleteCertificate:
        this->handleDeleteCertificateRequest(json_message);
        break;

    case MessageType::InstallCertificate:
        this->handleInstallCertificateRequest(json_message);
        break;

    case MessageType::GetLog:
        this->handleGetLogRequest(json_message);
        break;

    case MessageType::SignedUpdateFirmware:
        this->handleSignedUpdateFirmware(json_message);
        break;

    case MessageType::ReserveNow:
        this->handleReserveNowRequest(json_message);
        break;

    case MessageType::CancelReservation:
        this->handleCancelReservationRequest(json_message);
        break;

    case MessageType::ExtendedTriggerMessage:
        this->handleExtendedTriggerMessageRequest(json_message);
        break;

    case MessageType::SendLocalList:
        this->handleSendLocalListRequest(json_message);
        break;

    case MessageType::GetLocalListVersion:
        this->handleGetLocalListVersionRequest(json_message);
        break;

    default:
        // TODO(kai): not implemented error?
        break;
    }
}

void ChargePoint::handleBootNotificationResponse(ocpp::CallResult<BootNotificationResponse> call_result) {
    EVLOG_debug << "Received BootNotificationResponse: " << call_result.msg
                << "\nwith messageId: " << call_result.uniqueId;

    this->registration_status = call_result.msg.status;
    this->initialized = true;
    this->boot_time = date::utc_clock::now();
    if (call_result.msg.interval > 0) {
        this->configuration->setHeartbeatInterval(call_result.msg.interval);
    }
    switch (call_result.msg.status) {
    case RegistrationStatus::Accepted: {
        this->connection_state = ChargePointConnectionState::Booted;
        // we are allowed to send messages to the central system
        // activate heartbeat
        this->update_heartbeat_interval();

        // activate clock aligned sampling of meter values
        this->update_clock_aligned_meter_values_interval();

        auto connector_availability = this->database_handler->get_connector_availability();
        connector_availability[0] = AvailabilityType::Operative; // FIXME(kai): fix internal representation in charge
                                                                 // point states, we need a different kind of state
                                                                 // machine for connector 0 anyway (with reduced states)
        this->status->run(connector_availability);
        break;
    }
    case RegistrationStatus::Pending:
        this->connection_state = ChargePointConnectionState::Pending;

        EVLOG_debug << "BootNotification response is pending.";
        this->boot_notification_timer->timeout(std::chrono::seconds(call_result.msg.interval));
        break;
    default:
        this->connection_state = ChargePointConnectionState::Rejected;
        // In this state we are not allowed to send any messages to the central system, even when
        // requested. The first time we are allowed to send a message (a BootNotification) is
        // after boot_time + heartbeat_interval if the msg.interval is 0, or after boot_timer + msg.interval
        EVLOG_debug << "BootNotification was rejected, trying again in " << this->configuration->getHeartbeatInterval()
                    << "s";

        this->boot_notification_timer->timeout(std::chrono::seconds(call_result.msg.interval));

        break;
    }
}
void ChargePoint::handleChangeAvailabilityRequest(ocpp::Call<ChangeAvailabilityRequest> call) {
    EVLOG_debug << "Received ChangeAvailabilityRequest: " << call.msg << "\nwith messageId: " << call.uniqueId;

    ChangeAvailabilityResponse response;
    // we can only change the connector availability if there is no active transaction on this
    // connector. is that case this change must be scheduled and we should report an availability status
    // of "Scheduled"

    // check if connector exists
    if (call.msg.connectorId <= this->configuration->getNumberOfConnectors() && call.msg.connectorId >= 0) {
        std::vector<int32_t> connectors;
        bool transaction_running = false;

        if (call.msg.connectorId == 0) {
            int32_t number_of_connectors = this->configuration->getNumberOfConnectors();
            for (int32_t connector = 1; connector <= number_of_connectors; connector++) {
                if (this->transaction_handler->transaction_active(connector)) {
                    transaction_running = true;
                    std::lock_guard<std::mutex> change_availability_lock(change_availability_mutex);
                    this->change_availability_queue[connector] = call.msg.type;
                } else {
                    connectors.push_back(connector);
                }
            }
        } else {
            if (this->transaction_handler->transaction_active(call.msg.connectorId)) {
                transaction_running = true;
            } else {
                connectors.push_back(call.msg.connectorId);
            }
        }

        if (transaction_running) {
            response.status = AvailabilityStatus::Scheduled;
        } else {
            this->database_handler->insert_or_update_connector_availability(connectors, call.msg.type);
            for (auto connector : connectors) {
                if (call.msg.type == AvailabilityType::Operative) {
                    if (this->enable_evse_callback != nullptr) {
                        // TODO(kai): check return value
                        this->enable_evse_callback(connector);
                    }
                    this->status->submit_event(connector, Event_BecomeAvailable());
                } else {
                    if (this->disable_evse_callback != nullptr) {
                        // TODO(kai): check return value
                        this->disable_evse_callback(connector);
                    }
                    this->status->submit_event(connector, Event_ChangeAvailabilityToUnavailable());
                }
            }

            response.status = AvailabilityStatus::Accepted;
        }
    } else {
        // Reject if given connector id doesnt exist
        response.status = AvailabilityStatus::Rejected;
    }

    ocpp::CallResult<ChangeAvailabilityResponse> call_result(response, call.uniqueId);
    this->send<ChangeAvailabilityResponse>(call_result);
}

void ChargePoint::handleChangeConfigurationRequest(ocpp::Call<ChangeConfigurationRequest> call) {
    EVLOG_debug << "Received ChangeConfigurationRequest: " << call.msg << "\nwith messageId: " << call.uniqueId;

    ChangeConfigurationResponse response;
    // when reconnect or switching security profile the response has to be sent before that
    bool responded = false;

    auto kv = this->configuration->get(call.msg.key);
    if (kv || call.msg.key == "AuthorizationKey") {
        if (call.msg.key != "AuthorizationKey" && kv.value().readonly) {
            // supported but could not be changed
            response.status = ConfigurationStatus::Rejected;
        } else {
            // TODO(kai): how to signal RebootRequired? or what does need reboot required?
            response.status = this->configuration->set(call.msg.key, call.msg.value);
            if (response.status == ConfigurationStatus::Accepted) {
                if (call.msg.key == "HeartbeatInterval") {
                    this->update_heartbeat_interval();
                } else if (call.msg.key == "MeterValueSampleInterval") {
                    this->update_meter_values_sample_interval();
                } else if (call.msg.key == "ClockAlignedDataInterval") {
                    this->update_clock_aligned_meter_values_interval();
                } else if (call.msg.key == "AuthorizationKey") {
                    /*SECURITYLOG*/ EVLOG_debug << "AuthorizationKey was changed by central system";
                    if (this->configuration->getSecurityProfile() == 0) {
                        EVLOG_debug << "AuthorizationKey was changed while on security profile 0.";
                    } else if (this->configuration->getSecurityProfile() == 1 ||
                               this->configuration->getSecurityProfile() == 2) {
                        EVLOG_debug
                            << "AuthorizationKey was changed while on security profile 1 or 2. Reconnect Websocket.";
                        ocpp::CallResult<ChangeConfigurationResponse> call_result(response, call.uniqueId);
                        this->send<ChangeConfigurationResponse>(call_result);
                        responded = true;
                        this->websocket->reconnect(std::error_code(), 1000);
                    } else {
                        EVLOG_debug << "AuthorizationKey was changed while on security profile 3. Nothing to do.";
                    }
                    // what if basic auth is not in use? what if client side certificates are in use?
                    // log change in security log - if we have one yet?!
                } else if (call.msg.key == "SecurityProfile") {
                    ocpp::CallResult<ChangeConfigurationResponse> call_result(response, call.uniqueId);
                    this->send<ChangeConfigurationResponse>(call_result);
                    int32_t security_profile = std::stoi(call.msg.value);
                    responded = true;
                    this->switch_security_profile_callback = [this, security_profile]() {
                        this->switchSecurityProfile(security_profile);
                    };
                    // disconnected_callback will trigger security_profile_callback when it is set
                    this->websocket->disconnect(websocketpp::close::status::service_restart);
                } else if (call.msg.key == "ConnectionTimeout") {
                    this->set_connection_timeout_callback(this->configuration->getConnectionTimeOut());
                } else if (call.msg.key == "TransactionMessageAttempts") {
                    this->message_queue->update_transaction_message_attempts(
                        this->configuration->getTransactionMessageAttempts());
                } else if (call.msg.key == "TransactionMessageRetryInterval") {
                    this->message_queue->update_transaction_message_retry_interval(
                        this->configuration->getTransactionMessageRetryInterval());
                }
            }
        }
    } else {
        response.status = ConfigurationStatus::NotSupported;
    }

    if (!responded) {
        ocpp::CallResult<ChangeConfigurationResponse> call_result(response, call.uniqueId);
        this->send<ChangeConfigurationResponse>(call_result);
    }
}

void ChargePoint::switchSecurityProfile(int32_t new_security_profile) {
    EVLOG_info << "Switching security profile from " << this->configuration->getSecurityProfile() << " to "
               << new_security_profile;

    this->init_websocket(new_security_profile);
    this->switch_security_profile_callback = [this]() {
        EVLOG_warning << "Switching security profile back to fallback because new profile couldnt connect";
        this->switchSecurityProfile(this->configuration->getSecurityProfile());
    };

    // connection will only try to be established once. If a connection for this security profile cant be established,
    // we'll switch back to the old security profile
    this->websocket->connect(new_security_profile, true);
}

void ChargePoint::handleClearCacheRequest(ocpp::Call<ClearCacheRequest> call) {
    EVLOG_debug << "Received ClearCacheRequest: " << call.msg << "\nwith messageId: " << call.uniqueId;

    ClearCacheResponse response;

    if (this->configuration->getAuthorizationCacheEnabled()) {
        this->database_handler->clear_authorization_cache();
        response.status = ClearCacheStatus::Accepted;
    } else {
        response.status = ClearCacheStatus::Rejected;
    }

    ocpp::CallResult<ClearCacheResponse> call_result(response, call.uniqueId);
    this->send<ClearCacheResponse>(call_result);
}

void ChargePoint::handleDataTransferRequest(ocpp::Call<DataTransferRequest> call) {
    EVLOG_debug << "Received DataTransferRequest: " << call.msg << "\nwith messageId: " << call.uniqueId;

    DataTransferResponse response;

    auto vendorId = call.msg.vendorId.get();
    auto messageId = call.msg.messageId.get_value_or(CiString<50>()).get();
    std::function<void(const std::string data)> callback;
    {
        std::lock_guard<std::mutex> lock(data_transfer_callbacks_mutex);
        if (this->data_transfer_callbacks.count(vendorId) == 0) {
            response.status = DataTransferStatus::UnknownVendorId;
        } else {
            if (this->data_transfer_callbacks[vendorId].count(messageId) == 0) {
                response.status = DataTransferStatus::UnknownMessageId;
            } else {
                response.status = DataTransferStatus::Accepted;
                callback = this->data_transfer_callbacks[vendorId][messageId];
            }
        }
    }

    ocpp::CallResult<DataTransferResponse> call_result(response, call.uniqueId);
    this->send<DataTransferResponse>(call_result);

    if (response.status == DataTransferStatus::Accepted) {
        callback(call.msg.data.get_value_or(std::string("")));
    }
}

void ChargePoint::handleGetConfigurationRequest(ocpp::Call<GetConfigurationRequest> call) {
    EVLOG_debug << "Received GetConfigurationRequest: " << call.msg << "\nwith messageId: " << call.uniqueId;

    GetConfigurationResponse response;
    std::vector<KeyValue> configurationKey;
    std::vector<CiString<50>> unknownKey;

    if (!call.msg.key) {
        EVLOG_debug << "empty request, sending all configuration keys...";
        configurationKey = this->configuration->get_all_key_value();
    } else {
        auto keys = call.msg.key.value();
        if (keys.empty()) {
            EVLOG_debug << "key field is empty, sending all configuration keys...";
            configurationKey = this->configuration->get_all_key_value();
        } else {
            EVLOG_debug << "specific requests for some keys";
            for (const auto& key : call.msg.key.value()) {
                EVLOG_debug << "retrieving key: " << key;
                auto kv = this->configuration->get(key);
                if (kv) {
                    configurationKey.push_back(kv.value());
                } else {
                    unknownKey.push_back(key);
                }
            }
        }
    }

    if (!configurationKey.empty()) {
        response.configurationKey.emplace(configurationKey);
    }
    if (!unknownKey.empty()) {
        response.unknownKey.emplace(unknownKey);
    }

    ocpp::CallResult<GetConfigurationResponse> call_result(response, call.uniqueId);
    this->send<GetConfigurationResponse>(call_result);
}

void ChargePoint::handleRemoteStartTransactionRequest(ocpp::Call<RemoteStartTransactionRequest> call) {
    EVLOG_debug << "Received RemoteStartTransactionRequest: " << call.msg << "\nwith messageId: " << call.uniqueId;

    // a charge point may reject a remote start transaction request without a connectorId
    // TODO(kai): what is our policy here? reject for now
    RemoteStartTransactionResponse response;
    if (call.msg.connectorId) {
        if (call.msg.connectorId.get() == 0) {
            EVLOG_warning << "Received RemoteStartTransactionRequest with connector id 0";
            response.status = RemoteStartStopStatus::Rejected;
            ocpp::CallResult<RemoteStartTransactionResponse> call_result(response, call.uniqueId);
            this->send<RemoteStartTransactionResponse>(call_result);
            return;
        }
        int32_t connector = call.msg.connectorId.value();
        if (this->database_handler->get_connector_availability(connector) == AvailabilityType::Inoperative) {
            EVLOG_warning << "Received RemoteStartTransactionRequest for inoperative connector";
            response.status = RemoteStartStopStatus::Rejected;
            ocpp::CallResult<RemoteStartTransactionResponse> call_result(response, call.uniqueId);
            this->send<RemoteStartTransactionResponse>(call_result);
            return;
        }
        if (this->transaction_handler->get_transaction(connector) != nullptr ||
            this->status->get_state(connector) == ChargePointStatus::Finishing) {
            EVLOG_debug
                << "Received RemoteStartTransactionRequest for a connector with an active or finished transaction.";
            response.status = RemoteStartStopStatus::Rejected;
            ocpp::CallResult<RemoteStartTransactionResponse> call_result(response, call.uniqueId);
            this->send<RemoteStartTransactionResponse>(call_result);
            return;
        }
    }
    if (call.msg.chargingProfile) {
        // TODO(kai): A charging profile was provided, forward to the charger
        if (call.msg.connectorId &&
            call.msg.chargingProfile.value().chargingProfilePurpose == ChargingProfilePurposeType::TxProfile &&
            this->smart_charging_handler->validate_profile(
                call.msg.chargingProfile.value(), call.msg.connectorId.value(), true,
                this->configuration->getChargeProfileMaxStackLevel(),
                this->configuration->getMaxChargingProfilesInstalled(),
                this->configuration->getChargingScheduleMaxPeriods(),
                this->configuration->getChargingScheduleAllowedChargingRateUnitVector())) {
            this->smart_charging_handler->add_tx_profile(call.msg.chargingProfile.value(),
                                                         call.msg.connectorId.value());
        } else {
            response.status = RemoteStartStopStatus::Rejected;
            ocpp::CallResult<RemoteStartTransactionResponse> call_result(response, call.uniqueId);
            this->send<RemoteStartTransactionResponse>(call_result);
            return;
        }
    }

    {
        std::lock_guard<std::mutex> lock(remote_start_transaction_mutex);
        std::vector<int32_t> referenced_connectors;

        if (!call.msg.connectorId) {
            for (int connector = 1; connector <= this->configuration->getNumberOfConnectors(); connector++) {
                referenced_connectors.push_back(connector);
            }
        } else {
            referenced_connectors.push_back(call.msg.connectorId.value());
        }

        response.status = RemoteStartStopStatus::Accepted;
        ocpp::CallResult<RemoteStartTransactionResponse> call_result(response, call.uniqueId);
        this->send<RemoteStartTransactionResponse>(call_result);

        if (this->configuration->getAuthorizeRemoteTxRequests()) {
            this->provide_token_callback(call.msg.idTag.get(), referenced_connectors, false);
        } else {
            this->provide_token_callback(call.msg.idTag.get(), referenced_connectors, true); // prevalidated
        }
    };
}

bool ChargePoint::validate_against_cache_entries(CiString<20> id_tag) {
    const auto cache_entry_opt = this->database_handler->get_authorization_cache_entry(id_tag);
    if (cache_entry_opt.has_value()) {
        auto cache_entry = cache_entry_opt.value();
        const auto expiry_date_opt = cache_entry.expiryDate;

        if (cache_entry.status == AuthorizationStatus::Accepted) {
            if (expiry_date_opt.has_value()) {
                const auto expiry_date = expiry_date_opt.value();
                if (expiry_date < ocpp::DateTime()) {
                    cache_entry.status = AuthorizationStatus::Expired;
                    this->database_handler->insert_or_update_authorization_cache_entry(id_tag, cache_entry);
                    return false;
                } else {
                    return true;
                }
            } else {
                return true;
            }
        } else {
            return false;
        }
    } else {
        return false;
    }
}

void ChargePoint::handleRemoteStopTransactionRequest(ocpp::Call<RemoteStopTransactionRequest> call) {
    EVLOG_debug << "Received RemoteStopTransactionRequest: " << call.msg << "\nwith messageId: " << call.uniqueId;

    RemoteStopTransactionResponse response;
    response.status = RemoteStartStopStatus::Rejected;

    auto connector = this->transaction_handler->get_connector_from_transaction_id(call.msg.transactionId);
    if (connector > 0) {
        response.status = RemoteStartStopStatus::Accepted;
    }

    ocpp::CallResult<RemoteStopTransactionResponse> call_result(response, call.uniqueId);
    this->send<RemoteStopTransactionResponse>(call_result);

    if (connector > 0) {
        this->stop_transaction_callback(connector, Reason::Remote);
    }
}

void ChargePoint::handleResetRequest(ocpp::Call<ResetRequest> call) {
    EVLOG_debug << "Received ResetRequest: " << call.msg << "\nwith messageId: " << call.uniqueId;

    const auto reset_type = call.msg.type;
    ResetResponse response;

    if (this->is_reset_allowed_callback == nullptr || this->reset_callback == nullptr ||
        !this->is_reset_allowed_callback(reset_type)) {
        response.status = ResetStatus::Rejected;
    } else {
        // reset is allowed
        response.status = ResetStatus::Accepted;
    }

    // send response
    ocpp::CallResult<ResetResponse> call_result(response, call.uniqueId);
    this->send<ResetResponse>(call_result);

    if (response.status == ResetStatus::Accepted) {
        // gracefully stop all transactions and send StopTransaction. Restart software afterwards
        this->reset_thread = std::thread([this, reset_type]() {
            EVLOG_debug << "Waiting until all transactions are stopped...";
            std::unique_lock lk(this->stop_transaction_mutex);
            this->stop_transaction_cv.wait_for(lk, std::chrono::seconds(5), [this] {
                for (int32_t connector = 1; connector <= this->configuration->getNumberOfConnectors(); connector++) {
                    if (this->transaction_handler->transaction_active(connector)) {
                        return false;
                    }
                }
                return true;
            });
            // this is executed after all transactions have been stopped
            this->stop();
            this->reset_callback(reset_type);
        });
        if (call.msg.type == ResetType::Soft) {
            this->stop_all_transactions(Reason::SoftReset);
        } else {
            this->stop_all_transactions(Reason::HardReset);
        }
    }
}

void ChargePoint::handleStartTransactionResponse(ocpp::CallResult<StartTransactionResponse> call_result) {

    StartTransactionResponse start_transaction_response = call_result.msg;

    const auto transaction = this->transaction_handler->get_transaction(call_result.uniqueId);

    // this can happen when a chargepoint was offline during transaction and StopTransaction.req is already queued
    if (transaction->is_finished()) {
        this->message_queue->add_stopped_transaction_id(transaction->get_stop_transaction_message_id(),
                                                        start_transaction_response.transactionId);
    }
    this->message_queue->notify_start_transaction_handled();
    int32_t connector = transaction->get_connector();
    transaction->set_transaction_id(start_transaction_response.transactionId);

    this->database_handler->update_transaction(transaction->get_session_id(), start_transaction_response.transactionId,
                                               call_result.msg.idTagInfo.parentIdTag);

    auto idTag = transaction->get_id_tag();
    this->database_handler->insert_or_update_authorization_cache_entry(idTag, start_transaction_response.idTagInfo);

    if (start_transaction_response.idTagInfo.status != AuthorizationStatus::Accepted) {
        this->pause_charging_callback(connector);
        if (this->configuration->getStopTransactionOnInvalidId()) {
            this->stop_transaction_callback(connector, Reason::DeAuthorized);
        }
    }
}

void ChargePoint::handleStopTransactionResponse(ocpp::CallResult<StopTransactionResponse> call_result) {

    StopTransactionResponse stop_transaction_response = call_result.msg;

    // TODO(piet): Fix this for multiple connectors;
    int32_t connector = 1;

    if (stop_transaction_response.idTagInfo) {
        auto id_tag = this->transaction_handler->get_authorized_id_tag(call_result.uniqueId.get());
        if (id_tag) {
            this->database_handler->insert_or_update_authorization_cache_entry(
                id_tag.value(), stop_transaction_response.idTagInfo.value());
        }
    }

    // perform a queued connector availability change
    bool change_queued = false;
    AvailabilityType connector_availability;
    {
        std::lock_guard<std::mutex> change_availability_lock(change_availability_mutex);
        change_queued = this->change_availability_queue.count(connector) != 0;
        connector_availability = this->change_availability_queue[connector];
        this->change_availability_queue.erase(connector);
    }

    if (change_queued) {
        this->database_handler->insert_or_update_connector_availability(connector, connector_availability);
        EVLOG_debug << "Queued availability change of connector " << connector << " to "
                    << conversions::availability_type_to_string(connector_availability);

        if (connector_availability == AvailabilityType::Operative) {
            if (this->enable_evse_callback != nullptr) {
                // TODO(kai): check return value
                this->enable_evse_callback(connector);
            }
            this->status->submit_event(connector, Event_BecomeAvailable());
        } else {
            if (this->disable_evse_callback != nullptr) {
                // TODO(kai): check return value
                this->disable_evse_callback(connector);
            }
            this->status->submit_event(connector, Event_ChangeAvailabilityToUnavailable());
        }
    }
    this->transaction_handler->erase_stopped_transaction(call_result.uniqueId.get());
    // when this transaction was stopped because of a Reset.req this signals that StopTransaction.conf has been received
    this->stop_transaction_cv.notify_one();
}

void ChargePoint::handleUnlockConnectorRequest(ocpp::Call<UnlockConnectorRequest> call) {
    EVLOG_debug << "Received UnlockConnectorRequest: " << call.msg << "\nwith messageId: " << call.uniqueId;
    std::lock_guard<std::mutex> lock(this->stop_transaction_mutex);

    UnlockConnectorResponse response;
    auto connector = call.msg.connectorId;
    if (connector == 0 || connector > this->configuration->getNumberOfConnectors()) {
        response.status = UnlockStatus::NotSupported;
    } else {
        // this message is not intended to remotely stop a transaction, but if a transaction is still ongoing it is
        // advised to stop it first
        CiString<20> idTag;
        int32_t transactionId;
        if (this->transaction_handler->transaction_active(connector)) {
            EVLOG_info << "Received unlock connector request with active session for this connector.";
            this->stop_transaction_callback(connector, Reason::UnlockCommand);
        }

        if (this->unlock_connector_callback != nullptr) {
            if (this->unlock_connector_callback(call.msg.connectorId)) {
                response.status = UnlockStatus::Unlocked;
            } else {
                response.status = UnlockStatus::UnlockFailed;
            }
        } else {
            response.status = UnlockStatus::NotSupported;
        }
    }

    ocpp::CallResult<UnlockConnectorResponse> call_result(response, call.uniqueId);
    this->send<UnlockConnectorResponse>(call_result);
}

void ChargePoint::handleSetChargingProfileRequest(ocpp::Call<SetChargingProfileRequest> call) {
    EVLOG_debug << "Received SetChargingProfileRequest: " << call.msg << "\nwith messageId: " << call.uniqueId;

    // FIXME(kai): after a new profile has been installed we must notify interested parties (energy manager?)

    SetChargingProfileResponse response;
    response.status = ChargingProfileStatus::Rejected;

    auto profile = call.msg.csChargingProfiles;
    const int connector_id = call.msg.connectorId;

    const auto supported_purpose_types = this->configuration->getSupportedChargingProfilePurposeTypes();
    if (std::find(supported_purpose_types.begin(), supported_purpose_types.end(),
                  call.msg.csChargingProfiles.chargingProfilePurpose) == supported_purpose_types.end()) {
        EVLOG_warning << "Rejecting SetChargingProfileRequest because purpose type is not supported: "
                      << call.msg.csChargingProfiles.chargingProfilePurpose;
        response.status = ChargingProfileStatus::Rejected;
    } else if (this->smart_charging_handler->validate_profile(
                   profile, connector_id, false, this->configuration->getChargeProfileMaxStackLevel(),
                   this->configuration->getMaxChargingProfilesInstalled(),
                   this->configuration->getChargingScheduleMaxPeriods(),
                   this->configuration->getChargingScheduleAllowedChargingRateUnitVector())) {
        response.status = ChargingProfileStatus::Accepted;
        // If a charging profile with the same chargingProfileId, or the same combination of stackLevel /
        // ChargingProfilePurpose, exists on the Charge Point, the new charging profile SHALL replace the
        // existing charging profile, otherwise it SHALL be added.
        this->smart_charging_handler->clear_all_profiles_with_filter(profile.chargingProfileId, boost::none,
                                                                     boost::none, boost::none, true);
        if (profile.chargingProfilePurpose == ChargingProfilePurposeType::ChargePointMaxProfile) {
            this->smart_charging_handler->add_charge_point_max_profile(profile);
        } else if (profile.chargingProfilePurpose == ChargingProfilePurposeType::TxDefaultProfile) {
            this->smart_charging_handler->add_tx_default_profile(profile, connector_id);
        } else if (profile.chargingProfilePurpose == ChargingProfilePurposeType::TxProfile) {
            this->smart_charging_handler->add_tx_profile(profile, connector_id);
        }
        response.status = ChargingProfileStatus::Accepted;
    } else {
        response.status = ChargingProfileStatus::Rejected;
    }

    ocpp::CallResult<SetChargingProfileResponse> call_result(response, call.uniqueId);
    this->send<SetChargingProfileResponse>(call_result);

    if (response.status == ChargingProfileStatus::Accepted) {
        this->signal_set_charging_profiles_callback();
    }
}

void ChargePoint::handleGetCompositeScheduleRequest(ocpp::Call<GetCompositeScheduleRequest> call) {
    EVLOG_debug << "Received GetCompositeScheduleRequest: " << call.msg << "\nwith messageId: " << call.uniqueId;

    GetCompositeScheduleResponse response;

    const auto connector_id = call.msg.connectorId;
    const auto allowed_charging_rate_units = this->configuration->getChargingScheduleAllowedChargingRateUnitVector();

    if (connector_id >= this->connectors.size() or connector_id < 0) {
        response.status = GetCompositeScheduleStatus::Rejected;
    } else if (call.msg.chargingRateUnit and
               std::find(allowed_charging_rate_units.begin(), allowed_charging_rate_units.end(),
                         call.msg.chargingRateUnit.value()) == allowed_charging_rate_units.end()) {
        EVLOG_warning << "GetCompositeScheduleRequest: ChargingRateUnit not allowed";
        response.status = GetCompositeScheduleStatus::Rejected;
    } else {
        const auto start_time = ocpp::DateTime(std::chrono::floor<std::chrono::seconds>(date::utc_clock::now()));
        if (call.msg.duration > this->configuration->getMaxCompositeScheduleDuration()) {
            EVLOG_warning << "GetCompositeScheduleRequest: Requested duration of " << call.msg.duration << "s"
                          << " is bigger than configured maximum value of "
                          << this->configuration->getMaxCompositeScheduleDuration() << "s";
        }
        const auto duration = std::min(this->configuration->getMaxCompositeScheduleDuration(), call.msg.duration);
        const auto end_time = ocpp::DateTime(start_time.to_time_point() + std::chrono::seconds(duration));
        const auto valid_profiles =
            this->smart_charging_handler->get_valid_profiles(start_time, end_time, connector_id);

        const auto composite_schedule = this->smart_charging_handler->calculate_composite_schedule(
            valid_profiles, start_time, end_time, connector_id, call.msg.chargingRateUnit);
        response.status = GetCompositeScheduleStatus::Accepted;
        response.connectorId = connector_id;
        response.scheduleStart = start_time;
        response.chargingSchedule = composite_schedule;
    }

    ocpp::CallResult<GetCompositeScheduleResponse> call_result(response, call.uniqueId);
    this->send<GetCompositeScheduleResponse>(call_result);
}

void ChargePoint::handleClearChargingProfileRequest(ocpp::Call<ClearChargingProfileRequest> call) {
    EVLOG_debug << "Received ClearChargingProfileRequest: " << call.msg << "\nwith messageId: " << call.uniqueId;

    // FIXME(kai): after a profile has been deleted we must notify interested parties (energy manager?)

    ClearChargingProfileResponse response;
    response.status = ClearChargingProfileStatus::Unknown;

    // clear all charging profiles
    if (!call.msg.id && !call.msg.connectorId && !call.msg.chargingProfilePurpose && !call.msg.stackLevel) {
        this->smart_charging_handler->clear_all_profiles();
        response.status = ClearChargingProfileStatus::Accepted;
    } else if (call.msg.id &&
               this->smart_charging_handler->clear_all_profiles_with_filter(
                   call.msg.id, call.msg.connectorId, call.msg.stackLevel, call.msg.chargingProfilePurpose, true)) {
        response.status = ClearChargingProfileStatus::Accepted;

    } else if (this->smart_charging_handler->clear_all_profiles_with_filter(
                   call.msg.id, call.msg.connectorId, call.msg.stackLevel, call.msg.chargingProfilePurpose, false)) {
        response.status = ClearChargingProfileStatus::Accepted;
    }

    ocpp::CallResult<ClearChargingProfileResponse> call_result(response, call.uniqueId);
    this->send<ClearChargingProfileResponse>(call_result);
}

void ChargePoint::handleTriggerMessageRequest(ocpp::Call<TriggerMessageRequest> call) {
    EVLOG_debug << "Received TriggerMessageRequest: " << call.msg << "\nwith messageId: " << call.uniqueId;

    TriggerMessageResponse response;
    response.status = TriggerMessageStatus::Rejected;
    switch (call.msg.requestedMessage) {
    case MessageTrigger::BootNotification:
        response.status = TriggerMessageStatus::Accepted;
        break;
    case MessageTrigger::DiagnosticsStatusNotification:
        response.status = TriggerMessageStatus::Accepted;
        break;
    case MessageTrigger::FirmwareStatusNotification:
        response.status = TriggerMessageStatus::Accepted;
        break;
    case MessageTrigger::Heartbeat:
        response.status = TriggerMessageStatus::Accepted;
        break;
    case MessageTrigger::MeterValues:
        response.status = TriggerMessageStatus::Accepted;
        break;
    case MessageTrigger::StatusNotification:
        response.status = TriggerMessageStatus::Accepted;
        break;
    }

    auto connector = call.msg.connectorId.value_or(0);
    bool valid = true;
    if (connector < 0 || connector > this->configuration->getNumberOfConnectors()) {
        response.status = TriggerMessageStatus::Rejected;
        valid = false;
    }

    ocpp::CallResult<TriggerMessageResponse> call_result(response, call.uniqueId);
    this->send<TriggerMessageResponse>(call_result);

    if (!valid) {
        return;
    }

    switch (call.msg.requestedMessage) {
    case MessageTrigger::BootNotification:
        this->boot_notification();
        break;
    case MessageTrigger::DiagnosticsStatusNotification:
        this->diagnostic_status_notification(this->diagnostics_status);
        break;
    case MessageTrigger::FirmwareStatusNotification:
        this->firmware_status_notification(this->firmware_status);
        break;
    case MessageTrigger::Heartbeat:
        this->heartbeat();
        break;
    case MessageTrigger::MeterValues:
        this->send_meter_value(
            connector, this->get_latest_meter_value(connector, this->configuration->getMeterValuesSampledDataVector(),
                                                    ReadingContext::Trigger));
        break;
    case MessageTrigger::StatusNotification:
        this->status_notification(connector, ChargePointErrorCode::NoError, this->status->get_state(connector));
        break;
    }
}

void ChargePoint::handleGetDiagnosticsRequest(ocpp::Call<GetDiagnosticsRequest> call) {
    EVLOG_debug << "Received GetDiagnosticsRequest: " << call.msg << "\nwith messageId: " << call.uniqueId;
    GetDiagnosticsResponse response;
    if (this->upload_diagnostics_callback) {
        const auto get_log_response = this->upload_diagnostics_callback(call.msg);
        if (get_log_response.filename.has_value()) {
            response.fileName = get_log_response.filename.value();
        }
    }
    ocpp::CallResult<GetDiagnosticsResponse> call_result(response, call.uniqueId);
    this->send<GetDiagnosticsResponse>(call_result);
}

void ChargePoint::handleUpdateFirmwareRequest(ocpp::Call<UpdateFirmwareRequest> call) {
    EVLOG_debug << "Received UpdateFirmwareRequest: " << call.msg << "\nwith messageId: " << call.uniqueId;
    UpdateFirmwareResponse response;
    if (this->update_firmware_callback) {
        this->update_firmware_callback(call.msg);
    }
    ocpp::CallResult<UpdateFirmwareResponse> call_result(response, call.uniqueId);
    this->send<UpdateFirmwareResponse>(call_result);
}

void ChargePoint::handleExtendedTriggerMessageRequest(ocpp::Call<ExtendedTriggerMessageRequest> call) {
    EVLOG_debug << "Received ExtendedTriggerMessageRequest: " << call.msg << "\nwith messageId: " << call.uniqueId;

    ExtendedTriggerMessageResponse response;
    response.status = TriggerMessageStatusEnumType::Rejected;
    switch (call.msg.requestedMessage) {
    case MessageTriggerEnumType::BootNotification:
        response.status = TriggerMessageStatusEnumType::Accepted;
        break;
    case MessageTriggerEnumType::FirmwareStatusNotification:
        response.status = TriggerMessageStatusEnumType::Accepted;
        break;
    case MessageTriggerEnumType::Heartbeat:
        response.status = TriggerMessageStatusEnumType::Accepted;
        break;
    case MessageTriggerEnumType::LogStatusNotification:
        response.status = TriggerMessageStatusEnumType::Accepted;
        break;
    case MessageTriggerEnumType::MeterValues:
        response.status = TriggerMessageStatusEnumType::Accepted;
        break;
    case MessageTriggerEnumType::SignChargePointCertificate:
        if (this->configuration->getCpoName() != boost::none) {
            response.status = TriggerMessageStatusEnumType::Accepted;
        } else {
            EVLOG_warning << "Received ExtendedTriggerMessage with SignChargePointCertificate but no "
                             "CpoName is set.";
        }
        break;
    case MessageTriggerEnumType::StatusNotification:
        response.status = TriggerMessageStatusEnumType::Accepted;
        break;
    }

    auto connector = call.msg.connectorId.value_or(0);
    bool valid = true;
    if (response.status == TriggerMessageStatusEnumType::Rejected || connector < 0 ||
        connector > this->configuration->getNumberOfConnectors()) {
        response.status = TriggerMessageStatusEnumType::Rejected;
        valid = false;
    }

    ocpp::CallResult<ExtendedTriggerMessageResponse> call_result(response, call.uniqueId);
    this->send<ExtendedTriggerMessageResponse>(call_result);

    if (!valid) {
        return;
    }

    switch (call.msg.requestedMessage) {
    case MessageTriggerEnumType::BootNotification:
        this->boot_notification();
        break;
    case MessageTriggerEnumType::FirmwareStatusNotification:
        this->signed_firmware_update_status_notification(this->signed_firmware_status,
                                                         this->signed_firmware_status_request_id);
        break;
    case MessageTriggerEnumType::Heartbeat:
        this->heartbeat();
        break;
    case MessageTriggerEnumType::LogStatusNotification:
        this->log_status_notification(this->log_status, this->log_status_request_id);
        break;
    case MessageTriggerEnumType::MeterValues:
        this->send_meter_value(
            connector, this->get_latest_meter_value(connector, this->configuration->getMeterValuesSampledDataVector(),
                                                    ReadingContext::Trigger));
        break;
    case MessageTriggerEnumType::SignChargePointCertificate:
        this->sign_certificate();
        break;
    case MessageTriggerEnumType::StatusNotification:
        this->status_notification(connector, ChargePointErrorCode::NoError, this->status->get_state(connector));
        break;
    }
}

void ChargePoint::sign_certificate() {

    SignCertificateRequest req;

    std::string csr =
        this->pki_handler->generateCsr("DE", "BW", "Bad Schoenborn", this->configuration->getCpoName().get().c_str(),
                                       this->configuration->getChargeBoxSerialNumber().c_str());

    req.csr = csr;
    ocpp::Call<SignCertificateRequest> call(req, this->message_queue->createMessageId());
    this->send<SignCertificateRequest>(call);
}

void ChargePoint::handleCertificateSignedRequest(ocpp::Call<CertificateSignedRequest> call) {
    EVLOG_debug << "Received CertificateSignedRequest: " << call.msg << "\nwith messageId: " << call.uniqueId;

    CertificateSignedResponse response;
    response.status = CertificateSignedStatusEnumType::Rejected;

    std::string certificateChain = call.msg.certificateChain;

    CertificateVerificationResult certificateVerificationResult = this->pki_handler->verifyChargepointCertificate(
        certificateChain, this->configuration->getChargeBoxSerialNumber());

    if (certificateVerificationResult == CertificateVerificationResult::Valid) {
        response.status = CertificateSignedStatusEnumType::Accepted;
        // FIXME(piet): dont just override, store other one for at least one month according to spec
        this->pki_handler->writeClientCertificate(certificateChain);
    }

    ocpp::CallResult<CertificateSignedResponse> call_result(response, call.uniqueId);
    this->send<CertificateSignedResponse>(call_result);

    if (response.status == CertificateSignedStatusEnumType::Rejected) {
        this->securityEventNotification(
            SecurityEvent::InvalidChargePointCertificate,
            ocpp::conversions::certificate_verification_result_to_string(certificateVerificationResult));
    }

    // reconnect with new certificate if valid and security profile is 3
    if (response.status == CertificateSignedStatusEnumType::Accepted &&
        this->configuration->getSecurityProfile() == 3) {
        if (this->pki_handler->validIn(certificateChain) < 0) {
            this->websocket->reconnect(std::error_code(), 1000);
        } else {
            this->websocket->reconnect(std::error_code(), this->pki_handler->validIn(certificateChain));
        }
    }
}

void ChargePoint::handleGetInstalledCertificateIdsRequest(ocpp::Call<GetInstalledCertificateIdsRequest> call) {
    EVLOG_debug << "Received GetInstalledCertificatesRequest: " << call.msg << "\nwith messageId: " << call.uniqueId;
    GetInstalledCertificateIdsResponse response;
    response.status = GetInstalledCertificateStatusEnumType::NotFound;

    // this is common CertificateHashData type
    const auto certificate_hash_data =
        this->pki_handler->getRootCertificateHashData(ocpp::conversions::string_to_certificate_type(
            conversions::certificate_use_enum_type_to_string(call.msg.certificateType)));
    if (certificate_hash_data != boost::none) {
        // convert common CertificateHashData to 1.6 CertificateHashData
        boost::optional<std::vector<CertificateHashDataType>> certificate_hash_data_16_vec_opt;
        std::vector<CertificateHashDataType> certificate_hash_data_16_vec;
        for (const auto certificate_hash_data : certificate_hash_data.value()) {
            certificate_hash_data_16_vec.push_back(CertificateHashDataType(json(certificate_hash_data)));
        }
        certificate_hash_data_16_vec_opt.emplace(certificate_hash_data_16_vec);
        response.certificateHashData = certificate_hash_data_16_vec_opt;
        response.status = GetInstalledCertificateStatusEnumType::Accepted;
    }

    ocpp::CallResult<GetInstalledCertificateIdsResponse> call_result(response, call.uniqueId);
    this->send<GetInstalledCertificateIdsResponse>(call_result);
}

void ChargePoint::handleDeleteCertificateRequest(ocpp::Call<DeleteCertificateRequest> call) {
    DeleteCertificateResponse response;

    // convert 1.6 CertificateHashData to common CertificateHashData
    ocpp::CertificateHashDataType certificate_hash_data(json(call.msg.certificateHashData));

    response.status = conversions::string_to_delete_certificate_status_enum_type(
        ocpp::conversions::delete_certificate_result_to_string(this->pki_handler->deleteRootCertificate(
            certificate_hash_data, this->configuration->getSecurityProfile())));

    ocpp::CallResult<DeleteCertificateResponse> call_result(response, call.uniqueId);
    this->send<DeleteCertificateResponse>(call_result);
}

void ChargePoint::handleInstallCertificateRequest(ocpp::Call<InstallCertificateRequest> call) {
    InstallCertificateResponse response;
    response.status = InstallCertificateStatusEnumType::Rejected;

    InstallCertificateResult installCertificateResult = this->pki_handler->installRootCertificate(
        call.msg.certificate.get(),
        ocpp::conversions::string_to_certificate_type(
            conversions::certificate_use_enum_type_to_string(call.msg.certificateType)),
        this->configuration->getCertificateStoreMaxLength(), this->configuration->getAdditionalRootCertificateCheck());

    if (installCertificateResult == InstallCertificateResult::Ok ||
        installCertificateResult == InstallCertificateResult::Valid) {
        response.status = InstallCertificateStatusEnumType::Accepted;
    } else if (installCertificateResult == InstallCertificateResult::WriteError) {
        response.status = InstallCertificateStatusEnumType::Failed;
    }

    ocpp::CallResult<InstallCertificateResponse> call_result(response, call.uniqueId);
    this->send<InstallCertificateResponse>(call_result);

    if (response.status == InstallCertificateStatusEnumType::Rejected) {
        this->securityEventNotification(
            SecurityEvent::InvalidCentralSystemCertificate,
            ocpp::conversions::install_certificate_result_to_string(installCertificateResult));
    }
}

void ChargePoint::handleGetLogRequest(ocpp::Call<GetLogRequest> call) {
    GetLogResponse response;

    if (this->upload_logs_callback) {

        const auto get_log_response = this->upload_logs_callback(call.msg);
        response.status = get_log_response.status;
        response.filename = get_log_response.filename;
    }

    ocpp::CallResult<GetLogResponse> call_result(response, call.uniqueId);
    this->send<GetLogResponse>(call_result);
}

void ChargePoint::handleSignedUpdateFirmware(ocpp::Call<SignedUpdateFirmwareRequest> call) {
    EVLOG_debug << "Received SignedUpdateFirmwareRequest: " << call.msg << "\nwith messageId: " << call.uniqueId;
    SignedUpdateFirmwareResponse response;

    if (!this->pki_handler->verifyFirmwareCertificate(call.msg.firmware.signingCertificate.get())) {
        response.status = UpdateFirmwareStatusEnumType::InvalidCertificate;
        ocpp::CallResult<SignedUpdateFirmwareResponse> call_result(response, call.uniqueId);
        this->send<SignedUpdateFirmwareResponse>(call_result);
    } else {
        response.status = this->signed_update_firmware_callback(call.msg);
        ocpp::CallResult<SignedUpdateFirmwareResponse> call_result(response, call.uniqueId);
        this->send<SignedUpdateFirmwareResponse>(call_result);
    }

    if (response.status == UpdateFirmwareStatusEnumType::InvalidCertificate) {
        this->securityEventNotification(SecurityEvent::InvalidFirmwareSigningCertificate, "Certificate is invalid.");
    }
}

void ChargePoint::securityEventNotification(const SecurityEvent& type, const std::string& tech_info) {

    SecurityEventNotificationRequest req;
    req.type = type;
    req.techInfo.emplace(tech_info);
    req.timestamp = ocpp::DateTime();

    ocpp::Call<SecurityEventNotificationRequest> call(req, this->message_queue->createMessageId());
    this->send<SecurityEventNotificationRequest>(call);
}

void ChargePoint::log_status_notification(UploadLogStatusEnumType status, int requestId) {

    EVLOG_debug << "Sending log_status_notification with status: " << status << ", requestId: " << requestId;

    LogStatusNotificationRequest req;
    req.status = status;
    req.requestId = requestId;

    this->log_status = status;
    this->log_status_request_id = requestId;

    ocpp::Call<LogStatusNotificationRequest> call(req, this->message_queue->createMessageId());
    this->send<LogStatusNotificationRequest>(call);
}

void ChargePoint::signed_firmware_update_status_notification(FirmwareStatusEnumType status, int requestId) {
    EVLOG_debug << "Sending FirmwareUpdateStatusNotification";
    SignedFirmwareStatusNotificationRequest req;
    req.status = status;
    req.requestId = requestId;

    this->signed_firmware_status = status;
    this->signed_firmware_status_request_id = requestId;

    ocpp::Call<SignedFirmwareStatusNotificationRequest> call(req, this->message_queue->createMessageId());
    this->send<SignedFirmwareStatusNotificationRequest>(call);

    if (status == FirmwareStatusEnumType::InvalidSignature) {
        this->securityEventNotification(SecurityEvent::InvalidFirmwareSignature, "techinfo");
    }
}

void ChargePoint::handleReserveNowRequest(ocpp::Call<ReserveNowRequest> call) {
    ReserveNowResponse response;
    response.status = ReservationStatus::Rejected;

    if (this->status->get_state(call.msg.connectorId) == ChargePointStatus::Faulted) {
        response.status = ReservationStatus::Faulted;
    } else if (this->reserve_now_callback != nullptr &&
               this->configuration->getSupportedFeatureProfiles().find("Reservation") != std::string::npos) {
        response.status = this->reserve_now_callback(call.msg.reservationId, call.msg.connectorId, call.msg.expiryDate,
                                                     call.msg.idTag, call.msg.parentIdTag);
    }

    ocpp::CallResult<ReserveNowResponse> call_result(response, call.uniqueId);
    this->send<ReserveNowResponse>(call_result);
}

void ChargePoint::handleCancelReservationRequest(ocpp::Call<CancelReservationRequest> call) {
    CancelReservationResponse response;
    response.status = CancelReservationStatus::Rejected;

    if (this->cancel_reservation_callback != nullptr) {
        if (this->cancel_reservation_callback(call.msg.reservationId)) {
            response.status = CancelReservationStatus::Accepted;
        }
    }
    ocpp::CallResult<CancelReservationResponse> call_result(response, call.uniqueId);
    this->send<CancelReservationResponse>(call_result);
}

void ChargePoint::handleSendLocalListRequest(ocpp::Call<SendLocalListRequest> call) {
    EVLOG_debug << "Received SendLocalListRequest: " << call.msg << "\nwith messageId: " << call.uniqueId;

    SendLocalListResponse response;
    response.status = UpdateStatus::Failed;

    if (!this->configuration->getLocalAuthListEnabled()) {
        response.status = UpdateStatus::NotSupported;
    }

    else if (call.msg.updateType == UpdateType::Full) {
        if (call.msg.localAuthorizationList) {
            auto local_auth_list = call.msg.localAuthorizationList.get();
            this->database_handler->clear_local_authorization_list();
            this->database_handler->insert_or_update_local_list_version(call.msg.listVersion);
            this->database_handler->insert_or_update_local_authorization_list(local_auth_list);
        } else {
            this->database_handler->insert_or_update_local_list_version(call.msg.listVersion);
            this->database_handler->clear_local_authorization_list();
        }
        response.status = UpdateStatus::Accepted;
    } else if (call.msg.updateType == UpdateType::Differential) {
        if (call.msg.localAuthorizationList) {
            auto local_auth_list = call.msg.localAuthorizationList.get();
            if (this->database_handler->get_local_list_version() < call.msg.listVersion) {
                this->database_handler->insert_or_update_local_list_version(call.msg.listVersion);
                this->database_handler->insert_or_update_local_authorization_list(local_auth_list);

                response.status = UpdateStatus::Accepted;
            } else {
                response.status = UpdateStatus::VersionMismatch;
            }
        }
    }

    ocpp::CallResult<SendLocalListResponse> call_result(response, call.uniqueId);
    this->send<SendLocalListResponse>(call_result);
}

void ChargePoint::handleGetLocalListVersionRequest(ocpp::Call<GetLocalListVersionRequest> call) {
    EVLOG_debug << "Received GetLocalListVersionRequest: " << call.msg << "\nwith messageId: " << call.uniqueId;

    GetLocalListVersionResponse response;
    if (!this->configuration->getSupportedFeatureProfilesSet().count(
            SupportedFeatureProfiles::LocalAuthListManagement)) {
        // if Local Authorization List is not supported, report back -1 as list version
        response.listVersion = -1;
    } else {
        response.listVersion = this->database_handler->get_local_list_version();
    }

    ocpp::CallResult<GetLocalListVersionResponse> call_result(response, call.uniqueId);
    this->send<GetLocalListVersionResponse>(call_result);
}

bool ChargePoint::allowed_to_send_message(json::array_t message) {
    auto message_type = conversions::string_to_messagetype(message.at(CALL_ACTION));

    if (!this->initialized) {
        // BootNotification and StopTransaction messages can be queued before receiving a BootNotification.conf
        if (message_type == MessageType::BootNotification || message_type == MessageType::StopTransaction) {
            return true;
        }
        return false;
    }

    if (this->registration_status == RegistrationStatus::Rejected) {
        std::chrono::time_point<date::utc_clock> retry_time =
            this->boot_time + std::chrono::seconds(this->configuration->getHeartbeatInterval());
        if (date::utc_clock::now() < retry_time) {
            using date::operator<<;
            std::ostringstream oss;
            oss << "status is rejected and retry time not reached. Messages can be sent again at: " << retry_time;
            EVLOG_debug << oss.str();
            return false;
        }
    } else if (this->registration_status == RegistrationStatus::Pending) {
        // BootNotification and StopTransaction messages can be queued before receiving a BootNotification.conf
        if (message_type == MessageType::BootNotification || message_type == MessageType::StopTransaction) {
            return true;
        }
        return false;
    }
    return true;
}

template <class T> bool ChargePoint::send(ocpp::Call<T> call) {
    if (this->allowed_to_send_message(json(call))) {
        this->message_queue->push(call);
        return true;
    }
    return false;
}

template <class T> std::future<EnhancedMessage<v16::MessageType>> ChargePoint::send_async(ocpp::Call<T> call) {
    return this->message_queue->push_async(call);
}

template <class T> bool ChargePoint::send(ocpp::CallResult<T> call_result) {
    return this->websocket->send(json(call_result).dump());
}

bool ChargePoint::send(CallError call_error) {
    return this->websocket->send(json(call_error).dump());
}

void ChargePoint::status_notification(int32_t connector, ChargePointErrorCode errorCode, CiString<50> info,
                                      ChargePointStatus status, ocpp::DateTime timestamp) {
    StatusNotificationRequest request;
    request.connectorId = connector;
    request.errorCode = errorCode;
    request.info.emplace(info);
    request.status = status;
    request.timestamp.emplace(timestamp);
    ocpp::Call<StatusNotificationRequest> call(request, this->message_queue->createMessageId());
    this->send<StatusNotificationRequest>(call);
}

void ChargePoint::status_notification(int32_t connector, ChargePointErrorCode errorCode, ChargePointStatus status) {
    StatusNotificationRequest request;
    request.connectorId = connector;
    request.errorCode = errorCode;
    request.status = status;
    ocpp::Call<StatusNotificationRequest> call(request, this->message_queue->createMessageId());
    this->send<StatusNotificationRequest>(call);
}

// public API for Core profile

IdTagInfo ChargePoint::authorize_id_token(CiString<20> idTag) {
    // only do authorize req when authorization locally not enabled or fails
    // proritize auth list over auth cache for same idTags

    // Authorize locally (cache or local list) if
    // - LocalPreAuthorize is true and CP is online
    // OR
    // - LocalAuthorizeOffline is true and CP is offline
    if ((this->configuration->getLocalPreAuthorize() && this->websocket->is_connected()) ||
        this->configuration->getLocalAuthorizeOffline() && !this->websocket->is_connected()) {
        if (this->configuration->getLocalAuthListEnabled()) {
            const auto auth_list_entry_opt = this->database_handler->get_local_authorization_list_entry(idTag);
            if (auth_list_entry_opt.has_value()) {
                EVLOG_info << "Found id_tag " << idTag.get() << " in AuthorizationList";
                return auth_list_entry_opt.value();
            }
        }
        if (this->configuration->getAuthorizationCacheEnabled()) {
            if (this->validate_against_cache_entries(idTag)) {
                EVLOG_info << "Found vlaid id_tag " << idTag.get() << " in AuthorizationCache";
                return this->database_handler->get_authorization_cache_entry(idTag).value();
            }
        }
    }

    AuthorizeRequest req;
    req.idTag = idTag;

    ocpp::Call<AuthorizeRequest> call(req, this->message_queue->createMessageId());

    auto authorize_future = this->send_async<AuthorizeRequest>(call);
    auto enhanced_message = authorize_future.get();

    IdTagInfo id_tag_info;
    if (enhanced_message.messageType == MessageType::AuthorizeResponse) {
        ocpp::CallResult<AuthorizeResponse> call_result = enhanced_message.message;
        if (call_result.msg.idTagInfo.status == AuthorizationStatus::Accepted) {
            this->database_handler->insert_or_update_authorization_cache_entry(idTag, call_result.msg.idTagInfo);
        }
        return call_result.msg.idTagInfo;
    } else if (enhanced_message.offline) {
        if (this->configuration->getAllowOfflineTxForUnknownId() != boost::none &&
            this->configuration->getAllowOfflineTxForUnknownId().value()) {
            id_tag_info = {AuthorizationStatus::Accepted, boost::none, boost::none};
            return id_tag_info;
        }
    }
    id_tag_info = {AuthorizationStatus::Invalid, boost::none, boost::none};
    return id_tag_info;
}

std::map<int32_t, ChargingSchedule> ChargePoint::get_all_composite_charging_schedules(const int32_t duration_s) {

    std::map<int32_t, ChargingSchedule> charging_schedules;

    for (int connector_id = 0; connector_id < this->configuration->getNumberOfConnectors(); connector_id++) {
        const auto start_time = ocpp::DateTime();
        const auto duration = std::chrono::seconds(duration_s);
        const auto end_time = ocpp::DateTime(start_time.to_time_point() + duration);

        const auto valid_profiles =
            this->smart_charging_handler->get_valid_profiles(start_time, end_time, connector_id);
        const auto composite_schedule = this->smart_charging_handler->calculate_composite_schedule(
            valid_profiles, start_time, end_time, connector_id, ChargingRateUnit::A);
        charging_schedules[connector_id] = composite_schedule;
    }

    return charging_schedules;
}

DataTransferResponse ChargePoint::data_transfer(const CiString<255>& vendorId, const CiString<50>& messageId,
                                                const std::string& data) {
    DataTransferRequest req;
    req.vendorId = vendorId;
    req.messageId = messageId;
    req.data.emplace(data);

    DataTransferResponse response;
    ocpp::Call<DataTransferRequest> call(req, this->message_queue->createMessageId());

    auto data_transfer_future = this->send_async<DataTransferRequest>(call);

    auto enhanced_message = data_transfer_future.get();
    if (enhanced_message.messageType == MessageType::DataTransferResponse) {
        ocpp::CallResult<DataTransferResponse> call_result = enhanced_message.message;
        response = call_result.msg;
    }
    if (enhanced_message.offline) {
        // The charge point is offline or has a bad connection.
        response.status = DataTransferStatus::Rejected; // Rejected is not completely correct, but the
                                                        // best we have to indicate an error
    }

    return response;
}

void ChargePoint::register_data_transfer_callback(const CiString<255>& vendorId, const CiString<50>& messageId,
                                                  const std::function<void(const std::string data)>& callback) {
    std::lock_guard<std::mutex> lock(data_transfer_callbacks_mutex);
    this->data_transfer_callbacks[vendorId.get()][messageId.get()] = callback;
}

void ChargePoint::on_meter_values(int32_t connector, const Powermeter& power_meter) {
    // FIXME: fix power meter to also work with dc
    EVLOG_debug << "updating power meter for connector: " << connector;
    std::lock_guard<std::mutex> lock(power_meters_mutex);
    this->connectors.at(connector)->powermeter = power_meter;
}

void ChargePoint::on_max_current_offered(int32_t connector, int32_t max_current) {
    std::lock_guard<std::mutex> lock(power_meters_mutex);
    // TODO(kai): uses power meter mutex because the reading context is similar, think about storing
    // this information in a unified struct
    this->connectors.at(connector)->max_current_offered = max_current;
}

void ChargePoint::start_transaction(std::shared_ptr<Transaction> transaction) {

    StartTransactionRequest req;
    req.connectorId = transaction->get_connector();
    req.idTag = transaction->get_id_tag();
    req.meterStart = std::round(transaction->get_start_energy_wh()->energy_Wh);
    req.timestamp = transaction->get_start_energy_wh()->timestamp;
    const auto message_id = this->message_queue->createMessageId();
    ocpp::Call<StartTransactionRequest> call(req, message_id);

    if (transaction->get_reservation_id()) {
        call.msg.reservationId = transaction->get_reservation_id().value();
    }

    transaction->set_start_transaction_message_id(message_id.get());
    transaction->change_meter_values_sample_interval(this->configuration->getMeterValueSampleInterval());

    this->send<StartTransactionRequest>(call);
}

void ChargePoint::on_session_started(int32_t connector, const std::string& session_id, const std::string& reason) {

    EVLOG_debug << "Session on connector#" << connector << " started with reason " << reason;

    const auto session_started_reason = ocpp::conversions::string_to_session_started_reason(reason);

    // dont change to preparing when in reserved
    if ((this->status->get_state(connector) == ChargePointStatus::Reserved &&
         session_started_reason == SessionStartedReason::Authorized) ||
        this->status->get_state(connector) != ChargePointStatus::Reserved) {
        this->status->submit_event(connector, Event_UsageInitiated());
    }
}

void ChargePoint::on_session_stopped(const int32_t connector) {
    // TODO(piet) fix this when evse manager signals clearance of an error
    if (this->status->get_state(connector) == ChargePointStatus::Faulted) {
        this->status->submit_event(connector, Event_I1_ReturnToAvailable());
    } else if (this->status->get_state(connector) != ChargePointStatus::Reserved &&
               this->status->get_state(connector) != ChargePointStatus::Unavailable) {
        this->status->submit_event(connector, Event_BecomeAvailable());
    }
}

void ChargePoint::on_transaction_started(const int32_t& connector, const std::string& session_id,
                                         const std::string& id_token, const int32_t& meter_start,
                                         boost::optional<int32_t> reservation_id, const ocpp::DateTime& timestamp,
                                         boost::optional<std::string> signed_meter_value) {
    if (this->status->get_state(connector) == ChargePointStatus::Reserved) {
        this->status->submit_event(connector, Event_UsageInitiated());
    }

    auto meter_values_sample_timer = std::make_unique<Everest::SteadyTimer>(&this->io_service, [this, connector]() {
        auto meter_value = this->get_latest_meter_value(
            connector, this->configuration->getMeterValuesSampledDataVector(), ReadingContext::Sample_Periodic);
        this->transaction_handler->add_meter_value(connector, meter_value);
        this->send_meter_value(connector, meter_value);
    });
    meter_values_sample_timer->interval(std::chrono::seconds(this->configuration->getMeterValueSampleInterval()));
    std::shared_ptr<Transaction> transaction =
        std::make_shared<Transaction>(connector, session_id, CiString<20>(id_token), meter_start, reservation_id,
                                      timestamp, std::move(meter_values_sample_timer));
    if (signed_meter_value) {
        const auto meter_value =
            this->get_signed_meter_value(signed_meter_value.value(), ReadingContext::Transaction_Begin, timestamp);
        transaction->add_meter_value(meter_value);
    }

    this->database_handler->insert_transaction(session_id, transaction->get_transaction_id(), connector, id_token,
                                               timestamp.to_rfc3339(), meter_start, reservation_id);
    this->transaction_handler->add_transaction(transaction);
    this->connectors.at(connector)->transaction = transaction;

    this->start_transaction(transaction);
}

void ChargePoint::on_transaction_stopped(const int32_t connector, const std::string& session_id, const Reason& reason,
                                         ocpp::DateTime timestamp, float energy_wh_import,
                                         boost::optional<CiString<20>> id_tag_end,
                                         boost::optional<std::string> signed_meter_value) {
    if (signed_meter_value) {
        const auto meter_value =
            this->get_signed_meter_value(signed_meter_value.value(), ReadingContext::Transaction_End, timestamp);
        this->transaction_handler->get_transaction(connector)->add_meter_value(meter_value);
    }
    const auto stop_energy_wh = std::make_shared<StampedEnergyWh>(timestamp, energy_wh_import);
    this->transaction_handler->get_transaction(connector)->add_stop_energy_wh(stop_energy_wh);

    this->status->submit_event(connector, Event_TransactionStoppedAndUserActionRequired());
    this->stop_transaction(connector, reason, id_tag_end);
    this->database_handler->update_transaction(session_id, energy_wh_import, timestamp.to_rfc3339(), id_tag_end,
                                               reason);
    this->transaction_handler->remove_active_transaction(connector);
    this->smart_charging_handler->clear_all_profiles_with_filter(boost::none, connector, boost::none,
                                                                 ChargingProfilePurposeType::TxProfile, false);
}

void ChargePoint::stop_transaction(int32_t connector, Reason reason, boost::optional<CiString<20>> id_tag_end) {
    EVLOG_debug << "Called stop transaction with reason: " << conversions::reason_to_string(reason);
    StopTransactionRequest req;

    auto transaction = this->transaction_handler->get_transaction(connector);
    auto energyWhStamped = transaction->get_stop_energy_wh();

    if (reason == Reason::EVDisconnected) {
        // unlock connector
        if (this->configuration->getUnlockConnectorOnEVSideDisconnect() && this->unlock_connector_callback != nullptr) {
            this->unlock_connector_callback(connector);
        }
    }

    req.meterStop = std::round(energyWhStamped->energy_Wh);
    req.timestamp = energyWhStamped->timestamp;
    req.reason.emplace(reason);
    req.transactionId = transaction->get_transaction_id();

    if (id_tag_end) {
        req.idTag.emplace(id_tag_end.value());
    }

    std::vector<TransactionData> transaction_data_vec = transaction->get_transaction_data();
    if (!transaction_data_vec.empty()) {
        req.transactionData.emplace(transaction_data_vec);
    }

    auto message_id = this->message_queue->createMessageId();
    ocpp::Call<StopTransactionRequest> call(req, message_id);

    {
        std::lock_guard<std::mutex> lock(this->stop_transaction_mutex);
        this->send<StopTransactionRequest>(call);
    }

    transaction->set_finished();
    transaction->set_stop_transaction_message_id(message_id.get());
    this->transaction_handler->add_stopped_transaction(transaction->get_connector());
}

void ChargePoint::on_suspend_charging_ev(int32_t connector) {
    this->status->submit_event(connector, Event_PauseChargingEV());
}

void ChargePoint::on_suspend_charging_evse(int32_t connector) {
    this->status->submit_event(connector, Event_PauseChargingEVSE());
}

void ChargePoint::on_resume_charging(int32_t connector) {
    this->status->submit_event(connector, Event_StartCharging());
}

void ChargePoint::on_error(int32_t connector, const ChargePointErrorCode& error) {
    this->status->submit_event(connector, Event_FaultDetected(ChargePointErrorCode(error)));
}

void ChargePoint::on_log_status_notification(int32_t request_id, std::string log_status) {
    // request id of -1 indicates a diagnostics status notification, else log status notification
    if (request_id != -1) {
        this->log_status_notification(conversions::string_to_upload_log_status_enum_type(log_status), request_id);
    } else {
        // In OCPP enum DiagnosticsStatus it is called UploadFailed, in UploadLogStatusEnumType of Security Whitepaper
        // it is called UploadFailure
        if (log_status == "UploadFailure") {
            log_status = "UploadFailed";
        }
        this->diagnostic_status_notification(conversions::string_to_diagnostics_status(log_status));
    }
}

void ChargePoint::on_firmware_update_status_notification(int32_t request_id, std::string firmware_update_status) {
    if (request_id != -1) {
        this->signed_firmware_update_status_notification(
            conversions::string_to_firmware_status_enum_type(firmware_update_status), request_id);
    } else {
        this->firmware_status_notification(conversions::string_to_firmware_status(firmware_update_status));
    }
}

void ChargePoint::diagnostic_status_notification(DiagnosticsStatus status) {
    DiagnosticsStatusNotificationRequest req;
    req.status = status;
    this->diagnostics_status = status;

    ocpp::Call<DiagnosticsStatusNotificationRequest> call(req, this->message_queue->createMessageId());
    this->send_async<DiagnosticsStatusNotificationRequest>(call);
}

void ChargePoint::firmware_status_notification(FirmwareStatus status) {
    FirmwareStatusNotificationRequest req;
    req.status = status;
    this->firmware_status = status;

    ocpp::Call<FirmwareStatusNotificationRequest> call(req, this->message_queue->createMessageId());
    this->send_async<FirmwareStatusNotificationRequest>(call);
}

void ChargePoint::register_enable_evse_callback(const std::function<bool(int32_t connector)>& callback) {
    this->enable_evse_callback = callback;
}

void ChargePoint::register_disable_evse_callback(const std::function<bool(int32_t connector)>& callback) {
    this->disable_evse_callback = callback;
}

void ChargePoint::register_pause_charging_callback(const std::function<bool(int32_t connector)>& callback) {
    this->pause_charging_callback = callback;
}

void ChargePoint::register_resume_charging_callback(const std::function<bool(int32_t connector)>& callback) {
    this->resume_charging_callback = callback;
}

void ChargePoint::register_provide_token_callback(
    const std::function<void(const std::string& id_token, std::vector<int32_t> referenced_connectors,
                             bool prevalidated)>& callback) {
    this->provide_token_callback = callback;
}

void ChargePoint::register_stop_transaction_callback(
    const std::function<bool(int32_t connector, Reason reason)>& callback) {
    this->stop_transaction_callback = callback;
}

void ChargePoint::register_reserve_now_callback(
    const std::function<ReservationStatus(int32_t reservation_id, int32_t connector, ocpp::DateTime expiryDate,
                                          CiString<20> idTag, boost::optional<CiString<20>> parent_id)>& callback) {
    this->reserve_now_callback = callback;
}

void ChargePoint::register_cancel_reservation_callback(const std::function<bool(int32_t connector)>& callback) {
    this->cancel_reservation_callback = callback;
}

void ChargePoint::register_unlock_connector_callback(const std::function<bool(int32_t connector)>& callback) {
    this->unlock_connector_callback = callback;
}

void ChargePoint::register_set_max_current_callback(
    const std::function<bool(int32_t connector, double max_current)>& callback) {
    this->set_max_current_callback = callback;
}

void ChargePoint::register_is_reset_allowed_callback(const std::function<bool(const ResetType& reset_type)>& callback) {
    this->is_reset_allowed_callback = callback;
}

void ChargePoint::register_reset_callback(const std::function<void(const ResetType& reset_type)>& callback) {
    this->reset_callback = callback;
}

void ChargePoint::register_set_system_time_callback(
    const std::function<void(const std::string& system_time)>& callback) {
    this->set_system_time_callback = callback;
}

void ChargePoint::register_signal_set_charging_profiles_callback(const std::function<void()>& callback) {
    this->signal_set_charging_profiles_callback = callback;
}

void ChargePoint::register_upload_diagnostics_callback(
    const std::function<GetLogResponse(const GetDiagnosticsRequest& request)>& callback) {
    this->upload_diagnostics_callback = callback;
}

void ChargePoint::register_update_firmware_callback(
    const std::function<void(const UpdateFirmwareRequest msg)>& callback) {
    this->update_firmware_callback = callback;
}

void ChargePoint::register_signed_update_firmware_callback(
    const std::function<UpdateFirmwareStatusEnumType(const SignedUpdateFirmwareRequest msg)>& callback) {
    this->signed_update_firmware_callback = callback;
}

void ChargePoint::register_upload_logs_callback(const std::function<GetLogResponse(GetLogRequest req)>& callback) {
    this->upload_logs_callback = callback;
}

void ChargePoint::register_set_connection_timeout_callback(
    const std::function<void(int32_t connection_timeout)>& callback) {
    this->set_connection_timeout_callback = callback;
}

void ChargePoint::register_connection_state_changed_callback(const std::function<void(bool is_connected)>& callback) {
    this->connection_state_changed_callback = callback;
}

void ChargePoint::on_reservation_start(int32_t connector) {
    this->status->submit_event(connector, Event_ReserveConnector());
}

void ChargePoint::on_reservation_end(int32_t connector) {
    this->status->submit_event(connector, Event_BecomeAvailable());
}

} // namespace v16
} // namespace ocpp