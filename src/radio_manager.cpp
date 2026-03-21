#include "radio_manager.hpp"

RadioManager::RadioManager(Nrf24& radio)
    : radio_(radio)
{
}

bool RadioManager::boot(uint8_t channel)
{
    status_.state = RadioState::Boot;
    status_.channel = channel;
    status_.last_fault = 0;
    status_.last_tx_ok = false;
    status_.last_rx_len = 0;

    if (!radio_.probe()) {
        status_.state = RadioState::Fault;
        status_.last_fault = 1;
        return false;
    }

    if (!radio_.initDefaults(channel)) {
        status_.state = RadioState::Fault;
        status_.last_fault = 2;
        return false;
    }

    status_.last_status = radio_.getStatus();
    status_.state = RadioState::Standby;
    return true;
}

RadioStatus RadioManager::status() const
{
    return status_;
}

bool RadioManager::enterRx()
{
    if (radio_.startRx()) {
        status_.state = RadioState::RxListening;
        status_.last_status = radio_.getStatus();
        return true;
    }

    status_.state = RadioState::Fault;
    status_.last_fault = 1;
    return false;
}


bool RadioManager::leaveRx()
{
    if (radio_.stopRx()) {
        status_.state = RadioState::Standby;
        return true;
    }

    status_.state = RadioState::Fault;
    status_.last_fault = 2;
    return false;
}

bool RadioManager::sendPayload(const uint8_t* payload, size_t len)
{
    status_.state = RadioState::TxBusy;

    if (radio_.transmitOnce(payload, len)) {
        status_.last_tx_ok = true;
        status_.state = RadioState::Standby;
        return true;
    }

    status_.state = RadioState::Fault;
    status_.last_tx_ok = false;
    status_.last_fault = 3; 
    return false;
}

bool RadioManager::receivePayload(uint8_t* out, size_t capacity, size_t& outLen)
{
    outLen = 0;

    if (radio_.readOnePacket(out, capacity, outLen)) {
        status_.last_rx_len = outLen;
        status_.state = RadioState::RxListening;
        return true;
    }

    status_.state = RadioState::Fault;
    
    status_.last_fault = 4;
    return false;
}

bool RadioManager::sleep()
{
    if (radio_.powerDown()) {
        status_.state = RadioState::Sleep;
        return true;
    }

    status_.state = RadioState::Fault;
    status_.last_fault = 5;
    return false;
}

bool RadioManager::wake()
{
    if (radio_.powerUp()) {
        status_.state = RadioState::Standby;
        status_.last_status = radio_.getStatus();
        return true;
    }
    status_.state = RadioState::Fault;
    status_.last_fault = 7;
    return false;
}

bool RadioManager::powerDown()
{
    if (radio_.powerDown()) {
        status_.state = RadioState::PowerDown;
        return true;
    }

    status_.state = RadioState::Fault;
    status_.last_fault = 6;
    return false;
}

bool RadioManager::startCw(uint8_t channel, uint8_t rfPowerBits)
{
    if (radio_.startContinuousCarrier(channel, rfPowerBits)) {
        status_.channel = channel;
        status_.state = RadioState::CwTest;
        status_.last_status = radio_.getStatus();
        return true;
    }

    status_.state = RadioState::Fault;
    status_.last_fault = 8;
    return false;
}

bool RadioManager::stopCw()
{
    radio_.stopContinuousCarrier();
    status_.state = RadioState::Standby;
    return true;
}

const char* RadioManager::stateName(RadioState state)
{
    switch (state) {
        case RadioState::Boot:        return "Boot";
        case RadioState::Standby:     return "Standby";
        case RadioState::RxListening: return "RxListening";
        case RadioState::TxBusy:      return "TxBusy";
        case RadioState::Sleep:       return "Sleep";
        case RadioState::PowerDown:   return "PowerDown";
        case RadioState::CwTest:      return "CwTest";
        case RadioState::Fault:       return "Fault";
        default:                      return "Unknown";
    }
}