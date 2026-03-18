#include "radio_manager.hpp"

RadioManager::RadioManager(Nrf24& radio)
    : radio_(radio)
{
}

bool RadioManager::boot(uint8_t channel)
{
    status_.state = RadioState::Boot;
    status_.channel = channel;

    // TODO:
    // call radio_.probe()
    // call radio_.initDefaults(channel)
    // update status_
    return false;
}

RadioStatus RadioManager::status() const
{
    return status_;
}

bool RadioManager::enterRx()
{
    // TODO
    return false;
}

bool RadioManager::leaveRx()
{
    // TODO
    return false;
}

bool RadioManager::sendPayload(const uint8_t* payload, size_t len)
{
    (void)payload;
    (void)len;

    // TODO
    return false;
}

bool RadioManager::receivePayload(uint8_t* out, size_t capacity, size_t& outLen)
{
    (void)out;
    (void)capacity;

    outLen = 0;

    // TODO
    return false;
}

bool RadioManager::sleep()
{
    // TODO
    return false;
}

bool RadioManager::wake()
{
    // TODO
    return false;
}

bool RadioManager::powerDown()
{
    // TODO
    return false;
}

bool RadioManager::startCw(uint8_t channel, uint8_t rfPowerBits)
{
    (void)channel;
    (void)rfPowerBits;

    // TODO
    return false;
}

bool RadioManager::stopCw()
{
    // TODO
    return false;
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