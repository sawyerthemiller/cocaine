#pragma once

#include <cstdint>
#include <string>

bool initial_low_latency_pacing();
std::string session_pacing_path(const std::string& session);
uint32_t* create_session_pacing(const std::string& session, bool initial, std::string& error);
uint32_t* open_session_pacing(const std::string& session, bool writable, std::string& error);
