#include "radio_manager.hpp"

// RadioManager keeps app-facing state in sync with the lower-level driver calls.
RadioManager::RadioManager(Nrf24& radio)
    : radio_(radio)
{
}

bool RadioManager::boot(uint8_t channel)
{
    // Reset the status snapshot before each boot attempt so any later fault code
    // clearly reflects the current bring-up path.
    status_.state = RadioState::Boot;
    status_.channel = channel;
    status_.last_fault = 0;
    status_.last_tx_ok = false;
    status_.last_rx_len = 0;

    if (!radio_.probe()) {
        status_.state = RadioState::Fault;
        status_.last_fault = 1;  // Serial Peripheral Interface (SPI) register probe failed.
        return false;
    }

    if (!radio_.initDefaults(channel)) {
        status_.state = RadioState::Fault;
        status_.last_fault = 2;  // Register initialization failed.
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
    status_.last_fault = 1;  // Could not enter receive (RX) mode.
    return false;
}


bool RadioManager::leaveRx()
{
    if (radio_.stopRx()) {
        status_.state = RadioState::Standby;
        status_.last_status = radio_.getStatus();
        return true;
    }

    status_.state = RadioState::Fault;
    status_.last_fault = 2;  // Could not leave receive (RX) mode cleanly.
    return false;
}

bool RadioManager::hasPendingRx()
{
    // Mirror the latest STATUS register back into the public status snapshot so
    // STATUS output remains informative even when no payload is read.
    status_.last_status = radio_.getStatus();
    return (status_.last_status & (1 << 6)) != 0;
}

bool RadioManager::sendPayload(const uint8_t* payload, size_t len)
{
    // Expose one-shot transmit (TX) as a simple state change plus the last
    // success/failure snapshot.
    status_.state = RadioState::TxBusy;

    if (radio_.transmitOnce(payload, len)) {
        status_.last_tx_ok = true;
        status_.last_status = radio_.getStatus();
        status_.state = RadioState::Standby;
        return true;
    }

    status_.state = RadioState::Fault;
    status_.last_tx_ok = false;
    status_.last_fault = 3;  // Transmit (TX) operation failed or timed out.
    return false;
}

bool RadioManager::receivePayload(uint8_t* out, size_t capacity, size_t& outLen)
{
    outLen = 0;

    // Successful reads keep the manager in receive (RX) mode so callers can
    // continue polling.
    if (radio_.readOnePacket(out, capacity, outLen)) {
        status_.last_rx_len = outLen;
        status_.last_status = radio_.getStatus();
        status_.state = RadioState::RxListening;
        return true;
    }

    status_.state = RadioState::Fault;
    status_.last_fault = 4;  // Receive (RX) read failed while data was expected.
    return false;
}

bool RadioManager::sleep()
{
    if (radio_.powerDown()) {
        status_.state = RadioState::Sleep;
        return true;
    }

    status_.state = RadioState::Fault;
    status_.last_fault = 5;  // Sleep transition failed.
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
    status_.last_fault = 7;  // Wake transition failed.
    return false;
}

bool RadioManager::powerDown()
{
    if (radio_.powerDown()) {
        status_.state = RadioState::PowerDown;
        return true;
    }

    status_.state = RadioState::Fault;
    status_.last_fault = 6;  // Explicit power-down failed.
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
    status_.last_fault = 8;  // Continuous-wave (CW) mode failed to start.
    return false;
}

bool RadioManager::stopCw()
{
    // stopContinuousCarrier does not currently report failure, so this wrapper
    // always returns true after restoring the app-facing state.
    radio_.stopContinuousCarrier();
    status_.state = RadioState::Standby;
    return true;
}

const char* RadioManager::stateName(RadioState state)
{
    // Keep the string names centralized so logs and the console always match.
    switch (state) {
        case RadioState::Boot: return "Boot";
        case RadioState::Standby: return "Standby";
        case RadioState::RxListening: return "RxListening";
        case RadioState::TxBusy: return "TxBusy";
        case RadioState::Sleep: return "Sleep";
        case RadioState::PowerDown: return "PowerDown";
        case RadioState::CwTest: return "CwTest";
        case RadioState::Fault: return "Fault";
        default: return "Unknown";
    }
}
