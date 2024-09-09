// SPDX-License-Identifier: Apache-2.0
// Copyright 2020 - 2024 Pionix GmbH and Contributors to EVerest

#include <cstdint>

namespace ocpp {

// Time
constexpr std::int32_t DAYS_PER_WEEK = 7;
constexpr std::int32_t HOURS_PER_DAY = 24;
constexpr std::int32_t SECONDS_PER_HOUR = 3600;
constexpr std::int32_t SECONDS_PER_DAY = 86400;

constexpr float DEFAULT_LIMIT_AMPS = 48.0;
constexpr float DEFAULT_LIMIT_WATTS = 33120.0;
constexpr std::int32_t DEFAULT_AND_MAX_NUMBER_PHASES = 3;
constexpr float LOW_VOLTAGE = 230;

constexpr float NO_LIMIT_SPECIFIED = -1.0;
constexpr std::int32_t NO_START_PERIOD = -1;
constexpr std::int32_t EVSEID_NOT_SET = -1;

} // namespace ocpp