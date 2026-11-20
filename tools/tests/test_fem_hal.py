"""Compile the actual ESP32 FEM HAL against tracing GPIO/SPI adapters."""
import argparse
import os
from pathlib import Path
import subprocess

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--compiler',default=r'C:\msys64\ucrt64\bin\g++.exe')
    args=parser.parse_args()
    root=Path(__file__).resolve().parents[2]
    out=args.output.resolve()
    (out/'driver').mkdir(parents=True,exist_ok=True)
    gpio=r"""
#pragma once
#include <cassert>
#include <cstdint>
#include <map>
#include <vector>
using gpio_num_t=int;
using gpio_mode_t=int;
constexpr int GPIO_MODE_OUTPUT=1,GPIO_MODE_INPUT=0,GPIO_PULLUP_DISABLE=0,GPIO_PULLDOWN_DISABLE=0,GPIO_INTR_DISABLE=0;
struct gpio_config_t{uint64_t pin_bit_mask=0;int mode=0,pull_up_en=0,pull_down_en=0,intr_type=0;};
inline std::map<int,int> levels;
inline std::vector<int> touched;
inline void(*gpio_observer)()=nullptr;
inline int gpio_set_level(int pin,int value){
    if(pin==21 && levels[pin]!=value)assert(!levels[25]&&!levels[32]&&!levels[33]);
    levels[pin]=value;touched.push_back(pin);
    assert(!(levels[32]&&levels[33]));
    if(levels[25])assert(levels[26]&&(levels[32]||levels[33]));
    if(gpio_observer)gpio_observer();
    return 0;
}
inline int gpio_config(const gpio_config_t*){return 0;}
inline int gpio_set_direction(int,int){return 0;}
inline int gpio_get_level(int pin){return levels[pin];}
"""
    spi=r"""
#pragma once
#include <cstddef>
#include <cstdint>
using spi_host_device_t=int;
using spi_device_handle_t=void*;
constexpr int SPI3_HOST=3,SPI_DMA_CH_AUTO=0;
struct spi_bus_config_t{int sclk_io_num=0,mosi_io_num=0,miso_io_num=0,quadwp_io_num=0,quadhd_io_num=0;};
struct spi_device_interface_config_t{int clock_speed_hz=0,mode=0,spics_io_num=0,queue_size=0;};
struct spi_transaction_t{uint32_t length=0;const void* tx_buffer=nullptr;void* rx_buffer=nullptr;};
inline int spi_bus_initialize(int,const spi_bus_config_t*,int){return 0;}
inline int spi_bus_add_device(int,const spi_device_interface_config_t*,spi_device_handle_t* h){*h=reinterpret_cast<void*>(1);return 0;}
inline int spi_device_transmit(spi_device_handle_t,spi_transaction_t*){return 0;}
"""
    (out/'driver/gpio.h').write_text(gpio)
    (out/'driver/spi_master.h').write_text(spi)
    (out/'esp_err.h').write_text('#pragma once\n#include <cassert>\nconstexpr int ESP_OK=0;\n#define ESP_ERROR_CHECK(x) assert((x)==ESP_OK)\n')
    (out/'esp_rom_sys.h').write_text('#pragma once\n#include <cstdint>\ninline uint64_t elapsed=0;\ninline void esp_rom_delay_us(unsigned us){elapsed+=us;}\n')
    (out/'esp_timer.h').write_text('#pragma once\n#include "esp_rom_sys.h"\ninline int64_t esp_timer_get_time(){return elapsed;}\n')
    cpp=r"""
#include <algorithm>
#include <cassert>
#include <cstdio>
#include "esp32_nrf24_hal.hpp"
#include "host_status_protocol.hpp"
const Esp32Nrf24Hal* observed_hal=nullptr;
unsigned observations=0;
void observeSwitch(){
    assert(observed_hal->femMode()==nullptr);
    HostStatusProtocol::Snapshot status;
    status.fem_mode=observed_hal->femMode();
    assert(HostStatusProtocol::json(status).find("\"fem_mode\"")==std::string::npos);
    ++observations;
}
int main(){
    Esp32Nrf24Config cfg;
    Esp32Nrf24Hal hal(cfg);
    assert(hal.femAntenna()==1 && std::string(hal.femMode())=="off");
    observed_hal=&hal;
    for(int pin:{32,33,26,25,21})assert(levels[pin]==0);
    assert(hal.selectFemAntenna(2));
    assert(levels[21]==1 && hal.femAntenna()==2);
    assert(!hal.selectFemAntenna(0)&&!hal.selectFemAntenna(3));
    gpio_observer=&observeSwitch;
    hal.frontEnd(FrontEndMode::Tx);
    gpio_observer=nullptr;
    assert(observations==8 && std::string(hal.femMode())=="tx");
    assert(levels[32]&&levels[25]&&levels[26]&&!levels[33]&&levels[21]);
    assert(!hal.selectFemAntenna(1));
    hal.ce(true);assert(!hal.selectFemAntenna(1));
    assert(hal.setSignalOutputsEnabled(false));
    assert(!hal.ceAsserted()&&hal.femAntenna()==2 && std::string(hal.femMode())=="off");
    for(int pin:{32,33,26,25,21})assert(levels[pin]==0);
    assert(!hal.selectFemAntenna(1));
    hal.frontEnd(FrontEndMode::Rx);
    for(int pin:{32,33,26,25,21})assert(levels[pin]==0);
    assert(hal.setSignalOutputsEnabled(true));
    gpio_observer=&observeSwitch;
    hal.frontEnd(FrontEndMode::Rx);
    gpio_observer=nullptr;
    assert(std::string(hal.femMode())=="rx");
    assert(!levels[32]&&levels[33]&&levels[25]&&levels[26]&&levels[21]);
    gpio_observer=&observeSwitch;
    hal.frontEnd(FrontEndMode::Off);
    gpio_observer=nullptr;
    assert(std::string(hal.femMode())=="off");
    assert(!levels[32]&&!levels[33]&&!levels[25]&&!levels[26]&&levels[21]);
    assert(hal.selectFemAntenna(1)&&!levels[21]);
    for(int pin:{15,34,6,7,8,9,10,11})assert(std::find(touched.begin(),touched.end(),pin)==touched.end());
    HostStatusProtocol::Snapshot status;
    status.fem_antenna=2;
    for(const char* mode:{"off","rx","tx"}){
        status.fem_mode=mode;
        const auto encoded=HostStatusProtocol::json(status);
        assert(encoded.find(std::string("\"fem_mode\":\"")+mode+"\"")!=std::string::npos);
    }
    status.fem_mode=nullptr;assert(HostStatusProtocol::json(status).find("\"fem_mode\"")==std::string::npos);
    status.fem_mode="unknown";assert(HostStatusProtocol::json(status).find("\"fem_mode\"")==std::string::npos);
    std::puts("Production custom FEM HAL completed-mode and switching-unknown publication, serialization, boot/PA/LNA/OFF/ANT1/ANT2/idle guards/disabled antenna/cutoff/recovery/control-pin isolation trace PASS");
}
"""
    path=out/'fem_hal.cpp'
    path.write_text(cpp)
    binary=out/'fem_hal.exe'
    flags=['-DRF3_HARDWARE_PROFILE_ID=1','-DRF3_AUTOMATIC_FEM=1','-DNRF24_CE_PIN=17','-DNRF24_CSN_PIN=27','-DNRF24_IRQ_PIN=16','-DNRF24_SCK_PIN=18','-DNRF24_MOSI_PIN=23','-DNRF24_MISO_PIN=19','-DRF3_FEM_CTX_PIN=32','-DRF3_FEM_CRX_PIN=33','-DRF3_FEM_CSD_PIN=26','-DRF3_FEM_CPS_PIN=25','-DRF3_FEM_ANT_PIN=21']
    env=os.environ.copy()
    env['PATH']=str(Path(args.compiler).parent)+os.pathsep+env['PATH']
    subprocess.run([args.compiler,'-std=c++17','-Wall','-Wextra','-Werror','-pedantic',*flags,'-I',str(out),'-I',str(root/'include'),str(path),str(root/'src/esp32_nrf24_hal.cpp'),'-o',str(binary)],env=env,check=True)
    subprocess.run([str(binary)],env=env,check=True)
if __name__=='__main__':main()
